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
    c = ffrdp_init("127.0.0.1", 8001, NULL, NULL, 0, 1024, 0);
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
        printf(" %u", c->rto);
    }
    printf("\n  retransmits=%u reach_maxrto=%u\n", c->counter_resend_rto, c->counter_reach_maxrto);
#ifdef NEW_COUNTERS
    printf("  timeout_evt=%u rto_backoff=%u (12 timeouts, but the rto only backs off once)\n",
           c->counter_timeout_evt, c->counter_rto_backoff);
#endif
    return 0;
}
