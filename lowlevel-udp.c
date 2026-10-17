
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include "socket.h"
#include "utils.h"
#include "lowlevel.h"

// ── 诊断开关：与协议层的 FFRDP_DIAG 同口径 ✓（出货不传 ⇒ 本节统计/计时/收尾打印整块不存在 ✓）——
// 省常驻内存 ✓，并去掉每次 timer() 与采样圈里的那几次 clock_gettime ✓。
#ifndef FFRDP_DIAG
#define FFRDP_DIAG 0
#endif

#if FFRDP_DIAG
#define LSTAT(u, f)         ((u)->f++)
#define LSTAT_ADD(u, f, v)  ((u)->f += (uint32_t)(v))
#define LSTAT_SET(u, f, v)  ((u)->f = (v))
#else
#define LSTAT(u, f)         ((void)(u))
#define LSTAT_ADD(u, f, v)  ((void)(u), (void)(v))
#define LSTAT_SET(u, f, v)  ((void)(u), (void)(v))
#endif
// 只有"不门控的写点"才用这三个宏 ✓；`#if FFRDP_DIAG` 块内部直接写 `udp->stat_x++` ✓（那个块只在 dev 编译 ✓）。

// 唤醒用 UDP socket 而不是 pipe：select() 在 Windows 上只报 socket ⇒ 一条路径更值 ✓（pipe 单次更便宜，但那不是瓶颈 ✓）。

// 一圈最多收几帧（= FFRDP_LOWLEVEL_RX_BATCH_MAX ✓）；取 4 的依据：①-33 在"缓冲膝点 + 多判守卫都就位"之后**重扫** 2/4/8/16
// ⇒ 4 最好、吞吐 +6%（相对 2 ✓）、**零内存** ✓（notes §26.53 ✓）。
// ⚠️ 旧注释"批越大越差、主因是 ack 节奏"已经过期 ✗：那是在**缓冲被夹 + 守卫缺失**的区间里量的，
//    而"ack 节奏"这一半已被 ①-28（§26.45：攒了 ack 再加大批还是更差 ✓）否掉 ⇒ 别再按它推理 ✓。
#define FFRDP_LOWLEVEL_RX_BATCH_MAX 4
// 套接字缓冲请求值（KB ✓；内核翻倍后夹到 rmem_max ✓）；96 KB 是实测膝点（见 notes ✓）。
#define FFRDP_LOWLEVEL_BUFSIZE_KB 96

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

    // 没有期限时 select() 永久睡 ⇒ 要靠这个 socket 才能把"期限变了/该停了"传进去（任何线程写一个字节 ✓）。
    int             notify;    // UDP socket connected to its own loopback address (see ffrdp_udp_wakeup_create)
    int             asleep;    // 1 while the loop is inside select(); written and read under this lock
    pthread_mutex_t lock;      // guards deadline and asleep, which are written from other threads
    uint32_t        deadline;  // absolute ms on the get_timestamp32_ms() clock, 0 = nothing due
#if FFRDP_DIAG
    // 只读统计（dev 构建才有 ✓）：把"空圈"拆成"被叫醒却没数据"与"期限到点" —— 协议侧只看得到总圈数 ✗。
    uint32_t        stat_lap;         // 圈数：每次 select 归来算一圈 ✓
    uint32_t        stat_lap_frame;   // 其中收到 >=1 帧的圈数 ✓
    uint32_t        stat_lap_notify;  // 其中通知 fd 可读的圈数（跨线程 timer 叫醒 ✓）
    uint32_t        stat_lap_timeout; // 其中 select 返回 0（期限到点 ✓）
    uint32_t        stat_rx_frames;   // 收到的帧总数（直接用来算"帧/圈"✓）
    // 追加①：select 说可读、recvfrom 却立刻失败的那类空圈（典型是 ICMP 错误 ✓）⇒ 单独记账 + 记首次 errno ✓。
    uint32_t        stat_rx_fail;     // recvfrom <= 0 的次数（这些圈一个包都没读到 ✗）
    // 追加②：谁把期限 arm 成"已到点" —— 跨线程（应用 send 后催一下 ✓）与循环内（协议 tick ✓）分两类 ✓。
    uint32_t        stat_arm_now_other;  // 跨线程 arm 成 <= now 的次数（①-17 起：**不含** 0 ✓）
    uint32_t        stat_arm_now_loop;   // 从循环自己线程 arm 成 <= now 的次数（同上 ✓）
    // arm 值分档：0 是"没有期限"的哨兵，**不是**"现在" ✗（混在一起会让"要求现在"的数虚高 ✓）。
    uint32_t        stat_arm_d0;         // arm 值 == 0（没有期限 ✓；不是"现在" ✗）
    uint32_t        stat_arm_dnow;       // <= now（真的要求"现在" ✗）
    uint32_t        stat_arm_dnear;      // now+1..4
    uint32_t        stat_arm_dfar;       // > now+4
#endif
    // RX 批上限 = FFRDP_LOWLEVEL_RX_BATCH_MAX（文件头的重扫选出来的 4 ✓）。
    int             rx_batch;         // 一圈最多收几帧（= FFRDP_LOWLEVEL_RX_BATCH_MAX ✓）
    int             bufsize;          // 套接字缓冲请求值（字节 ✓）= FFRDP_LOWLEVEL_BUFSIZE_KB * 1024
#if FFRDP_DIAG
    int             buf_rcv_eff;      // 内核实际给的（getsockopt ✓，dump 里打 ✓）
    int             buf_snd_eff;
    int             stat_rx_errno;    // 第一次失败的 errno（0 = 还没失败过 ✓）
    // 追加③：把空圈再拆一层 —— 睡下时期限离现在多远 ✓、tick 进去时是什么情形 ✓。
    uint32_t        stat_left_le0;    // 睡下时期限已过期（left<0 ⇒ 实睡 0ms ✗ 自旋圈）
    uint32_t        stat_left_0;      // left == 0（0ms，但确实等到点了 ✓）
    uint32_t        stat_left_1_9;    // 1~9ms
    uint32_t        stat_left_ge10;   // >= 10ms（正常睡眠 ✓）
    uint32_t        stat_sleep_none;  // 睡下时没有期限（deadline 0 ⇒ 永久睡 ✓）
    uint32_t        stat_tick_fire;   // tick 真的到点并跑了回调 ✓
    uint32_t        stat_tick_none;   // tick 进去发现没期限（没有回调 ✗）
    uint32_t        stat_tick_early;  // tick 进去发现期限还没到（没有回调 ✗）
    // 收包侧代价：批有没有被上限截断 ✓、一次回调多少微秒 ✓、每帧 sendto 次数 ✓。
    uint32_t        stat_rx_batch_full; // 一圈收满上限就停了（还没有证明后面真有货 ✗）
    uint32_t        stat_rx_capped_more; // 上面那批里，**零超时 select 说"还有可读"**的次数 ⇒ 真积压 ✓
    uint32_t        stat_lap_rx1;       // 这一圈只收 1 帧（没批起来 ✓）
    uint32_t        stat_lap_rxmulti;   // 这一圈收了 >= 2 帧（批起来了 ✓）
    uint32_t        stat_cb_us_sum;     // 收包回调耗时之和（微秒 ✓）
    uint32_t        stat_cb_us_max;     // 单次最长（微秒 ✓）
    uint32_t        stat_tx_calls;      // sendto 调用次数（本架构一帧一个数据报 ⇒ 与帧数比就是"每帧 sendto 次数"✓）
    uint32_t        stat_tx_bytes;      // 送出去的字节数 ✓
    // 一圈的时间去向 + arm/wake 自己值多少 ✓。采样口径：每 64 圈一次 ✓；三段互斥、和 = 整圈 ✓
    // （pre = arm/算期限/select ✓、mid = 记账/notify/收包（含回调 ✓）、tick = 期限 + 协议圈 ✓）。
    uint32_t        stat_cost_n;        // 采样到的圈数（分母 ✓）
    uint32_t        stat_cost_pre_us;   // 三段之和（微秒 ✓）
    uint32_t        stat_cost_mid_us;
    uint32_t        stat_cost_tick_us;
    uint32_t        stat_cost_all_us;   // 整圈（t0→t3 ✓）
    uint32_t        stat_cost_max_us;   // 采到的圈里最长的一个 ✓
    // 慢圈（收尾空闲 ⇒ select 永久睡 ✓）单列 ⇒ 每圈均值 = (all - slow_us) / (n - slow_n) ✓。
    uint32_t        stat_cost_slow_n;   // 采到的慢圈数（> 100 ms ✓）
    uint32_t        stat_cost_slow_us;  // 它们的时长之和 ✓
    uint32_t        stat_arm_n_loop;    // 循环内 arm 的次数（协议在回调里定下次期限 ✓）
    uint32_t        stat_arm_us_loop;   // 它们花掉的微秒 ✓
    uint32_t        stat_arm_n_other;   // 跨线程 arm 的次数（应用 send 后催一下 ✓）
    uint32_t        stat_arm_us_other;
#endif
} UDP_LOWLEVEL;

// 跨 API 的 addr 是 uint64（两条层的共同约定，见 utils.h）⇒ 这里只剩 sockaddr_in ↔ (ip,port) 的胶水 ✓。
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

// 唤醒 socket：绑回环端口再 connect 到自己 ⇒ 写一个字节就让它可读 ✓（connect 后别人塞不进来 ✓）。
static int ffrdp_udp_wakeup_create(UDP_LOWLEVEL *udp)
{
    struct sockaddr_in lo  = {};
    socklen_t          len = sizeof(lo);
    int                fd  = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return -1;
    socket_setopt(fd, "nonblock_io", 1);   // a caller must never block on a full queue
    // 它一辈子只传唤醒字节 ⇒ 请求 4 KB 就够（原来拿内核默认 212 KB/向 ✗ = 白占 416 KB ✓）。
    socket_setopt(fd, "send_buffer", 4096);
    socket_setopt(fd, "recv_buffer", 4096);
    lo.sin_family      = AF_INET;
    lo.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    lo.sin_port        = 0;                // the kernel picks; nobody else ever needs to know it
    if (bind(fd, (struct sockaddr*)&lo, sizeof(lo)) != 0) { closesocket(fd); return -1; }
    if (getsockname(fd, (struct sockaddr*)&lo, &len) != 0) { closesocket(fd); return -1; }
    if (connect(fd, (struct sockaddr*)&lo, sizeof(lo)) != 0) { closesocket(fd); return -1; }
    udp->notify = fd;
    return 0;
}

// 唤醒循环：任何线程都可调用 ✓；队列满 = 已有一个待处理的唤醒 ⇒ 不是错误 ✓。
static void ffrdp_udp_notify(UDP_LOWLEVEL *udp)
{
    char b = 1;
    int  n;
    if (udp->notify < 0) return;
    // 队列满不是错误（已有唤醒在等 ✓）；结果存进变量而不是丢掉 —— send() 是 warn_unused_result ✓。
    n = (int)send(udp->notify, &b, 1, 0);
    (void)n;
}

static void ffrdp_udp_notify_drain(UDP_LOWLEVEL *udp)
{
    char b[64];
    // 必须读空：留一个字节在队列里，select() 会一直报可读 ⇒ 循环变全速自旋 ✗。
    if (udp->notify < 0) return;
    while (recv(udp->notify, b, sizeof(b), 0) > 0) { }
}

// asleep 与 deadline 同一把锁 ✓：**先置 asleep 再读期限** ⇒ 期间的 timer() 看到 asleep=1 会发字节 ✓，
// 而那个字节已被缓冲 ⇒ 随后的 select() 立刻返回 ✓（唤醒不会像条件变量那样丢 ✓）。
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

#if FFRDP_DIAG
// 量"每收一帧一次回调"与"一圈三段"用的微秒时钟（与 ffrdp.c 里量 ack 延迟那个同源 ✓）。
static uint64_t ffrdp_udp_us(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)(ts.tv_nsec / 1000);
}
#endif

// 到点就投递：期限在回调**之前**消费掉 ⇒ 回调里 arm 的下一个不被覆盖 ✓，不 arm 就停表 ✓。
static void ffrdp_udp_tick(UDP_LOWLEVEL *udp)
{
    uint32_t now = get_timestamp32_ms();
    pthread_mutex_lock(&udp->lock);
    if (!udp->deadline || (int32_t)(udp->deadline - now) > 0) {
        if (!udp->deadline) LSTAT(udp, stat_tick_none); else LSTAT(udp, stat_tick_early);
        pthread_mutex_unlock(&udp->lock); return;
    }
    LSTAT(udp, stat_tick_fire);
    udp->deadline = 0;
    pthread_mutex_unlock(&udp->lock);
    if (udp->callback) udp->callback(udp->cbctx, FFRDP_LOWLEVEL_TYPE_TIMER, NULL, 0);
}

// 一次 timer 请求（任何线程 ✓）：值总是存下，只有唤醒是有条件的 ✓。三类白唤醒在这里被滤掉 ✓：
// 调用方就是循环自己 ✓；新期限不比已 arm 的早 ✓；循环没在睡 ✓。
static int ffrdp_udp_lowlevel_timer(void *fl, uint32_t deadline_ms)
{
    UDP_LOWLEVEL *udp = (UDP_LOWLEVEL *)fl;
    uint32_t      old;
    int           wake, from_loop;
    if (!udp) return -1;
    pthread_mutex_lock(&udp->lock);
    old           = udp->deadline;
    udp->deadline = deadline_ms;
    wake          = (old == 0) || ((int32_t)(deadline_ms - old) < 0) || udp->asleep;
    pthread_mutex_unlock(&udp->lock);
    // The same "am I the loop?" test exit() uses: a callback is running on the loop's own thread.
    from_loop = (udp->mthread.hthread && udp->mthread.hthread == (void*)pthread_self());
#if FFRDP_DIAG
    {   uint64_t t_arm0 = ffrdp_udp_us();      // arm/wake 自己值多少 ✓
        // 分档：0 是"没有期限"的哨兵，**不是**"现在" ✓
        {   int32_t left = (int32_t)(deadline_ms - get_timestamp32_ms());
            if      (!deadline_ms)  udp->stat_arm_d0++;
            else if (left <= 0)   { udp->stat_arm_dnow++;
                                    if (from_loop) udp->stat_arm_now_loop++; else udp->stat_arm_now_other++; }
            else if (left <= 4)     udp->stat_arm_dnear++;
            else                    udp->stat_arm_dfar++;
        }
        {   uint32_t d_arm = (uint32_t)(ffrdp_udp_us() - t_arm0);
            if (from_loop) { udp->stat_arm_n_loop++;  udp->stat_arm_us_loop  += d_arm; }
            else           { udp->stat_arm_n_other++; udp->stat_arm_us_other += d_arm; }
        }
    }
#endif
    if (wake && !from_loop) ffrdp_udp_notify(udp);
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
        // 唤醒 socket 也由本线程建/关（与 socket_init()/socket_exit() 同一层 ✓）；建不出来就下一圈再试 ✓。
        if (udp->notify < 0 && ffrdp_udp_wakeup_create(udp) != 0) {
            printf("<2> ffrdp-udp failed to create the wakeup socket\n");
            usleep(100 * 1000); continue;
        }
        if (udp->sockfd < 0) {
            udp->sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            socket_setopt(udp->sockfd, "reuse_addr" , 1); // enable reuse addr
            // 非阻塞：交不出去的帧要作为错误回到调用方 ✓，不能在收包循环里卡住 ✓。
            socket_setopt(udp->sockfd, "nonblock_io", 1);
            socket_setopt(udp->sockfd, "send_buffer", udp->bufsize);   // ①-30
            socket_setopt(udp->sockfd, "recv_buffer", udp->bufsize);   // ①-30
#if FFRDP_DIAG
            {   // 把内核**实际**给的值记下来（Linux 会翻倍后再夹到 rmem_max/wmem_max ✓）
                socklen_t sl = sizeof(int);
                udp->buf_rcv_eff = udp->buf_snd_eff = 0;
                getsockopt(udp->sockfd, SOL_SOCKET, SO_RCVBUF, (void*)&udp->buf_rcv_eff, &sl);
                sl = sizeof(int);
                getsockopt(udp->sockfd, SOL_SOCKET, SO_SNDBUF, (void*)&udp->buf_snd_eff, &sl);
            }
#endif
            if (udp->sockfd < 0) { printf("<2> ffrdp-udp failed to create socket !\n"); usleep(100 * 1000); continue; }
            if ((udp->flags & FLAG_BIND_ADDR) && bind(udp->sockfd, (struct sockaddr*)&udp->addr, sizeof(udp->addr)) < 0) {
                printf("<2> udpproxy failed to bind socket to port %d !\n", ntohs(udp->addr.sin_port));
                closesocket(udp->sockfd); udp->sockfd = -1; usleep(100 * 1000); continue;
            }
        }

#if FFRDP_DIAG
        // 一圈三段计时：每 64 圈采一次 ✓（摊薄到全部圈 ≈ 1.5 ns/圈 ⇒ 量不出代价 ✓）
        int      cost  = ((udp->stat_lap & 63) == 0);
        uint64_t  t_c0 = 0, t_c1 = 0, t_c2 = 0, t_c3 = 0;
        if (cost) t_c0 = ffrdp_udp_us();
#endif
        // 睡到有帧 / 期限到 / 被叫醒 ✓；没有期限就永久睡 ✓（旧的固定 100 ms 既限精度又让空转端点每秒醒十次 ✗）。
        struct timeval tv, *tvp = NULL;
        uint32_t dl = ffrdp_udp_sleep_begin(udp);
        if (dl) {
            int32_t left = (int32_t)(dl - get_timestamp32_ms());
            // left<0 就是"实睡 0ms"的那种自旋圈 ✗（select 返回 0 ⇒ 又是一圈）
            if (left < 0)       LSTAT(udp, stat_left_le0);
            else if (left == 0) LSTAT(udp, stat_left_0);
            else if (left <= 9) LSTAT(udp, stat_left_1_9);
            else                LSTAT(udp, stat_left_ge10);
            if (left < 0) left = 0; // due already: come straight back
            tv.tv_sec  = left / 1000;
            tv.tv_usec = (left % 1000) * 1000;
            tvp = &tv;
        } else {
            LSTAT(udp, stat_sleep_none); // 没有期限 ⇒ 永久睡 ✓
        }
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(udp->sockfd, &rfds);
        FD_SET(udp->notify, &rfds);
        ret = select(MAX(udp->sockfd, udp->notify) + 1, &rfds, NULL, NULL, tvp);
        ffrdp_udp_sleep_end(udp); // awake again, whatever select() answered
#if FFRDP_DIAG
        if (cost) t_c1 = ffrdp_udp_us(); // pre 段结束 ✓
#endif
        if (ret == -1) {
            if (socket_errno() == EINTR) continue; // a signal is not an error, and not a reason to sleep
            printf("select error !\n"); fflush(stdout); usleep(100 * 1000); continue;
        }
        LSTAT(udp, stat_lap); // 一圈一笔 ✓
        if (ret == 0) LSTAT(udp, stat_lap_timeout);
        if (ret > 0 && FD_ISSET(udp->notify, &rfds)) LSTAT(udp, stat_lap_notify);

        if (ret > 0 && FD_ISSET(udp->notify, &rfds)) ffrdp_udp_notify_drain(udp);
        // 这里和下面的收包循环都不测"停"：每圈都有界 ✓（收包有上限 ✓、socket 非阻塞 ✓、select 有期限 ✓）⇒
        // 顶多多跑一圈 ✓；ffrdp_free() 会先 join 我们 ✓。
        if (ret > 0 && FD_ISSET(udp->sockfd, &rfds)) {
            // 一圈把就绪的包都收下来（有上限 ✓）：上层是"一段一个 ack" ✓ ⇒ 一圈只收一帧就等于一帧一个 ack ✗。
            int frames = 0;
            while (frames < udp->rx_batch) { // ①-25：运行时上限 ✓
                uint8_t buf[FFRDP_LOWLEVEL_FRAME_MAX];
                struct  sockaddr_in from;
                socklen_t fromlen = sizeof(from);
                // (void *)：Winsock 把 recvfrom 的缓冲声明成 char*、POSIX 是 void* ⇒ 这个转换让两边都 -Wall -Werror 干净 ✓。
                ret = recvfrom(udp->sockfd, (void*)buf, sizeof(buf), 0, (struct sockaddr*)&from, &fromlen);
                if (ret <= 0) { // zero length datagram, an empty socket, or a socket error
                    LSTAT(udp, stat_rx_fail);
                    LSTAT_SET(udp, stat_rx_errno, socket_errno());
                    break;
                }
                frames++; LSTAT(udp, stat_rx_frames); if (frames == 1) LSTAT(udp, stat_lap_frame);
                if (udp->callback) {
                    // msg 带来源地址 ⇒ 回复能原路 send() 回去 ✓（这是本层报地址的唯一原因 ✓）；buf 是每圈复用的栈缓冲 ⇒
                    // 只在这次回调期间有效，要留就得自己拷 ✓。
                    FFRDP_LOWLEVEL_MSG msg;
                    msg.addr = ffrdp_udp_addr_pack(&from);
                    msg.buf  = buf;
                    msg.len  = ret;
#if FFRDP_DIAG
                    {   // 给这一次回调标价（量的是协议侧那一段 ✓）
                        uint64_t t_cb = ffrdp_udp_us();
                        udp->callback(udp->cbctx, FFRDP_LOWLEVEL_TYPE_RECV, &msg, sizeof(msg));
                        {   uint32_t dt = (uint32_t)(ffrdp_udp_us() - t_cb);
                            udp->stat_cb_us_sum += dt;
                            if (dt > udp->stat_cb_us_max) udp->stat_cb_us_max = dt;
                        }
                    }
#else
                    udp->callback(udp->cbctx, FFRDP_LOWLEVEL_TYPE_RECV, &msg, sizeof(msg));
#endif
                }
            }
#if FFRDP_DIAG
            // 收满时再问一次零超时 select ✓：它能读 ⇒ socket 里真有货 = 这圈是被上限截断的 ✓（不消费数据报 ✓）。
            if (frames >= udp->rx_batch) {                       // ①-25 ✓
                udp->stat_rx_batch_full++;
                {   fd_set pf; struct timeval z; int pr;
                    FD_ZERO(&pf); FD_SET(udp->sockfd, &pf);
                    z.tv_sec = 0; z.tv_usec = 0;
                    pr = select(udp->sockfd + 1, &pf, NULL, NULL, &z);
                    if (pr > 0 && FD_ISSET(udp->sockfd, &pf)) udp->stat_rx_capped_more++;
                }
            }
            if (frames == 1) udp->stat_lap_rx1++; else if (frames >= 2) udp->stat_lap_rxmulti++;
#endif
        }
#if FFRDP_DIAG
        if (cost) t_c2 = ffrdp_udp_us();                                              // mid 段结束 ✓
#endif
        // 先收活、再取时钟 ⇒ 两者不交错：回调里 arm 的 tick 会在**同一圈**投递（而不是下一圈 ✓）。
        ffrdp_udp_tick(udp);
#if FFRDP_DIAG
        if (cost) {                                                                   // tick 段结束 ⇒ 记账 ✓
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
#endif
    }

#if FFRDP_DIAG
    // ① 的读出点：这一行 ASCII，任何控制台都不乱码 ✓。空圈 = 总圈 - 带帧圈 ✓。
    printf("udp lowlevel laps: %u total, %u with frames, %u notify-woken, %u deadline, %u empty, %u frames, %u rx-fail (errno %d)\n",
           udp->stat_lap, udp->stat_lap_frame, udp->stat_lap_notify, udp->stat_lap_timeout,
           udp->stat_lap - udp->stat_lap_frame, udp->stat_rx_frames,
           udp->stat_rx_fail, udp->stat_rx_errno);
    printf("udp lowlevel arm-now: %u from other threads, %u from the loop itself (only true '<=now' arms; the zero sentinel is out)\n",
           udp->stat_arm_now_other, udp->stat_arm_now_loop);
    // ①-17：arm 值分档 —— zero 那一档就是"没有期限"（被 ①-2 误算成"现在"的那一批 ✗）
    printf("udp lowlevel arm value: zero %u, <=now %u, now+1..4 %u, >now+4 %u\n",
           udp->stat_arm_d0, udp->stat_arm_dnow, udp->stat_arm_dnear, udp->stat_arm_dfar);
    // ①-4：空圈往下一层拆 —— 睡下时的 left 分布 + tick 三分类 ✓
    printf("udp lowlevel sleep-left: expired %u, 0ms %u, 1-9ms %u, >=10ms %u, no-deadline %u\n",
           udp->stat_left_le0, udp->stat_left_0, udp->stat_left_1_9, udp->stat_left_ge10, udp->stat_sleep_none);
    printf("udp lowlevel tick kind: fire %u, none %u, early %u (the two 'none/early' are laps that ran no callback)\n",
           udp->stat_tick_fire, udp->stat_tick_none, udp->stat_tick_early);
    // ①-③（§26.30 步骤1）：收包侧代价 —— 批有没有被上限截断 ✓、一次回调多少微秒 ✓、送出多少个数据报 ✓
    printf("udp lowlevel rx cost: batch-full %u, of which socket still had data %u, laps with 1 frame %u, with >=2 frames %u, callback %u us total, %u us max\n",
           udp->stat_rx_batch_full, udp->stat_rx_capped_more, udp->stat_lap_rx1, udp->stat_lap_rxmulti,
           udp->stat_cb_us_sum, udp->stat_cb_us_max);
    printf("udp lowlevel rx batch: %d frames per lap (compile-time FFRDP_LOWLEVEL_RX_BATCH_MAX = %d)\n",
           udp->rx_batch, FFRDP_LOWLEVEL_RX_BATCH_MAX);
    printf("footprint ①-35      : sizeof(UDP_LOWLEVEL) %u bytes (plus the two sockets' kernel buffers above)\n",
           (unsigned)sizeof(UDP_LOWLEVEL));
    printf("udp lowlevel buffers : requested %d B, kernel gave rcv %d B / snd %d B (FFRDP_LOWLEVEL_BUFSIZE_KB; rmem_max caps it)\n",
           udp->bufsize, udp->buf_rcv_eff, udp->buf_snd_eff);
    {   // 内存盘点（只读 ✓）：**唤醒 socket** 的内核缓冲 —— 它上面从来没设过大小，
        // 唤醒 socket 的内核缓冲（它只传唤醒字节 ⇒ 拿内核默认值就是白占 ✓）。
        int r = 0, w = 0; socklen_t sl = sizeof(int);
        if (udp->notify >= 0) {
            getsockopt(udp->notify, SOL_SOCKET, SO_RCVBUF, (void*)&r, &sl); sl = sizeof(int);
            getsockopt(udp->notify, SOL_SOCKET, SO_SNDBUF, (void*)&w, &sl);
        }
        printf("wakeup socket bufs    : rcv %d B / snd %d B (kernel default; it only ever carries wakeup bytes)\n", r, w);
    }
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
#endif
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
    // 每帧 sendto 次数 = 本层调用数 / ffrdp 那块里的帧数 ✓（ack/query 也走这里 ⇒ >= 帧数 ✓）。
    LSTAT(udp, stat_tx_calls);
    if (ret > 0) LSTAT_ADD(udp, stat_tx_bytes, ret);
    if (ret == len) return 0;
    // 非阻塞 ⇒ 发送缓冲满是繁忙链路上的正常答复 ✓（不值得每帧打一行 ✓）；其余才算真错误 ✓。
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
    // 顺序就是要点：**先置停标志、再唤醒** ✓ —— 反过来的话，一个正在两圈之间的线程会吃掉字节、看不到标志、
    // 又睡回去 ✓。这里是"join 自己"的情形，契约上禁止（见 lowlevel.h ✓）⇒ 代价压成"这一轮之后自然退出" ✓。
    if (udp->mthread.hthread && udp->mthread.hthread == (void*)pthread_self()) {
        udp->mthread.flags |= MOD_THREAD_FLAG_STOP;
        return;
    }
    udp->mthread.flags |= MOD_THREAD_FLAG_STOP;
    ffrdp_udp_notify(udp);
    // 然后停它并等它：那个线程拥有 socket 且会解引用本句柄 ⇒ 不等就释放会读到已释放内存 ✓（socket 由它自己关 ✓）。
    module_thread_set_state(&udp->mthread, 0);
    // 唤醒 socket 已由工作线程退出前关掉 ✓；socket_init/exit 也都在那个线程里 ⇒ 这里没有 socket 层收尾 ✓。
    pthread_mutex_destroy(&udp->lock);
    free(udp);
}

// params 只认低层文档那一形："bind:ip:port" ✓。畸形 "bind:" 直接拒 ✓；其余按"不绑定"处理并打一行 ✓
// （只记在这里 ✓：bind 由接收线程做 ⇒ 端口正被占用是稍后重试，而不是让整个启动失败 ✓）。
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
    udp->rx_batch     = FFRDP_LOWLEVEL_RX_BATCH_MAX;
    udp->bufsize      = FFRDP_LOWLEVEL_BUFSIZE_KB * 1024;
    udp->callback     = callback;
    udp->cbctx        = cbctx;
    udp->mthread.proc = ffrdp_udp_lowlevel_proc;
    udp->mthread.arg  = udp;
    // 唤醒 socket 不在这里建：它属于工作线程（也是平台 socket 层所在的线程 ✓）。早到的 timer() 不会丢 ✓：
    // 期限已经存在锁下 ✓，那个线程睡之前会读到它 ✓。
    pthread_mutex_init(&udp->lock, NULL);
    // 状态 1 启动接收线程（本层存在的意义 ✓；API 里没有单独的 start ✓ ⇒ 句柄一回来就已经在听 ✓）。
    module_thread_set_state(&udp->mthread, 1);
    return (FFRDP_LOWLEVEL*)udp;
}
