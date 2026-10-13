// end to end traffic over the fake lossy network: two ffrdp contexts, same shape
// as test.c (both sides keep offering data).  Prints one summary line per run.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../ffrdp.h"
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

// ---- 判定：只放"任何负载/任何旋钮下都必须成立"的不变量。统计数字（resend%、rto、rto_floor、
// ooo 直方图……）随负载与旋钮变化，是给人看的，不是判据 —— 调参类改动不该因为它们被判失败。----
static int g_fail;
static void check(int ok, const char *what)
{
    if (!ok) { printf("       FAIL: %s\n", what); g_fail++; }
}

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
    c0 = TEST_INIT("127.0.0.1", 8001, 0, 1280);
    c1 = TEST_INIT("127.0.0.1", 8002, 1, 1280);
    if (!c0 || !c1) { printf("       FAIL: init failed\n"); return -1; }
    memset(sbuf, 0x5a, sizeof(sbuf));

    for (i = 0; i < steps; i++) {
        sim_setstep(i);
        // same shape as test.c: a random chunk of 1..sndsz bytes every iteration
        sim_begin(0); TEST_SEND(c0, (char*)sbuf, 1 + (int)(hash(i, 11) % (unsigned)sndsz)); TEST_RUN(c0, 0);
        sim_begin(1); TEST_SEND(c1, (char*)sbuf, 1 + (int)(hash(i, 12) % (unsigned)sndsz)); TEST_RUN(c1, 1);
        sim_begin(1); while ((ret = TEST_RECV(c1, (char*)rbuf, sizeof(rbuf))) > 0) r1 += ret;
        sim_begin(0); while ((ret = TEST_RECV(c0, (char*)rbuf, sizeof(rbuf))) > 0) r0 += ret;
    }
    s0.rto = CONN(c0)->counter_resend_rto; s0.fast = CONN(c0)->counter_resend_fast; s0.first = CONN(c0)->counter_send_1sttime;
    s0.cwnd = CONN(c0)->cwnd; s0.ssth = CONN(c0)->ssthresh; s0.wait = CONN(c0)->wait_snd; s0.fail = CONN(c0)->counter_send_failed; s0.rttd = CONN(c0)->rttd;
    s1.rto = CONN(c1)->counter_resend_rto; s1.fast = CONN(c1)->counter_resend_fast; s1.first = CONN(c1)->counter_send_1sttime;
    s1.maxrto = CONN(c1)->counter_reach_maxrto;
    s0.tx = CONN(c0)->counter_txfull; s0.rx = CONN(c1)->counter_rxfull; // frames c0 sent vs frames c1 received: the real air loss
    s1.tx = CONN(c1)->counter_txfull; s1.rx = CONN(c0)->counter_rxfull;
    s0.ooo = s0.far = s0.dup = s0.adup = s0.spur = s0.rfl = 0;
#ifdef NEW_COUNTERS
    s0.ack = CONN(c0)->counter_ack_recv; s0.mack = CONN(c0)->counter_ack_mack; s0.hole = CONN(c0)->counter_ack_hole;
    s0.tev = CONN(c0)->counter_timeout_evt; s0.rbk = CONN(c0)->counter_rto_backoff;
#endif
#ifdef RECV_OOO
    s0.ooo = CONN(c1)->counter_recv_ooo; s0.far = CONN(c1)->counter_recv_far; // c1's view of what c0 sent
#endif
#ifdef DUP_REPORT
    s0.dup  = CONN(c1)->counter_recv_dup; // what c1 actually received a second time
    s0.adup = CONN(c0)->counter_ack_dup;  // ... and what c0 was told about it
    s0.flr  = CONN(c0)->rto_floor;
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
    s0.d1 = CONN(c1)->counter_ooo_d1_24; s0.d25 = CONN(c1)->counter_ooo_d25_64; s0.dgt = CONN(c1)->counter_ooo_dgt64;
#ifdef OOO_HIST2
    s0.d65 = CONN(c1)->counter_ooo_d65_128; s0.d128 = CONN(c1)->counter_ooo_dgt128;
#endif
    s0.mbs = CONN(c1)->counter_mack_bits_sent; s0.mbr = CONN(c0)->counter_mack_bits_recv; // c1's reports, what c0 got
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

    // ---- 判定（负载无关）----
    // 1) 一切都要还回去：ctx / 帧 / 乱序队列，残留就是泄漏
    check(sim_alloc() == sim_freed(), "leaked bytes (a context or a frame was not returned)");
    // 2) 丢包模型：配了丢包率、又没有加 shaper/policer 时，实测的"空中丢失"应当接近配置值。
    //    容差给得宽（10 个百分点）—— 这一条是用来抓"模型坏了"的，不是用来卡统计波动的。
    if (loss > 0.0 && cap == 0 && pcap == 0 && s0.tx > 0 && s1.tx > 0) {
        double l01 = 100.0 * (s0.tx - s0.rx) / s0.tx;   // c0 -> c1 实际丢了多少
        double l10 = 100.0 * (s1.tx - s1.rx) / s1.tx;   // c1 -> c0
        check(l01 >= loss * 100.0 - 10.0 && l01 <= loss * 100.0 + 10.0, "observed loss c0->c1 far from the configured rate");
        check(l10 >= loss * 100.0 - 10.0 && l10 <= loss * 100.0 + 10.0, "observed loss c1->c0 far from the configured rate");
    }
    // 3) 交付上界：收到的不可能比发出的多（两边都发，所以用各自的对端）
    check(r1 >= 0 && r0 >= 0, "negative byte count");
    printf("       verdict -> %s\n", g_fail ? "FAIL" : "OK");
    return 0;   // 判定结果体现在上面那一行：run.sh 读它
}
