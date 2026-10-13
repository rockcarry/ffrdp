// deterministic test: how far the rto is inflated when several already expired
// frames have to be retransmitted one after another.
//
// ffrdp_congestion_control(CEVENT_ACK_TIMEOUT) drops cwnd to 1, so only ONE frame
// is retransmitted per ffrdp_update().  When the connection is collapsed the head
// keeps being replaced by the next already expired frame, so the retransmits come
// back to back and the naive code multiplies the rto by 1.5 for each of them -
// twelve frames are enough to slam it into the 2000 ms ceiling.
#include <stdio.h>
#include <string.h>
#include "simdefs.h"
#include "../../ffrdp.c"

// ── 结构拆分（P1）之后，连接级字段挂在 ctxt->conn 下 ──────────────────────────
// HEAD 侧没有这一层 ⇒ 用这个宏把两侧写成同一句话 ✓（与 TEST_SEND / TEST_RUN 同一套路 ✓）。
#ifdef CONN_SPLIT
#define CONN(c) (&(c)->conn)
#else
#define CONN(c) (c)
#endif

// The harness builds this test twice: once against HEAD's ffrdp.c, once against the working tree's. HEAD
// has no timeout argument on send/recv and the working tree does, so the test calls these wrappers -- both
// sides compile, and both sides mean "do not wait", which is exactly what HEAD does. run.sh defines
// FFRDP_API_HAS_TIMEOUT for the side that has it.
#ifdef FFRDP_API_V2
// v2 接口：send/recv 多了连接句柄（单连接测试用 0），init 收配置串，退出叫 exit。
#define TEST_SEND(c, b, l) ffrdp_send(c, 0, b, l, 0)
#define TEST_RECV(c, b, l) ffrdp_recv(c, 0, b, l, 0)
#define TEST_FREE(c)       ffrdp_exit(c)
#elif defined(FFRDP_API_HAS_TIMEOUT)
#define TEST_SEND(c, b, l) ffrdp_send(c, b, l, 0)
#define TEST_RECV(c, b, l) ffrdp_recv(c, b, l, 0)
#define TEST_FREE(c)       ffrdp_free(c)
#else
#define TEST_SEND(c, b, l) ffrdp_send(c, b, l)
#define TEST_RECV(c, b, l) ffrdp_recv(c, b, l)
#define TEST_FREE(c)       ffrdp_free(c)
#endif

// One step of the protocol. HEAD is driven by the test (ffrdp_update); the working tree is driven by
// its transport, so a step is a step of the fake lowlevel: every packet queued for endpoint idx is
// handed over as one RECV, then one TIMER. idx is the number the test passes to sim_begin().
#ifdef FFRDP_LL_DRIVEN
#define TEST_RUN(c, idx) sim_ll_run(idx, c)
#else
#define TEST_RUN(c, idx) ffrdp_update(c)
#endif

// ffrdp_init 的参数形状变过一次：ip + port 收敛成单个 "ip:port"。两侧都要能编，所以测试走这个入口
// （调用点只关心 ip / port / 角色 / smss）。
#ifdef FFRDP_API_V2
static void *TEST_INIT(const char *ip, int port, int server, int mss)
{
    char params[128];
    snprintf(params, sizeof(params), "%s://%s:%d,mss:%d,fec:0",
             server ? "ffrdps" : "ffrdpc", ip, port, mss);
    return ffrdp_init(params, NULL, NULL);
}
#elif defined(FFRDP_API_HAS_ADDR)
static void *TEST_INIT(const char *ip, int port, int server, int mss)
{
    char addr[64];
    snprintf(addr, sizeof(addr), "%s:%d", ip, port);
    return ffrdp_init(addr, NULL, NULL, server, mss, 0);
}
#else
#define TEST_INIT(ip, port, server, mss) ffrdp_init((char *)(ip), port, NULL, NULL, server, mss, 0)
#endif

static void make_ack(unsigned char *ack, unsigned int una, unsigned int mack, unsigned char rwnd)
{
    memset(ack, 0, 8);
    *(uint32_t*)(ack + 0) = (FFRDP_FRAME_TYPE_ACK << 0) | (una << 8);
    *(uint32_t*)(ack + 4) = (mack << 0) | ((uint32_t)rwnd << 24);
}

int main(void)
{
    static unsigned char buf[64 * 1024], ack[8];
    FFRDPCONTEXT *c;
    int i, n = 12;

    sim_reset();
    sim_config(0, 0, 0, 0);
    c = TEST_INIT("127.0.0.1", 8001, 0, 1024);
    sim_begin(0);
    TEST_SEND(c, (char*)buf, n * 1024);    // n frames
    TEST_RUN(c, 0);                         // first send them all
    sim_advance(30);                        // every tick_timeout of them expires

    printf("  rto starts at %u, ceiling %u, %d expired frames in the window\n",
           FFRDP_MIN_RTO, FFRDP_MAX_RTO, n);
    printf("  rto after each retransmit:");
    for (i = 0; i < n; i++) {
        make_ack(ack, i + 1, 0, 255);       // peer confirms the head, exposing the next expired frame
        sim_inject(0, ack, 8);
        TEST_RUN(c, 0);                     // one expired head is retransmitted here
        printf(" %u", CONN(c)->rto);
    }
    printf("\n  retransmits=%u reach_maxrto=%u\n", CONN(c)->counter_resend_rto, CONN(c)->counter_reach_maxrto);
#ifdef NEW_COUNTERS
    printf("  timeout_evt=%u rto_backoff=%u (12 timeouts, but the rto only backs off once)\n",
           CONN(c)->counter_timeout_evt, CONN(c)->counter_rto_backoff);
#endif
    return 0;
}
