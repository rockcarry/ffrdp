
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

typedef struct {
    int  (*send)(void *fl, uint64_t addr, uint8_t *buf, int len);
    int  (*timer)(void *fl, uint32_t deadline_ms);
    void (*exit)(void *fl);
    int  (*addr_parse)(void *fl, const char *text, uint64_t *addr);

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

// The addr that crosses the API is one uint64: the IPv4 address in the high 32 bits, the port in the low
// 16, both in host order. The address is kept in host order on purpose -- 127.0.0.1 comes out as
// 0x7F000001, which is what a log line prints, instead of the byte swapped number a raw s_addr would give
// on a little endian machine. Only this file needs to know the layout: a caller takes the addr out of the
// receive message and hands it back to send unchanged.
static uint64_t ffrdp_udp_addr_pack(const struct sockaddr_in *sin)
{
    return ((uint64_t)ntohl(sin->sin_addr.s_addr) << 16) | (uint64_t)ntohs(sin->sin_port);
}

static void ffrdp_udp_addr_unpack(uint64_t addr, struct sockaddr_in *sin)
{
    memset(sin, 0, sizeof(*sin));
    sin->sin_family      = AF_INET;
    sin->sin_addr.s_addr = htonl((uint32_t)(addr >> 16));
    sin->sin_port        = htons((uint16_t)(addr & 0xFFFF));
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
static int ffrdp_udp_lowlevel_addr_parse(void *fl, const char *text, uint64_t *addr)
{
    struct sockaddr_in sin;
    const char        *colon;
    char               ip[64];
    size_t             iplen;
    int                port;

    if (!fl || !text || !addr) return -1;
    colon = strrchr(text, ':');
    iplen = colon ? (size_t)(colon - text) : 0;
    if (!colon || iplen == 0 || iplen >= sizeof(ip)) return -1;
    memcpy(ip, text, iplen);
    ip[iplen] = '\0';
    port = atoi(colon + 1);
    if (port <= 0 || port > 65535) return -1;

    memset(&sin, 0, sizeof(sin));
    sin.sin_family      = AF_INET;
    sin.sin_addr.s_addr = inet_addr(ip);
    if (sin.sin_addr.s_addr == INADDR_NONE) return -1;
    sin.sin_port        = htons((uint16_t)port);
    *addr = ffrdp_udp_addr_pack(&sin);
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
            uint8_t buf[FFRDP_LOWLEVEL_FRAME_MAX];
            struct  sockaddr_in from;
            socklen_t fromlen = sizeof(from);
            ret = recvfrom(udp->sockfd, buf, sizeof(buf), 0, (struct sockaddr*)&from, &fromlen);
            if (ret <= 0) continue;   // zero length datagram, or a socket error: nothing to hand over
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
    const char *rest, *colon;
    char        ip[64];
    int         port;
    size_t      iplen;

    // "none": 明确表示"这个端点不绑定"（客户端就是这样：源端口由内核在第一次发包时定）。它是一条正常
    // 的配置，不是笔误，所以不打印、也没有 flags 位 —— 没有 FLAG_BIND_ADDR，接收线程就不去 bind。
    if (strcmp(params, "none") == 0) return 0;
    if (strncmp(params, "bind:", 5) != 0) {
        printf("<3> ffrdp-udp params \"%s\" is not \"bind:ip:port\" or \"none\", no bind address is set\n", params);
        return 0;
    }
    rest  = params + 5;
    colon = strrchr(rest, ':');   // the last colon: an IPv4 literal has none, so this splits ip and port
    iplen = colon ? (size_t)(colon - rest) : 0;
    if (!colon || iplen == 0 || iplen >= sizeof(ip)) {
        printf("<2> ffrdp-udp params \"%s\" has no ip:port after \"bind:\"\n", params);
        return -1;
    }
    memcpy(ip, rest, iplen);
    ip[iplen] = '\0';
    port = atoi(colon + 1);
    if (port < 0 || port > 65535) {
        printf("<2> ffrdp-udp params \"%s\" has a port outside 0..65535\n", params);
        return -1;
    }
    memset(&udp->addr, 0, sizeof(udp->addr));
    udp->addr.sin_family = AF_INET;
    udp->addr.sin_port   = htons((uint16_t)port);
    udp->addr.sin_addr.s_addr = inet_addr(ip);
    if (udp->addr.sin_addr.s_addr == INADDR_NONE) {
        printf("<2> ffrdp-udp params \"%s\" has an address that is not a dotted quad\n", params);
        return -1;
    }
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
    udp->addr_parse   = ffrdp_udp_lowlevel_addr_parse;
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
