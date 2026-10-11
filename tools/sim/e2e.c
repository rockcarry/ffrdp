// end to end traffic over the fake lossy network: two ffrdp contexts, same shape
// as test.c (both sides keep offering data).  Prints one summary line per run.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../ffrdp.h"
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
// handed over as one RECV, then one TIMER. idx must be the number the test passes to sim_begin();
// with two contexts the two endpoints are 0 and 1.
#ifdef FFRDP_LL_DRIVEN
#define TEST_RUN(c, idx) sim_ll_run(idx, c)
#else
#define TEST_RUN(c, idx) ffrdp_update(c)
#endif

static unsigned hash(unsigned k, unsigned salt)
{
    unsigned h = k * 2654435761u + salt * 40503u + 12345u;
    h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
    return h;
}

int main(int argc, char *argv[])
{
    double loss  = argc > 1 ? atof(argv[1]) : 0.0;
    int    burst = argc > 2 ? atoi(argv[2]) : 0;
    int    delay = argc > 3 ? atoi(argv[3]) : 4;
    int    jit   = argc > 4 ? atoi(argv[4]) : 0;
    int    steps = argc > 5 ? atoi(argv[5]) : 6000;
    int    sndsz = argc > 6 ? atoi(argv[6]) : 8192;
    double cap   = argc > 7 ? atof(argv[7]) : 0;   // KB/s, shared shaper rate, 0 = unlimited
    int    qmax  = argc > 8 ? atoi(argv[8]) : 0;   // KB,   shared shaper queue limit
    double pcap  = argc > 9 ? atof(argv[9]) : 0;   // KB/s, uplink token bucket rate (side 0), 0 = unlimited
    int    pburst= argc > 10 ? atoi(argv[10]) : 0; // KB,   token bucket depth
    int    spikep= argc > 11 ? atoi(argv[11]) : 0; // %,    share of packets hit by a delay spike
    int    spikem= argc > 12 ? atoi(argv[12]) : 0; // ms,   extra delay of a spike
    static unsigned char sbuf[64 * 1024], rbuf[64 * 1024];
    struct { unsigned rto, fast, cwnd, ssth, wait, fail, rttd, maxrto, first, ack, mack, hole, tev, rbk, inflight, tx, rx, ooo, far, dup, adup, spur, rfl, flr, d1, d25, dgt, d65, d128, mbs, mbr; } s0, s1;
    FFRDPCONTEXT *c0, *c1;
    unsigned long r0 = 0, r1 = 0;
    int i, ret;

    sim_reset();
    sim_config(loss, burst, delay, jit);
    if (cap  > 0) sim_shaper (cap  * 1024.0 / 1000.0, qmax * 1024);  // KB/s -> bytes per ms
    if (pcap > 0) sim_policer(pcap * 1024.0 / 1000.0, pburst * 1024);
    if (spikep > 0) sim_spike(spikep, spikem);
    c0 = ffrdp_init("127.0.0.1", 8001, NULL, NULL, 0, 1280, 0);
    c1 = ffrdp_init("127.0.0.1", 8002, NULL, NULL, 1, 1280, 0);
    if (!c0 || !c1) { printf("init failed\n"); return -1; }
    memset(sbuf, 0x5a, sizeof(sbuf));

    for (i = 0; i < steps; i++) {
        sim_setstep(i);
        // same shape as test.c: a random chunk of 1..sndsz bytes every iteration
        sim_begin(0); TEST_SEND(c0, (char*)sbuf, 1 + (int)(hash(i, 11) % (unsigned)sndsz)); TEST_RUN(c0, 0);
        sim_begin(1); TEST_SEND(c1, (char*)sbuf, 1 + (int)(hash(i, 12) % (unsigned)sndsz)); TEST_RUN(c1, 1);
        sim_begin(1); while ((ret = TEST_RECV(c1, (char*)rbuf, sizeof(rbuf))) > 0) r1 += ret;
        sim_begin(0); while ((ret = TEST_RECV(c0, (char*)rbuf, sizeof(rbuf))) > 0) r0 += ret;
    }
    s0.rto = c0->counter_resend_rto; s0.fast = c0->counter_resend_fast; s0.first = c0->counter_send_1sttime;
    s0.cwnd = c0->cwnd; s0.ssth = c0->ssthresh; s0.wait = c0->wait_snd; s0.fail = c0->counter_send_failed; s0.rttd = c0->rttd;
    s1.rto = c1->counter_resend_rto; s1.fast = c1->counter_resend_fast; s1.first = c1->counter_send_1sttime;
    s1.maxrto = c1->counter_reach_maxrto;
    s0.tx = c0->counter_txfull; s0.rx = c1->counter_rxfull; // frames c0 sent vs frames c1 received: the real air loss
    s1.tx = c1->counter_txfull; s1.rx = c0->counter_rxfull;
    s0.ooo = s0.far = s0.dup = s0.adup = s0.spur = s0.rfl = 0;
#ifdef NEW_COUNTERS
    s0.ack = c0->counter_ack_recv; s0.mack = c0->counter_ack_mack; s0.hole = c0->counter_ack_hole;
    s0.tev = c0->counter_timeout_evt; s0.rbk = c0->counter_rto_backoff;
#endif
#ifdef RECV_OOO
    s0.ooo = c1->counter_recv_ooo; s0.far = c1->counter_recv_far; // c1's view of what c0 sent
#endif
#ifdef DUP_REPORT
    s0.dup  = c1->counter_recv_dup; // what c1 actually received a second time
    s0.adup = c0->counter_ack_dup;  // ... and what c0 was told about it
    s0.flr  = c0->rto_floor;
#endif
#ifdef DUP_REPORT
    printf("       rto_floor=%u ms (the peer called %u of our retransmits pointless)\n", s0.flr, s0.adup);
#endif
#ifdef SPUR
    s0.spur = c0->counter_rto_spurious; // pointless retransmits proven by timing
#endif
#ifdef RTO_FLOOR
    s0.rfl = c0->counter_rto_floor;     // only exists in builds that carry the rto floor experiment
#endif
#ifdef HAVE_INFLIGHT
    s0.inflight = c0->inflight;
#endif
#ifdef OOO_HIST
    // read these before the contexts are returned: the summary below prints after they are gone
    s0.d1 = c1->counter_ooo_d1_24; s0.d25 = c1->counter_ooo_d25_64; s0.dgt = c1->counter_ooo_dgt64;
#ifdef OOO_HIST2
    s0.d65 = c1->counter_ooo_d65_128; s0.d128 = c1->counter_ooo_dgt128;
#endif
    s0.mbs = c1->counter_mack_bits_sent; s0.mbr = c0->counter_mack_bits_recv; // c1's reports, what c0 got
#endif
    ffrdp_free(c0); ffrdp_free(c1); // everything must be returned, so the residual is a leak
    printf("loss=%.3f burst=%d delay=%d jit=%d cap=%.0fKB/s q=%dKB pol=%.0fKB/s/%dKB spike=%d%%/%dms | "
           "c0 recv=%6.2fMB resend=%5.2f%% (rto %4u fast %4u) cwnd=%2u ssth=%2u wait=%3u fail=%4u rttd=%u | "
           "c1 recv=%6.2fMB resend=%5.2f%% maxrto=%u leaked=%lu\n",
           loss, burst, delay, jit, cap, qmax, pcap, pburst, spikep, spikem,
           r1 / 1048576.0, 100.0 * (s0.rto + s0.fast) / (s0.first ? s0.first : 1),
           s0.rto, s0.fast, s0.cwnd, s0.ssth, s0.wait, s0.fail, s0.rttd,
           r0 / 1048576.0, 100.0 * (s1.rto + s1.fast) / (s1.first ? s1.first : 1),
           s1.maxrto, sim_alloc() - sim_freed());
#ifdef NEW_COUNTERS
    printf("       c0 ack_recv=%u mack=%u hole=%u timeout_evt=%u rto_backoff=%u | peer recv_ooo=%u recv_far=%u | dup=%u ack_dup=%u\n",
           s0.ack, s0.mack, s0.hole, s0.tev, s0.rbk, s0.ooo, s0.far, s0.dup, s0.adup);
    printf("       c0 spur=%u rto_floor_raises=%u\n", s0.spur, s0.rfl);
#endif
#ifdef OOO_HIST
    // c1 is the receiver of what c0 sent: its histogram says how far the out of order arrivals were
    // from the hole, and the two bit counts say whether the reports made it across (they only differ
    // when an ack carrying bits was lost).
#ifdef OOO_HIST2
    printf("       c1 ooo dist 1..24=%u 25..64=%u 65..128=%u >128=%u | mack bits sent(c1)=%u recv(c0)=%u\n",
           s0.d1, s0.d25, s0.d65, s0.d128, s0.mbs, s0.mbr);
#else
    printf("       c1 ooo dist 1..24=%u 25..64=%u | mack bits sent(c1)=%u recv(c0)=%u\n",
           s0.d1, s0.d25, s0.mbs, s0.mbr);
#endif
#endif
    printf("       air loss c0->c1=%.2f%% (%u/%u)  c1->c0=%.2f%% (%u/%u)\n",
           100.0 * (s0.tx - s0.rx) / (s0.tx ? s0.tx : 1), s0.rx, s0.tx,
           100.0 * (s1.tx - s1.rx) / (s1.tx ? s1.tx : 1), s1.rx, s1.tx);
#ifdef HAVE_INFLIGHT
    printf("       c0 inflight=%u\n", s0.inflight);
#endif
    return 0;
}
