
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include "socket.h"
#include "utils.h"
#include "lowlevel.h"

// The wakeup the loop waits on is a UDP socket connected to its own loopback address, not a pipe. A pipe is
// cheaper per call (measured here: 0.54us against 1.44us for write+drain) but select() on Windows only ever
// reports sockets, and one code path for both platforms is worth more than that difference -- what matters
// is how often the wakeup is used, not what one call costs, and timer() below is where that is decided.

// 一圈最多收几帧再交给定时那一段。见 worker loop 里的说明。它同时是两个东西的旋钮：一圈的固定开销
// （select + 算期限 + 交给定时那一段）被几帧摊掉，以及上层多久能回一个 ack —— ack 报的是 recv_seq +
// 位图，一批一个就够，可这一批攒多大直接决定对端多久看到窗口动。
//
// 实测（本机回环，10s 每方向，同一个二进制只改这个数；参照：改架构之前 40.7 MB/s、C2 刚落地时 13.7）：
//     2  ->  42.7 MB/s       3  ->  28.8 MB/s       4  ->  24.1 MB/s
//     6  ->  19.1 MB/s      64  ->  10.3 MB/s
// 单调下降，而且是三倍的差距 —— 所以这里的主因是 **ack 节奏**，不是每圈的固定开销（否则越大越好）。
// 取 2：反馈永远不该成为瓶颈，而 ack 数仍比"一帧一个"少一半。链路 RTT 是真的（不像回环）时攒大一些
// 更省 ack，但那要另一个旋钮（按 RTT 或按时间自适应），不是现在这一步的事。
#define FFRDP_LOWLEVEL_RX_BATCH_MAX 2

typedef struct {
    int  (*send )(void *fl, uint64_t addr, uint8_t *buf, int len);
    int  (*timer)(void *fl, uint32_t deadline_ms);
    void (*exit )(void *fl);

    #define FLAG_BIND_ADDR (1 << 0)
    uint32_t           flags;
    struct sockaddr_in addr;
    int                sockfd;
    FFRDP_LOWLEVEL_CB  callback;
    void              *cbctx;
    MOD_THREAD         mthread;

    // The loop sleeps in select() with no timeout at all whenever nothing is due, and select() only ever
    // wakes for a descriptor -- never because a variable changed. So this socket is how "the deadline moved"
    // and "stop" reach it: any thread sends one byte to it, the loop watches it for reading.
    int             notify;    // UDP socket connected to its own loopback address (see ffrdp_udp_wakeup_create)
    int             asleep;    // 1 while the loop is inside select(); written and read under this lock
    pthread_mutex_t lock;      // guards deadline and asleep, which are written from other threads
    uint32_t        deadline;  // absolute ms on the get_timestamp32_ms() clock, 0 = nothing due
    // 2026-10-15（lo 结构性代价调查 ①）：只读统计 —— 这个 worker 一圈一圈到底在干什么 ✓。
    // 为什么要它：协议侧只看得见"圈数"（counter_upd_cnt ✓）与"收到多少帧"✗，分不出**空圈**
    // 是"被通知叫醒却没数据"✗ 还是"期限到点"✓ —— 而 lo 上 master 的疑点正是"≈2.2 圈/帧"✗
    // （参考实现 ≈1.05 ✓）⇒ 先把这个数拆开，再决定动不动 timer 的唤醒条件 ✓。纯计数，不改行为 ✗。
    uint32_t        stat_lap;         // 圈数：每次 select 归来算一圈 ✓
    uint32_t        stat_lap_frame;   // 其中收到 >=1 帧的圈数 ✓
    uint32_t        stat_lap_notify;  // 其中通知 fd 可读的圈数（跨线程 timer 叫醒 ✓）
    uint32_t        stat_lap_timeout; // 其中 select 返回 0（期限到点 ✓）
    uint32_t        stat_rx_frames;   // 收到的帧总数（直接用来算"帧/圈"✓）
    // 追加①：select 说"数据 fd 可读"、而 recvfrom 立刻失败的那一类空圈 ✗
    // （实测某一轮里空圈 60 万、吞吐同时掉下来 ✓，而通知只占 3 万 ⇒ 不是通知引起的 ✗，
    //  典型是套接字上的非数据可读事件 ✓，例如 ICMP 错误 ✓）⇒ 单独记账 + 记首次 errno ✓。
    uint32_t        stat_rx_fail;     // recvfrom <= 0 的次数（这些圈一个包都没读到 ✗）
    // 追加②：谁把期限 arm 成"已经到点"（<= now ✓）—— 这是上面那批 deadline 空圈的直接来源 ✗。
    // 分两类：跨线程（应用线程 send 后催一下 ✓）与循环内（协议 tick 返回下一个期限 ✓）。
    uint32_t        stat_arm_now_other;  // 跨线程 arm 成 <= now 的次数
    uint32_t        stat_arm_now_loop;   // 从循环自己线程 arm 成 <= now 的次数
    int             stat_rx_errno;       // 第一次失败的 errno（0 = 还没失败过 ✓）
    // 追加③（①-4）：把"空圈"再往下一层拆 ✓ —— 协议侧四个桶对不上总圈数（真机 2026-10-15：
    // 服务端 laps 695849 vs 桶和 248908 ✗，有 ~45 万圈在协议账上根本不存在 ✗）⇒ 肇事者不在
    // ffrdp_next_due 那一层 ⇒ 只能在这里量：睡下去时期限离现在多远 ✓、tick 进去时是什么情形 ✓。
    // 纯计数，不改行为 ✗。
    uint32_t        stat_left_le0;    // 睡下时期限已过期（left<0 ⇒ 实睡 0ms ✗ 自旋圈）
    uint32_t        stat_left_0;      // left == 0（0ms，但确实等到点了 ✓）
    uint32_t        stat_left_1_9;    // 1~9ms
    uint32_t        stat_left_ge10;   // >= 10ms（正常睡眠 ✓）
    uint32_t        stat_sleep_none;  // 睡下时没有期限（deadline 0 ⇒ 永久睡 ✓）
    uint32_t        stat_tick_fire;   // tick 真的到点并跑了回调 ✓
    uint32_t        stat_tick_none;   // tick 进去发现没期限（没有回调 ✗）
    uint32_t        stat_tick_early;  // tick 进去发现期限还没到（没有回调 ✗）
    // 2026-10-15（§26.30 步骤1-③）：收包侧 —— "一圈最多收 FFRDP_LOWLEVEL_RX_BATCH_MAX 帧"与
    // "每收一帧一次回调"到底值多少 ✓。为什么缺这个数：RX_BATCH_MAX=2 是吞吐旋钮（见文件头的实测表 ✓），
    // 但没有任何计数说它**真的挡在瓶颈上**（batch 被上限截断 = socket 里还有货 ✗），也没有数说一次回调
    // 值多少微秒 ✓；另外"每帧 sendto 次数"（v1.1 很可能一次发多帧 ✗，本架构是一帧一个数据报 ✓）
    // 也一直没量过 ✓。纯计数 ✓，不改行为 ✗。
    uint32_t        stat_rx_batch_full; // 一圈收满上限就停了（还没有证明后面真有货 ✗）
    uint32_t        stat_rx_capped_more; // 上面那批里，**零超时 select 说"还有可读"**的次数 ⇒ 真积压 ✓
    uint32_t        stat_lap_rx1;       // 这一圈只收 1 帧（没批起来 ✓）
    uint32_t        stat_lap_rxmulti;   // 这一圈收了 >= 2 帧（批起来了 ✓）
    uint32_t        stat_cb_us_sum;     // 收包回调耗时之和（微秒 ✓）
    uint32_t        stat_cb_us_max;     // 单次最长（微秒 ✓）
    uint32_t        stat_tx_calls;      // sendto 调用次数（本架构一帧一个数据报 ⇒ 与帧数比就是"每帧 sendto 次数"✓）
    uint32_t        stat_tx_bytes;      // 送出去的字节数 ✓
    // 2026-10-15（§26.30 第 2 步 ④-13）：一圈的**时间去向** ✓ + arm/wake 自己值多少 ✓。
    // 为什么缺这个数：①-12 只给了"回调值 4.3 µs/次"✗，而"一圈的固定开销 × 97k 圈/s"到底占多少
    // 一直没拆开 ✗ —— 这是"4.6 倍差距"剩下的两个候选之一 ✓（另一个是 ack 节奏 = ④-14 ✓）。
    // 采样口径：每 64 圈采一次 ✓（4 次时钟/采样圈 ⇒ 摊薄到全部圈上 ≈ 1.5 ns/圈 ✓，量不出代价 ✓）。
    // 三段互斥、和 = 整圈 ✓：pre = arm/算期限/select ✓、mid = 记账/notify/收包（含回调 ✓）、tick = 期限 + 协议圈 ✓。
    uint32_t        stat_cost_n;        // 采样到的圈数（分母 ✓）
    uint32_t        stat_cost_pre_us;   // 三段之和（微秒 ✓）
    uint32_t        stat_cost_mid_us;
    uint32_t        stat_cost_tick_us;
    uint32_t        stat_cost_all_us;   // 整圈（t0→t3 ✓）
    uint32_t        stat_cost_max_us;   // 采到的圈里最长的一个 ✓
    // ⚠️ 口径补严（④-13b）：收尾空闲圈（客户端已走、没有期限 ⇒ select 永久睡 ✓）会混进采样，
    //    实测一个这样的圈就是 **1.47 s** ✗ ⇒ 把"每圈均值"带偏 6 倍（和 §26.27b 那三次盲点同一类病 ✓）。
    //    单独把它们记出来 ⇒ 均值 = (all - slow_us) / (n - slow_n) ✓ 一眼能扣 ✓。
    uint32_t        stat_cost_slow_n;   // 采到的慢圈数（> 100 ms ✓）
    uint32_t        stat_cost_slow_us;  // 它们的时长之和 ✓
    uint32_t        stat_arm_n_loop;    // 循环内 arm 的次数（协议在回调里定下次期限 ✓）
    uint32_t        stat_arm_us_loop;   // 它们花掉的微秒 ✓
    uint32_t        stat_arm_n_other;   // 跨线程 arm 的次数（应用 send 后催一下 ✓）
    uint32_t        stat_arm_us_other;
} UDP_LOWLEVEL;

// 跨 API 的 addr 是 uint64：布局是两条层共同的约定（见 utils.h 的 ffrdp_addr_make），不是本文件的私事。
// 这里只剩 "sockaddr_in ↔ (ip, port)" 这层胶水（字节序转换）。
static uint64_t ffrdp_udp_addr_pack(const struct sockaddr_in *sin)
{
    return ffrdp_addr_make(ntohl(sin->sin_addr.s_addr), ntohs(sin->sin_port));
}

static void ffrdp_udp_addr_unpack(uint64_t addr, struct sockaddr_in *sin)
{
    uint32_t ip;
    uint16_t port;

    ffrdp_addr_split(addr, &ip, &port);
    memset(sin, 0, sizeof(*sin));
    sin->sin_family      = AF_INET;
    sin->sin_addr.s_addr = htonl(ip);
    sin->sin_port        = htons(port);
}

// The wakeup socket: bound to a loopback port and then connected to its own address, so that sending one
// byte to it makes it readable here. Built and closed by the worker thread -- the thread where the socket
// layer is up. Loopback delivery is immediate (measured: select() sees it on the very next call, 1000 times
// out of 1000), and connect() means nothing else can put a byte in it.
static int ffrdp_udp_wakeup_create(UDP_LOWLEVEL *udp)
{
    struct sockaddr_in lo  = {};
    socklen_t          len = sizeof(lo);
    int                fd  = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return -1;
    socket_setopt(fd, "nonblock_io", 1);   // a caller must never block on a full queue
    lo.sin_family      = AF_INET;
    lo.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    lo.sin_port        = 0;                // the kernel picks; nobody else ever needs to know it
    if (bind(fd, (struct sockaddr*)&lo, sizeof(lo)) != 0) { closesocket(fd); return -1; }
    if (getsockname(fd, (struct sockaddr*)&lo, &len) != 0) { closesocket(fd); return -1; }
    if (connect(fd, (struct sockaddr*)&lo, sizeof(lo)) != 0) { closesocket(fd); return -1; }
    udp->notify = fd;
    return 0;
}

// Wake the loop. Any thread may call this; it is the only thing in this file that has to be safe to call
// while the loop is asleep. A full queue means a wakeup is already pending, which is all this needs.
static void ffrdp_udp_notify(UDP_LOWLEVEL *udp)
{
    char b = 1;
    int  n;
    if (udp->notify < 0) return;
    // A full queue is not an error: it means a wakeup is already pending, which is all this is for. The
    // result goes into a variable rather than being cast away, because send() is warn_unused_result.
    n = (int)send(udp->notify, &b, 1, 0);
    (void)n;
}

static void ffrdp_udp_notify_drain(UDP_LOWLEVEL *udp)
{
    char b[64];
    // It has to be drained completely: a byte left behind keeps select() reporting it readable, and the
    // loop then spins at full speed instead of sleeping.
    if (udp->notify < 0) return;
    while (recv(udp->notify, b, sizeof(b), 0) > 0) { }
}

// "I am about to sleep" and "I am awake", both under the lock that also guards the deadline, so that
// timer() can tell "the loop will re-read the deadline anyway" from "the loop has to be woken". The flag
// goes up BEFORE the deadline is read: a timer() that lands in between sees asleep=1 and sends its byte,
// and because that byte is buffered, the select() that follows returns at once instead of sleeping on the
// old, later deadline. A wakeup cannot be lost here the way a condition variable's signal can.
static uint32_t ffrdp_udp_sleep_begin(UDP_LOWLEVEL *udp)
{
    uint32_t d;
    pthread_mutex_lock(&udp->lock);
    udp->asleep = 1;
    d = udp->deadline;
    pthread_mutex_unlock(&udp->lock);
    return d;
}

static void ffrdp_udp_sleep_end(UDP_LOWLEVEL *udp)
{
    pthread_mutex_lock(&udp->lock);
    udp->asleep = 0;
    pthread_mutex_unlock(&udp->lock);
}

// §26.30 ①-③ / ④-13 用的微秒时钟：量"每收一帧一次回调"与"一圈三段"都要它 ✓
//（与 ffrdp.c 里量 ack 延迟那个同源 ✓）。放在 timer() 之前：④-13 的 arm 计时也要用它 ✓。
static uint64_t ffrdp_udp_us(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)(ts.tv_nsec / 1000);
}

// Deliver a tick that has come due. The deadline is consumed BEFORE the callback runs, so a callback that
// arms its next one is not overwritten by this, and a callback that arms nothing stops the clock rather
// than being called again on every lap.
static void ffrdp_udp_tick(UDP_LOWLEVEL *udp)
{
    uint32_t now = get_timestamp32_ms();
    pthread_mutex_lock(&udp->lock);
    if (!udp->deadline || (int32_t)(udp->deadline - now) > 0) {
        if (!udp->deadline) udp->stat_tick_none++; else udp->stat_tick_early++;   // 只读（①-4）
        pthread_mutex_unlock(&udp->lock); return;
    }
    udp->stat_tick_fire++;                                                        // 只读（①-4）
    udp->deadline = 0;
    pthread_mutex_unlock(&udp->lock);
    if (udp->callback) udp->callback(udp->cbctx, FFRDP_LOWLEVEL_TYPE_TIMER, NULL, 0);
}

// One timer request, from any thread: remember when it is due, and wake the loop only when this call is
// the one that has to. The value is always stored; only the wakeup is conditional. Three kinds of wasted
// wakeup are filtered out here, and this is where the cost of the layer actually lives:
//   - the loop itself is the caller (the protocol arms its next tick from inside a callback): it re-reads
//     the deadline before it next sleeps, so there is nobody to wake;
//   - the new deadline is not earlier than the one already armed: the loop wakes at the earlier one anyway
//     and re-reads the deadline then;
//   - the loop is not asleep: it is on its way back to the top, where it reads the deadline again.
static int ffrdp_udp_lowlevel_timer(void *fl, uint32_t deadline_ms)
{
    UDP_LOWLEVEL *udp = (UDP_LOWLEVEL *)fl;
    uint32_t      old;
    int           wake;
    if (!udp) return -1;
    pthread_mutex_lock(&udp->lock);
    old           = udp->deadline;
    udp->deadline = deadline_ms;
    wake          = (old == 0) || ((int32_t)(deadline_ms - old) < 0) || udp->asleep;
    pthread_mutex_unlock(&udp->lock);
    // The same "am I the loop?" test exit() uses: a callback is running on the loop's own thread.
    {   // 只读记账（①）："arm 成已到点"分两类 ✓
        int from_loop = (udp->mthread.hthread && udp->mthread.hthread == (void*)pthread_self());
        uint64_t t_arm0 = ffrdp_udp_us();                                  // 只读（④-13）：arm/wake 自己值多少 ✓
        if ((int32_t)(deadline_ms - get_timestamp32_ms()) <= 0) {
            if (from_loop) udp->stat_arm_now_loop++; else udp->stat_arm_now_other++;
        }
        if (wake && !from_loop) ffrdp_udp_notify(udp);
        {   uint32_t d_arm = (uint32_t)(ffrdp_udp_us() - t_arm0);
            if (from_loop) { udp->stat_arm_n_loop++;  udp->stat_arm_us_loop  += d_arm; }
            else           { udp->stat_arm_n_other++; udp->stat_arm_us_other += d_arm; }
        }
    }
    return 0;
}

static void* ffrdp_udp_lowlevel_proc(void *arg)
{
    UDP_LOWLEVEL *udp = arg;
    int ret;

    thread_setname("ffrdp-udp-lowlvl");
    socket_init();

    while (!module_thread_check_exit(&udp->mthread)) {
        if (module_thread_check_pause(&udp->mthread)) { usleep(100 * 1000); continue; }
        // 唤醒 socket 也由这个线程建/关：它和本线程的 socket_init()/socket_exit() 同一层，所以 init/exit
        // 不必各自再管一次平台的 socket 层。建不出来就下一圈再试，和数据 socket 一样。
        if (udp->notify < 0 && ffrdp_udp_wakeup_create(udp) != 0) {
            printf("<2> ffrdp-udp failed to create the wakeup socket\n");
            usleep(100 * 1000); continue;
        }
        if (udp->sockfd < 0) {
            udp->sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            socket_setopt(udp->sockfd, "reuse_addr" , 1); // enable reuse addr
            // non blocking, the same contract this project's own socket has: a frame that cannot be handed
            // to the kernel must come back as an error to the caller, not as a stall in a receive loop.
            socket_setopt(udp->sockfd, "nonblock_io", 1);
            socket_setopt(udp->sockfd, "send_buffer", FFRDP_LOWLEVEL_BUFFER_SIZE);
            socket_setopt(udp->sockfd, "recv_buffer", FFRDP_LOWLEVEL_BUFFER_SIZE);
            if (udp->sockfd < 0) { printf("<2> ffrdp-udp failed to create socket !\n"); usleep(100 * 1000); continue; }
            if ((udp->flags & FLAG_BIND_ADDR) && bind(udp->sockfd, (struct sockaddr*)&udp->addr, sizeof(udp->addr)) < 0) {
                printf("<2> udpproxy failed to bind socket to port %d !\n", ntohs(udp->addr.sin_port));
                closesocket(udp->sockfd); udp->sockfd = -1; usleep(100 * 1000); continue;
            }
        }

        // 只读（§26.30 ④-13）：一圈三段计时 —— 每 64 圈采一次 ✓（代价量不出 ✓，见字段注释）
        int      cost  = ((udp->stat_lap & 63) == 0);
        uint64_t t_c0 = 0, t_c1 = 0, t_c2 = 0, t_c3 = 0;
        if (cost) t_c0 = ffrdp_udp_us();
        // Sleep until a frame arrives, the deadline passes, or someone wakes us -- and when there is no
        // deadline at all, sleep indefinitely. The old fixed 100ms wakeup both capped what precision the
        // layer above could ask for and kept an idle endpoint waking ten times a second.
        struct timeval tv, *tvp = NULL;
        uint32_t dl = ffrdp_udp_sleep_begin(udp);
        if (dl) {
            int32_t left = (int32_t)(dl - get_timestamp32_ms());
            // 只读（①-4）：left<0 就是"实睡 0ms"的那种自旋圈 ✗（select 返回 0 ⇒ 又是一圈）
            if (left < 0)       udp->stat_left_le0 ++;
            else if (left == 0) udp->stat_left_0   ++;
            else if (left <= 9) udp->stat_left_1_9 ++;
            else                udp->stat_left_ge10++;
            if (left < 0) left = 0;                    // due already: come straight back
            tv.tv_sec  = left / 1000;
            tv.tv_usec = (left % 1000) * 1000;
            tvp = &tv;
        } else {
            udp->stat_sleep_none++;                    // 只读（①-4）：没有期限 ⇒ 永久睡 ✓
        }
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(udp->sockfd, &rfds);
        FD_SET(udp->notify, &rfds);
        ret = select(MAX(udp->sockfd, udp->notify) + 1, &rfds, NULL, NULL, tvp);
        ffrdp_udp_sleep_end(udp);       // awake again, whatever select() answered
        if (cost) t_c1 = ffrdp_udp_us();                                              // 只读（④-13）：pre 段结束 ✓
        if (ret == -1) {
            if (socket_errno() == EINTR) continue;     // a signal is not an error, and not a reason to sleep
            printf("select error !\n"); fflush(stdout); usleep(100 * 1000); continue;
        }
        // 只读记账（①）：一圈一笔 ✓
        udp->stat_lap++;
        if (ret == 0) udp->stat_lap_timeout++;
        if (ret > 0 && FD_ISSET(udp->notify, &rfds)) udp->stat_lap_notify++;

        if (ret > 0 && FD_ISSET(udp->notify, &rfds)) ffrdp_udp_notify_drain(udp);
        // No stop test here, nor in the receive loop below: the condition at the top is enough, because every
        // lap is bounded -- that loop stops after FFRDP_LOWLEVEL_RX_BATCH_MAX frames, the socket is non
        // blocking, and select() has a deadline or is woken by the socket above. A stop that lands mid lap is
        // seen one lap later (one more tick, two more frames at most); ffrdp_free() joins us before letting go
        // of anything these callbacks touch.
        if (ret > 0 && FD_ISSET(udp->sockfd, &rfds)) {
            // 一圈把当前就绪的包都收下来（有上限），再交给定时那一段。为什么要收完一圈：上层是"一段
            // 一个 ack"——它报的是 recv_seq + 位图，第二个 ack 覆盖的帧第一个已经报过了，什么都不多带；
            // 而一圈只收一帧就等于一帧一个 ack，实测（差分仿真）是攒批行为的 6.7 倍。上限是给定了的：
            // 一个不停把 socket 灌满的发送方不该把下面的定时工作（重传、探测、速率采样）饿住。
            int frames = 0;
            while (frames < FFRDP_LOWLEVEL_RX_BATCH_MAX) {
                uint8_t buf[FFRDP_LOWLEVEL_FRAME_MAX];
                struct  sockaddr_in from;
                socklen_t fromlen = sizeof(from);
                // (void *): Winsock declares recvfrom's buffer as char*, POSIX as void* -- the buffer is
                // bytes either way, and the cast is what keeps -Wall -Werror clean on both.
                ret = recvfrom(udp->sockfd, (void*)buf, sizeof(buf), 0, (struct sockaddr*)&from, &fromlen);
                if (ret <= 0) {        // zero length datagram, an empty socket, or a socket error
                    udp->stat_rx_fail++;
                    if (udp->stat_rx_errno == 0) udp->stat_rx_errno = socket_errno();
                    break;
                }
                frames++; udp->stat_rx_frames++; if (frames == 1) udp->stat_lap_frame++;
                if (udp->callback) {
                    // The message carries where the frame came from, so a reply goes straight back out through
                    // send() with the very same addr -- that is the only reason this layer reports an address
                    // at all. buf points at the buffer above: it is a stack buffer reused by the next lap, so
                    // it is valid for exactly the duration of this call and the callback has to copy what it
                    // wants to keep.
                    FFRDP_LOWLEVEL_MSG msg;
                    msg.addr = ffrdp_udp_addr_pack(&from);
                    msg.buf  = buf;
                    msg.len  = ret;
                    {   // 只读（§26.30 ①-③）：给这一次回调标价 ✓（两次 clock_gettime，量的是协议侧一段 ✓）
                        uint64_t t_cb = ffrdp_udp_us();
                        udp->callback(udp->cbctx, FFRDP_LOWLEVEL_TYPE_RECV, &msg, sizeof(msg));
                        {   uint32_t dt = (uint32_t)(ffrdp_udp_us() - t_cb);
                            udp->stat_cb_us_sum += dt;
                            if (dt > udp->stat_cb_us_max) udp->stat_cb_us_max = dt;
                        }
                    }
                }
            }
            // 只读（§26.30 ①-③）：这一圈是"被上限截断"还是"socket 真收空了"✗
            // ⚠️ 光数"收满 2 帧就停"**证不出**积压 ✗（可能刚巧就是 2 帧 ✓）⇒ 收满时再问一次
            //    零超时 select ✓：它能读 ⇒ socket 里真的还有货 = 这一圈是被上限截断的 ✓。
            //    不消费任何数据报 ✓（不像再 recvfrom 一次 ✗ —— 那会把下一圈要的帧吃掉 ✓）。
            if (frames >= FFRDP_LOWLEVEL_RX_BATCH_MAX) {
                udp->stat_rx_batch_full++;
                {   fd_set pf; struct timeval z; int pr;
                    FD_ZERO(&pf); FD_SET(udp->sockfd, &pf);
                    z.tv_sec = 0; z.tv_usec = 0;
                    pr = select(udp->sockfd + 1, &pf, NULL, NULL, &z);
                    if (pr > 0 && FD_ISSET(udp->sockfd, &pf)) udp->stat_rx_capped_more++;
                }
            }
            if (frames == 1) udp->stat_lap_rx1++; else if (frames >= 2) udp->stat_lap_rxmulti++;
        }
        if (cost) t_c2 = ffrdp_udp_us();                                              // 只读（④-13）：mid 段结束 ✓
        // Received work first, then the clock, so the two are never handed over interleaved. A tick armed
        // from inside a receive callback is therefore delivered in the same lap rather than the next one.
        ffrdp_udp_tick(udp);
        if (cost) {                                                                   // 只读（④-13）：tick 段结束 ⇒ 记账 ✓
            t_c3 = ffrdp_udp_us();
            udp->stat_cost_n++;
            udp->stat_cost_pre_us  += (uint32_t)(t_c1 - t_c0);
            udp->stat_cost_mid_us  += (uint32_t)(t_c2 - t_c1);
            udp->stat_cost_tick_us += (uint32_t)(t_c3 - t_c2);
            udp->stat_cost_all_us  += (uint32_t)(t_c3 - t_c0);
            if ((uint32_t)(t_c3 - t_c0) > udp->stat_cost_max_us) udp->stat_cost_max_us = (uint32_t)(t_c3 - t_c0);
            if ((t_c3 - t_c0) > 100000) {   // > 100 ms ⇒ 慢圈（收尾空闲 ✓）：单独记，别混进均值 ✓
                udp->stat_cost_slow_n++;
                udp->stat_cost_slow_us += (uint32_t)(t_c3 - t_c0);
            }
        }
    }

    // ① 的读出点：这一行 ASCII，任何控制台都不乱码 ✓。空圈 = 总圈 - 带帧圈 ✓。
    printf("udp lowlevel laps: %u total, %u with frames, %u notify-woken, %u deadline, %u empty, %u frames, %u rx-fail (errno %d)\n",
           udp->stat_lap, udp->stat_lap_frame, udp->stat_lap_notify, udp->stat_lap_timeout,
           udp->stat_lap - udp->stat_lap_frame, udp->stat_rx_frames,
           udp->stat_rx_fail, udp->stat_rx_errno);
    printf("udp lowlevel arm-now: %u from other threads, %u from the loop itself\n",
           udp->stat_arm_now_other, udp->stat_arm_now_loop);
    // ①-4：空圈往下一层拆 —— 睡下时的 left 分布 + tick 三分类 ✓
    printf("udp lowlevel sleep-left: expired %u, 0ms %u, 1-9ms %u, >=10ms %u, no-deadline %u\n",
           udp->stat_left_le0, udp->stat_left_0, udp->stat_left_1_9, udp->stat_left_ge10, udp->stat_sleep_none);
    printf("udp lowlevel tick kind: fire %u, none %u, early %u (the two 'none/early' are laps that ran no callback)\n",
           udp->stat_tick_fire, udp->stat_tick_none, udp->stat_tick_early);
    // ①-③（§26.30 步骤1）：收包侧代价 —— 批有没有被上限截断 ✓、一次回调多少微秒 ✓、送出多少个数据报 ✓
    printf("udp lowlevel rx cost: batch-full %u, of which socket still had data %u, laps with 1 frame %u, with >=2 frames %u, callback %u us total, %u us max\n",
           udp->stat_rx_batch_full, udp->stat_rx_capped_more, udp->stat_lap_rx1, udp->stat_lap_rxmulti,
           udp->stat_cb_us_sum, udp->stat_cb_us_max);
    printf("udp lowlevel tx      : sendto %u calls, %u bytes (per frame = calls / frames in the ffrdp block)\n",
           udp->stat_tx_calls, udp->stat_tx_bytes);
    // ④-13（§26.30 第 2 步）：一圈的时间去向（采样 1/64 ✓；三段互斥、和 = 整圈 ✓）—— 除以采样圈数就是每圈 ✓
    printf("udp lowlevel lap cost: %u sampled laps, pre(arm+sleep+select) %u us, mid(book+notify+rx) %u us, tick(deadline+proto) %u us, all %u us, max %u us\n",
           udp->stat_cost_n, udp->stat_cost_pre_us, udp->stat_cost_mid_us, udp->stat_cost_tick_us,
           udp->stat_cost_all_us, udp->stat_cost_max_us);
    // ④-13b：扣掉慢圈才是"每圈" ✓（每圈 = (all - slow_us) / (n - slow_n) ✓）
    printf("udp lowlevel lap cost slow (>100ms): %u laps, %u us  => per lap = (all - slow) / (n - slow)\n",
           udp->stat_cost_slow_n, udp->stat_cost_slow_us);
    printf("udp lowlevel arm cost: from loop %u calls %u us, from other threads %u calls %u us\n",
           udp->stat_arm_n_loop, udp->stat_arm_us_loop, udp->stat_arm_n_other, udp->stat_arm_us_other);
    closesocket(udp->sockfd); udp->sockfd = -1;
    {   int fd = udp->notify;
        udp->notify = -1;              // 先摘引用、再关：晚到的 timer() 只会看到"没有"，不会发到已关的 fd 上
        if (fd >= 0) closesocket(fd);
    }
    socket_exit();
    return NULL;
}

static int ffrdp_udp_lowlevel_send(void *fl, uint64_t addr, uint8_t *buf, int len)
{
    UDP_LOWLEVEL *udp = (UDP_LOWLEVEL *)fl;
    struct sockaddr_in to;
    if (!udp || !buf || len <= 0 || udp->sockfd < 0) return -1;
    ffrdp_udp_addr_unpack(addr, &to);
    int ret = (int)sendto(udp->sockfd, (void*)buf, len, 0, (struct sockaddr*)&to, sizeof(to));
    // 只读（§26.30 ①-③）：每帧 sendto 次数 —— 本架构是"一帧一个数据报"✓，这个调用数就是分子 ✓
    // （分母是 ffrdp 那一块里的帧数：首发+两种重传 ✓）。ack / query 也走这里 ✓，所以它 >= 帧数 ✓。
    udp->stat_tx_calls++;
    if (ret > 0) udp->stat_tx_bytes += (uint32_t)ret;
    if (ret == len) return 0;
    // The socket is non blocking, so a full send buffer is a normal answer on a busy link and is not worth
    // a line of output on every frame; the caller decides what to do with the failure. Anything else is a
    // real error and is worth naming once.
    if (socket_errno() != EWOULDBLOCK) {
        printf("<3> ffrdp-udp sendto %s:%d failed, ret: %d, errno: %d\n",
               inet_ntoa(to.sin_addr), ntohs(to.sin_port), ret, socket_errno());
    }
    return -1;
}

static void ffrdp_udp_lowlevel_exit(void *fl)
{
    UDP_LOWLEVEL *udp = (UDP_LOWLEVEL *)fl;
    if (!udp) return;
    // The flag goes up BEFORE the wakeup, and the order is the whole point. With no deadline set the thread
    // sleeps in select() with no timeout, so it has to be woken -- but a wakeup sent first can be consumed
    // by a thread that is between laps: it drains the byte, sees no stop flag yet, and goes straight back
    // to sleep for ever, while this side sits in the join. Both have to happen, flag first, or the join is
    // a coin flip. (Setting the flag here is not a shortcut around the state call below: state 0 does the
    // same thing to the same field and then joins, so this only makes the order explicit.)
    // 从工作线程里调用 exit 是"join 自己"：状态 0 会一直等一个只有它自己能走完的退出，于是整个进程
    // 卡死。契约上禁止这样做（见 lowlevel.h），这里再把代价压成"这一轮之后自然退出"而不是挂住。
    if (udp->mthread.hthread && udp->mthread.hthread == (void*)pthread_self()) {
        udp->mthread.flags |= MOD_THREAD_FLAG_STOP;
        return;
    }
    udp->mthread.flags |= MOD_THREAD_FLAG_STOP;
    ffrdp_udp_notify(udp);
    // Then stop it and wait for it: that thread owns the socket and dereferences this handle, so freeing
    // the handle while the thread is still running would leave the thread reading freed memory. The thread
    // closes the socket on its way out.
    module_thread_set_state(&udp->mthread, 0);
    // 唤醒 socket 已由工作线程在退出前关掉（并把引用摘成 -1）；socket_init()/socket_exit() 也都发生在
    // 那个线程里，所以这里没有 socket 层的收尾要做。
    pthread_mutex_destroy(&udp->lock);
    free(udp);
}

// params: the one form lowlevel.h documents, "bind:127.0.0.1:1234". It is only recorded here: the receive
// thread does the binding, so a port that is busy right now is retried instead of failing the whole start.
// A malformed "bind:" is a configuration mistake worth refusing outright; anything else is taken as "no
// bind requested" with a line of output, which keeps a send-only endpoint usable.
static int ffrdp_udp_lowlevel_parse_params(UDP_LOWLEVEL *udp, char *params)
{
    char    *rest;
    uint32_t ip;
    int      port;

    // "none": 明确表示"这个端点不绑定"（客户端就是这样：源端口由内核在第一次发包时定）。它是一条正常
    // 的配置，不是笔误，所以不打印、也没有 flags 位 —— 没有 FLAG_BIND_ADDR，接收线程就不去 bind。
    if (strcmp(params, "none") == 0) return 0;
    if (strncmp(params, "bind:", 5) != 0) {
        printf("<3> ffrdp-udp params \"%s\" is not \"bind:ip:port\" or \"none\", no bind address is set\n", params);
        return 0;
    }
    rest = params + 5;
    // 解析走共享约定（协议层解析对端用的是同一个函数）；default_port < 0 ⇒ 端口必须显式给。
    if (ffrdp_addr_parse_text(rest, &ip, &port, -1) != 0) {
        printf("<2> ffrdp-udp params \"%s\" needs an ip:port after \"bind:\" (dotted quad, port 0..65535)\n", params);
        return -1;
    }
    memset(&udp->addr, 0, sizeof(udp->addr));
    udp->addr.sin_family      = AF_INET;
    udp->addr.sin_port        = htons((uint16_t)port);
    udp->addr.sin_addr.s_addr = htonl(ip);
    udp->flags |= FLAG_BIND_ADDR;
    return 0;
}

FFRDP_LOWLEVEL* ffrdp_udp_lowlevel_init(void *params, FFRDP_LOWLEVEL_CB callback, void *cbctx)
{
    if (!params) return NULL;
    UDP_LOWLEVEL *udp = (UDP_LOWLEVEL *)calloc(1, sizeof(UDP_LOWLEVEL));
    if (!udp) return NULL;
    if (ffrdp_udp_lowlevel_parse_params(udp, params) != 0) { free(udp); return NULL; }

    udp->send         = ffrdp_udp_lowlevel_send;
    udp->timer        = ffrdp_udp_lowlevel_timer;
    udp->exit         = ffrdp_udp_lowlevel_exit;
    udp->sockfd       = -1;   // 0 is a valid descriptor, so "no socket yet" cannot be spelled with a zero
    udp->notify       = -1;
    udp->callback     = callback;
    udp->cbctx        = cbctx;
    udp->mthread.proc = ffrdp_udp_lowlevel_proc;
    udp->mthread.arg  = udp;
    // The wakeup socket is not created here: it belongs to the worker thread, which is also the thread that
    // has the platform's socket layer up (socket_init()/socket_exit() live there, and that thread needs the
    // same layer for its own socket anyway). A timer() that arrives before that socket exists is not lost:
    // the deadline is already recorded under the lock, and the worker reads it before it ever sleeps.
    pthread_mutex_init(&udp->lock, NULL);
    // State 1 starts the receive thread: it is the whole point of this layer, and there is no separate
    // start call in the API, so the handle the caller gets back is already listening. State 0 in exit()
    // stops and joins it again.
    module_thread_set_state(&udp->mthread, 1);
    return (FFRDP_LOWLEVEL*)udp;
}
