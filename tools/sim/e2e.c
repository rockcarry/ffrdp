// end to end traffic over the fake lossy network: two ffrdp contexts, same shape
// as test.c (both sides keep offering data).  Prints one summary line per run.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../ffrdp.h"
#include "simdefs.h"
#include "../../ffrdp.c"

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
    static unsigned char sbuf[64 * 1024], rbuf[64 * 1024];
    struct { unsigned rto, fast, cwnd, ssth, wait, fail, rttd, maxrto, first, ack, mack, hole, tev, rbk, inflight, tx, rx; } s0, s1;
    FFRDPCONTEXT *c0, *c1;
    unsigned long r0 = 0, r1 = 0;
    int i, ret;

    sim_reset();
    sim_config(loss, burst, delay, jit);
    if (cap > 0) sim_shaper(cap * 1024.0 / 1000.0, qmax * 1024); // KB/s -> bytes per ms
    c0 = ffrdp_init("127.0.0.1", 8001, NULL, NULL, 0, 1280, 0);
    c1 = ffrdp_init("127.0.0.1", 8002, NULL, NULL, 1, 1280, 0);
    if (!c0 || !c1) { printf("init failed\n"); return -1; }
    memset(sbuf, 0x5a, sizeof(sbuf));

    for (i = 0; i < steps; i++) {
        sim_setstep(i);
        // same shape as test.c: a random chunk of 1..sndsz bytes every iteration
        sim_begin(0); ffrdp_send(c0, (char*)sbuf, 1 + (int)(hash(i, 11) % (unsigned)sndsz)); ffrdp_update(c0);
        sim_begin(1); ffrdp_send(c1, (char*)sbuf, 1 + (int)(hash(i, 12) % (unsigned)sndsz)); ffrdp_update(c1);
        sim_begin(1); while ((ret = ffrdp_recv(c1, (char*)rbuf, sizeof(rbuf))) > 0) r1 += ret;
        sim_begin(0); while ((ret = ffrdp_recv(c0, (char*)rbuf, sizeof(rbuf))) > 0) r0 += ret;
    }
    s0.rto = c0->counter_resend_rto; s0.fast = c0->counter_resend_fast; s0.first = c0->counter_send_1sttime;
    s0.cwnd = c0->cwnd; s0.ssth = c0->ssthresh; s0.wait = c0->wait_snd; s0.fail = c0->counter_send_failed; s0.rttd = c0->rttd;
    s1.rto = c1->counter_resend_rto; s1.fast = c1->counter_resend_fast; s1.first = c1->counter_send_1sttime;
    s1.maxrto = c1->counter_reach_maxrto;
    s0.tx = c0->counter_txfull; s0.rx = c1->counter_rxfull; // frames c0 sent vs frames c1 received: the real air loss
    s1.tx = c1->counter_txfull; s1.rx = c0->counter_rxfull;
#ifdef NEW_COUNTERS
    s0.ack = c0->counter_ack_recv; s0.mack = c0->counter_ack_mack; s0.hole = c0->counter_ack_hole;
    s0.tev = c0->counter_timeout_evt; s0.rbk = c0->counter_rto_backoff;
#endif
#ifdef HAVE_INFLIGHT
    s0.inflight = c0->inflight;
#endif
    ffrdp_free(c0); ffrdp_free(c1); // everything must be returned, so the residual is a leak
    printf("loss=%.3f burst=%d delay=%d jit=%d cap=%.0fKB/s q=%dKB | "
           "c0 recv=%6.2fMB resend=%5.2f%% (rto %4u fast %4u) cwnd=%2u ssth=%2u wait=%3u fail=%4u rttd=%u | "
           "c1 recv=%6.2fMB resend=%5.2f%% maxrto=%u leaked=%lu\n",
           loss, burst, delay, jit, cap, qmax,
           r1 / 1048576.0, 100.0 * (s0.rto + s0.fast) / (s0.first ? s0.first : 1),
           s0.rto, s0.fast, s0.cwnd, s0.ssth, s0.wait, s0.fail, s0.rttd,
           r0 / 1048576.0, 100.0 * (s1.rto + s1.fast) / (s1.first ? s1.first : 1),
           s1.maxrto, sim_alloc() - sim_freed());
#ifdef NEW_COUNTERS
    printf("       c0 ack_recv=%u mack=%u hole=%u timeout_evt=%u rto_backoff=%u\n",
           s0.ack, s0.mack, s0.hole, s0.tev, s0.rbk);
#endif
    printf("       air loss c0->c1=%.2f%% (%u/%u)  c1->c0=%.2f%% (%u/%u)\n",
           100.0 * (s0.tx - s0.rx) / (s0.tx ? s0.tx : 1), s0.rx, s0.tx,
           100.0 * (s1.tx - s1.rx) / (s1.tx ? s1.tx : 1), s1.rx, s1.tx);
#ifdef HAVE_INFLIGHT
    printf("       c0 inflight=%u\n", s0.inflight);
#endif
    return 0;
}
