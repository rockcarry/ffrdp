
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <pthread.h>
#include "socket.h"
#include "utils.h"
#include "lowlevel.h"

#ifdef WIN32
// The wakeup below is a POSIX pipe, watched with select(). That works on every unix here, and it is the one
// thing in this file that is not portable: Windows select() only ever reports sockets, so a Windows build
// needs a loopback socket pair (or WSAEventSelect) doing the same job. It is refused rather than quietly
// turned into a no-op, because a loop that cannot be woken still compiles and still passes a smoke test --
// it just answers late, and only under load.
#error "ffrdp-udp lowlevel: needs a socket based wakeup on Windows, see the comment above"
#endif

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
    int  (*send)(void *fl, uint64_t addr, uint8_t *buf, int len);
    int  (*timer)(void *fl, uint32_t deadline_ms);
    void (*exit)(void *fl);

    #define FLAG_BIND_ADDR (1 << 0)
    uint32_t           flags;
    struct sockaddr_in addr;
    int                sockfd;
    FFRDP_LOWLEVEL_CB  callback;
    void              *cbctx;
    MOD_THREAD         mthread;

    // The loop sleeps in select() with no timeout at all whenever nothing is due, and select() only ever
    // wakes for a descriptor -- never because a variable changed. So this pipe is how "the deadline moved"
    // and "stop" reach it: any thread writes notify[1], the loop watches notify[0].
    int             notify[2];
    pthread_mutex_t lock;      // guards deadline, which is written from other threads
    uint32_t        deadline;  // absolute ms on the ffrdp_get_timestamp32_ms() clock, 0 = nothing due
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

static void ffrdp_udp_set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// Wake the loop. Any thread may call this; it is the only thing in this file that has to be safe to call
// while the loop is asleep. A full pipe means a wakeup is already pending, which is all this needs.
static void ffrdp_udp_notify(UDP_LOWLEVEL *udp)
{
    char    b = 1;
    ssize_t n;
    if (udp->notify[1] < 0) return;
    // A full pipe is not an error: it means a wakeup is already pending, which is all this is for. The
    // result goes into a variable rather than being cast away, because write() is warn_unused_result.
    n = write(udp->notify[1], &b, 1);
    (void)n;
}

static void ffrdp_udp_notify_drain(UDP_LOWLEVEL *udp)
{
    char b[64];
    // It has to be drained completely: a byte left behind keeps select() reporting it readable, and the
    // loop then spins at full speed instead of sleeping.
    if (udp->notify[0] < 0) return;
    while (read(udp->notify[0], b, sizeof(b)) > 0) { }
}

static uint32_t ffrdp_udp_deadline_get(UDP_LOWLEVEL *udp)
{
    uint32_t d;
    pthread_mutex_lock(&udp->lock);
    d = udp->deadline;
    pthread_mutex_unlock(&udp->lock);
    return d;
}

// Deliver a tick that has come due. The deadline is consumed BEFORE the callback runs, so a callback that
// arms its next one is not overwritten by this, and a callback that arms nothing stops the clock rather
// than being called again on every lap.
static void ffrdp_udp_tick(UDP_LOWLEVEL *udp)
{
    uint32_t now = ffrdp_get_timestamp32_ms();
    pthread_mutex_lock(&udp->lock);
    if (!udp->deadline || (int32_t)(udp->deadline - now) > 0) { pthread_mutex_unlock(&udp->lock); return; }
    udp->deadline = 0;
    pthread_mutex_unlock(&udp->lock);
    if (udp->callback) udp->callback(udp->cbctx, FFRDP_LOWLEVEL_TYPE_TIMER, NULL, 0);
}

// One timer request, from any thread: remember when, and wake the loop in case it is asleep on a later
// deadline or on no deadline at all.
static int ffrdp_udp_lowlevel_timer(void *fl, uint32_t deadline_ms)
{
    UDP_LOWLEVEL *udp = (UDP_LOWLEVEL *)fl;
    if (!udp) return -1;
    pthread_mutex_lock(&udp->lock);
    udp->deadline = deadline_ms;
    pthread_mutex_unlock(&udp->lock);
    ffrdp_udp_notify(udp);
    return 0;
}

// "1.2.3.4:5678" -> addr, going through the same encoder the receive path uses, so that a peer named up
// front is addressed exactly like one that has spoken. A peer needs a real port, so 0 is refused here.

static void* ffrdp_udp_lowlevel_proc(void *arg)
{
    UDP_LOWLEVEL *udp = arg;
    int ret;

    thread_setname("ffrdp-udp-lowlvl");
    socket_init();

    while (!module_thread_check_exit(&udp->mthread)) {
        if (module_thread_check_pause(&udp->mthread)) { usleep(100 * 1000); continue; }
        if (udp->sockfd < 0) {
            udp->sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            socket_setopt(udp->sockfd, "reuse_addr" , 1); // enable reuse addr
            // non blocking, the same contract this project's own socket has: a frame that cannot be handed
            // to the kernel must come back as an error to the caller, not as a stall in a receive loop.
            socket_setopt(udp->sockfd, "nonblock_io", 1);
            // 套接字缓冲区大小跟着 socket 一起搬到这里：它本来就是"这个后端要多大缓冲"的问题，不是协议
            // 的问题。值按一帧的最大长度算，与协议层以前写死的那个量级一致（128 帧）。socket_setopt 没有
            // 这两个 key，所以直接设。
            {   int bs = 128 * (FFRDP_LOWLEVEL_FRAME_MAX + 6);
                setsockopt(udp->sockfd, SOL_SOCKET, SO_SNDBUF, (char*)&bs, sizeof(bs));
                setsockopt(udp->sockfd, SOL_SOCKET, SO_RCVBUF, (char*)&bs, sizeof(bs));
            }
            if (udp->sockfd < 0) { printf("<2> ffrdp-udp failed to create socket !\n"); usleep(100 * 1000); continue; }
            if ((udp->flags & FLAG_BIND_ADDR) && bind(udp->sockfd, (struct sockaddr*)&udp->addr, sizeof(udp->addr)) < 0) {
                printf("<2> udpproxy failed to bind socket to port %d !\n", ntohs(udp->addr.sin_port));
                closesocket(udp->sockfd); udp->sockfd = -1; usleep(100 * 1000); continue;
            }
        }

        // Sleep until a frame arrives, the deadline passes, or someone wakes us -- and when there is no
        // deadline at all, sleep indefinitely. The old fixed 100ms wakeup both capped what precision the
        // layer above could ask for and kept an idle endpoint waking ten times a second.
        struct timeval tv, *tvp = NULL;
        uint32_t dl = ffrdp_udp_deadline_get(udp);
        if (dl) {
            int32_t left = (int32_t)(dl - ffrdp_get_timestamp32_ms());
            if (left < 0) left = 0;                    // due already: come straight back
            tv.tv_sec  = left / 1000;
            tv.tv_usec = (left % 1000) * 1000;
            tvp = &tv;
        }
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(udp->sockfd, &rfds);
        FD_SET(udp->notify[0], &rfds);
        ret = select(MAX(udp->sockfd, udp->notify[0]) + 1, &rfds, NULL, NULL, tvp);
        if (ret == -1) {
            if (socket_errno() == EINTR) continue;     // a signal is not an error, and not a reason to sleep
            printf("select error !\n"); fflush(stdout); usleep(100 * 1000); continue;
        }

        if (ret > 0 && FD_ISSET(udp->notify[0], &rfds)) ffrdp_udp_notify_drain(udp);
        // Woken in order to stop: leave without handing anything else to the layer.
        if (module_thread_check_exit(&udp->mthread)) break;
        if (ret > 0 && FD_ISSET(udp->sockfd, &rfds)) {
            // 一圈把当前就绪的包都收下来（有上限），再交给定时那一段。为什么要收完一圈：上层是"一段
            // 一个 ack"——它报的是 recv_seq + 位图，第二个 ack 覆盖的帧第一个已经报过了，什么都不多带；
            // 而一圈只收一帧就等于一帧一个 ack，实测（差分仿真）是攒批行为的 6.7 倍。上限是给定了的：
            // 一个不停把 socket 灌满的发送方不该把下面的定时工作（重传、探测、速率采样）饿住。
            int frames = 0;
            while (frames < FFRDP_LOWLEVEL_RX_BATCH_MAX && !module_thread_check_exit(&udp->mthread)) {
                uint8_t buf[FFRDP_LOWLEVEL_FRAME_MAX];
                struct  sockaddr_in from;
                socklen_t fromlen = sizeof(from);
                ret = recvfrom(udp->sockfd, buf, sizeof(buf), 0, (struct sockaddr*)&from, &fromlen);
                if (ret <= 0) break;   // zero length datagram, an empty socket, or a socket error
                frames++;
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
                    udp->callback(udp->cbctx, FFRDP_LOWLEVEL_TYPE_RECV, &msg, sizeof(msg));
                }
            }
        }
        // Received work first, then the clock, so the two are never handed over interleaved. A tick armed
        // from inside a receive callback is therefore delivered in the same lap rather than the next one.
        ffrdp_udp_tick(udp);
    }

    closesocket(udp->sockfd); udp->sockfd = -1;
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
    if (udp->notify[0] >= 0) close(udp->notify[0]);
    if (udp->notify[1] >= 0) close(udp->notify[1]);
    pthread_mutex_destroy(&udp->lock);
    free(udp);
}

// params: the one form lowlevel.h documents, "bind:127.0.0.1:1234". It is only recorded here: the receive
// thread does the binding, so a port that is busy right now is retried instead of failing the whole start.
// A malformed "bind:" is a configuration mistake worth refusing outright; anything else is taken as "no
// bind requested" with a line of output, which keeps a send-only endpoint usable.
static int ffrdp_udp_lowlevel_parse(UDP_LOWLEVEL *udp, const char *params)
{
    const char *rest;
    uint32_t    ip;
    int         port;

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
    if (ffrdp_udp_lowlevel_parse(udp, (const char*)params) != 0) { free(udp); return NULL; }

    udp->send         = ffrdp_udp_lowlevel_send;
    udp->timer        = ffrdp_udp_lowlevel_timer;
    udp->exit         = ffrdp_udp_lowlevel_exit;
    udp->sockfd       = -1;   // 0 is a valid descriptor, so "no socket yet" cannot be spelled with a zero
    udp->notify[0]    = -1;
    udp->notify[1]    = -1;
    udp->callback     = callback;
    udp->cbctx        = cbctx;
    udp->mthread.proc = ffrdp_udp_lowlevel_proc;
    udp->mthread.arg  = udp;
    // The wakeup pipe is created here rather than in the thread so that timer() is usable from the first
    // moment the handle exists: a caller that gets NULL back has not been handed a half working endpoint.
    if (pipe(udp->notify) != 0) {
        printf("<2> ffrdp-udp failed to create the wakeup pipe\n");
        free(udp); return NULL;
    }
    ffrdp_udp_set_nonblock(udp->notify[0]);
    ffrdp_udp_set_nonblock(udp->notify[1]);   // a caller must never block on a full pipe
    pthread_mutex_init(&udp->lock, NULL);
    // State 1 starts the receive thread: it is the whole point of this layer, and there is no separate
    // start call in the API, so the handle the caller gets back is already listening. State 0 in exit()
    // stops and joins it again.
    module_thread_set_state(&udp->mthread, 1);
    return (FFRDP_LOWLEVEL*)udp;
}
