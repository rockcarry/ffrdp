// deterministic test: do the new ack diagnostics report what they claim to.
// Three ack frames are injected by hand:
//   1) uma=1, mack=0        -> cumulative ack only
//   2) uma=1, mack=0b000111 -> seq 2,3,4 arrived, contiguous, no gap
//   3) uma=1, mack=0b001010 -> seq 3 and 5 arrived, seq 4 missing -> a real gap
// so ack_recv=3, ack_mack=2, ack_hole=1 is the expected result.
#include <stdio.h>
#include <string.h>
#include "simdefs.h"
#include "../../ffrdp.c"

// The harness builds this test twice: once against HEAD's ffrdp.c, once against the working tree's. HEAD
// has no timeout argument on send/recv and the working tree does, so the test calls these wrappers -- both
// sides compile, and both sides mean "do not wait", which is exactly what HEAD does. run.sh defines
// FFRDP_API_HAS_TIMEOUT for the side that has it.
#ifdef FFRDP_API_HAS_TIMEOUT
#define TEST_SEND(c, b, l) ffrdp_send(c, b, l, 0)
#define TEST_RECV(c, b, l) ffrdp_recv(c, b, l, 0)
#else
#define TEST_SEND(c, b, l) ffrdp_send(c, b, l)
#define TEST_RECV(c, b, l) ffrdp_recv(c, b, l)
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
#ifdef FFRDP_API_HAS_ADDR
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

    sim_reset();
    sim_config(0, 0, 0, 0);
    c = TEST_INIT("127.0.0.1", 8001, 0, 1024);
    sim_begin(0);
    TEST_SEND(c, (char*)buf, 8 * 1024);
    TEST_RUN(c, 0);

    make_ack(ack, 1, 0x00, 255); sim_inject(0, ack, 8); TEST_RUN(c, 0);
    make_ack(ack, 1, 0x07, 255); sim_inject(0, ack, 8); TEST_RUN(c, 0);
    make_ack(ack, 1, 0x0a, 255); sim_inject(0, ack, 8); TEST_RUN(c, 0);

#ifdef NEW_COUNTERS
    printf("  ack_recv=%u ack_mack=%u ack_hole=%u (expect 3/2/1) -> %s\n",
           c->counter_ack_recv, c->counter_ack_mack, c->counter_ack_hole,
           (c->counter_ack_recv == 3 && c->counter_ack_mack == 2 && c->counter_ack_hole == 1) ? "PASS" : "FAIL");
#ifdef OOO_HIST
    // the masked bytes are counted by population, not by frame: 0x07 has three bits set, 0x0a has two,
    // so a sender that counted what it actually received has to read five.
    printf("  mack_bits_recv=%u (expect 5 = 3 from 0x07 + 2 from 0x0a) -> %s\n",
           c->counter_mack_bits_rx, c->counter_mack_bits_rx == 5 ? "PASS" : "FAIL");
    if (c->counter_mack_bits_rx != 5) return 1;
#endif
    return (c->counter_ack_recv == 3 && c->counter_ack_mack == 2 && c->counter_ack_hole == 1) ? 0 : 1;
#else
    printf("  counter_ack_* does not exist in this build\n");
    return 0;
#endif
}
