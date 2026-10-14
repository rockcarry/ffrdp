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
#ifdef FFRDP_API_V2
// v2 接口：send/recv 多了连接句柄（单连接测试用 0），init 收配置串，退出叫 exit。
#define TEST_SEND(c, b, l) ffrdp_send(c, 0, b, l, 0)
#define TEST_RECV(c, b, l) ffrdp_recv(c, 0, b, l, 0)
#define TEST_FREE(c)       ffrdp_exit(c)
// ── 应用句柄（v2 多连接）────────────────────────────────────────────────
// 服务端**没有**现成的句柄：它必须先被 accept，拿到 FFRDP_ACCEPT 报出来的那个
// 句柄，才有地方收发 ✓（客户端就是 0 号主连接 ✓）。这个小组件把它记下来，
// 两侧于是写成同一句话 ⇒ 老场景（握手关、只有 0 号）行为一字不变 ✓。
struct e2e_app { int conn; };
static long e2e_event(void *cbctx, const FFRDP_EVENT *ev)
{
    // 持着协议锁被调用 ⇒ 只记句柄，绝不回调 ffrdp_*（会自锁）✓
    struct e2e_app *a = (struct e2e_app*)cbctx;
    if (a && ev->type == FFRDP_ACCEPT) a->conn = ev->conn;
    return FFRDP_NOT_CONSUMED;
}
#define APP_CONN(c, h)          (((FFRDPCONTEXT*)(c))->table[h])   // 按句柄取连接
#define TEST_SEND_H(c, h, b, l) ffrdp_send(c, h, b, l, 0)
#define TEST_RECV_H(c, h, b, l) ffrdp_recv(c, h, b, l, 0)
#elif defined(FFRDP_API_HAS_TIMEOUT)
#define TEST_SEND(c, b, l) ffrdp_send(c, b, l, 0)
#define TEST_RECV(c, b, l) ffrdp_recv(c, b, l, 0)
#define TEST_FREE(c)       ffrdp_free(c)
struct e2e_app { int conn; };            // 老接口：没有句柄这回事
#define APP_CONN(c, h)          CONN(c)
#define TEST_SEND_H(c, h, b, l) ffrdp_send(c, b, l, 0)
#define TEST_RECV_H(c, h, b, l) ffrdp_recv(c, b, l, 0)
#else
#define TEST_SEND(c, b, l) ffrdp_send(c, b, l)
#define TEST_RECV(c, b, l) ffrdp_recv(c, b, l)
#define TEST_FREE(c)       ffrdp_free(c)
struct e2e_app { int conn; };
#define APP_CONN(c, h)          CONN(c)
#define TEST_SEND_H(c, h, b, l) ffrdp_send(c, b, l)
#define TEST_RECV_H(c, h, b, l) ffrdp_recv(c, b, l)
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
#ifdef FFRDP_API_V2
static void *TEST_INIT_CB(const char *ip, int port, int server, int mss, void *cbctx)
{
    char params[128];
    // E2E_HS=1 ⇒ 强制握手；E2E_HS=0 ⇒ 强制老路径（用来钉"老路径一字未变" ✓）；
    // 不设 ⇒ 跟随协议默认（v2 起默认就是握手 ✓）。
    const char *hs = getenv("E2E_HS");
    snprintf(params, sizeof(params), "%s://%s:%d,mss:%d,fec:0%s",
             server ? "ffrdps" : "ffrdpc", ip, port, mss,
             hs ? (atoi(hs) ? ",hs:1" : ",hs:0") : "");
    return ffrdp_init(params, e2e_event, cbctx);
}
#elif defined(FFRDP_API_HAS_ADDR)
static void *TEST_INIT_CB(const char *ip, int port, int server, int mss, void *cbctx)
{
    char addr[64];
    (void)cbctx;
    snprintf(addr, sizeof(addr), "%s:%d", ip, port);
    return ffrdp_init(addr, NULL, NULL, server, mss, 0);
}
#else
#define TEST_INIT_CB(ip, port, server, mss, cbctx) ffrdp_init((char *)(ip), port, NULL, NULL, server, mss, 0)
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
    struct e2e_app app0 = { 0 }, app1 = { 0 };
    int i, ret, h0 = 0, h1 = 0;   // 应用句柄：客户端恒 0；服务端等 ACCEPT ✓

    sim_reset();
    sim_config(loss, burst, delay, jit);
    if (cap  > 0) sim_shaper (cap  * 1024.0 / 1000.0, qmax * 1024);  // KB/s -> bytes per ms
    if (pcap > 0) sim_policer(pcap * 1024.0 / 1000.0, pburst * 1024);
    if (spikep > 0) sim_spike(spikep, spikem);
    c0 = TEST_INIT_CB("127.0.0.1", 8001, 0, 1280, &app0);
    c1 = TEST_INIT_CB("127.0.0.1", 8002, 1, 1280, &app1);
    if (!c0 || !c1) { printf("       FAIL: init failed\n"); return -1; }
    // ── 地址登记（P2 第二步）──────────────────────────────────────────────
    // 协议按 (ip<<16)|port 表示地址 ✓、按地址查表分发 ✓ ⇒ 仿真必须报**协议真在用的值** ✓：
    //   slot 1（服务端）发出的帧带 c0 认为的对端地址 ✓（否则 c0 会把回包当"别人的包"丢掉 ✗）；
    //   slot 0（客户端）发出的帧带客户端自己的地址 ✓ —— c1 从第一帧学会它、之后回包都用它 ✓。
    // 两侧登记**同一个**地址：在这套两端夹具里，双方"眼中的对端"都是 c0 的 peer_addr ✓。
    // ⚠️ 别给某一侧登记自造地址 ✗：走握手时，服务端记下的是它**收到 SYN 时**的地址，
    // 之后的数据帧必须带同一个值，否则按地址分发会把它当"别人的包"丢掉 ✓。
    sim_slot(1, APP_CONN(c0, h0)->peer_addr);
    sim_slot(0, APP_CONN(c0, h0)->peer_addr);
    memset(sbuf, 0x5a, sizeof(sbuf));

    for (i = 0; i < steps; i++) {
        h0 = app0.conn; h1 = app1.conn;   // 服务端：ACCEPT 之前是 0（那时 send 会被拒 ✓）
        sim_setstep(i);
        // same shape as test.c: a random chunk of 1..sndsz bytes every iteration
        sim_begin(0); TEST_SEND_H(c0, h0, (char*)sbuf, 1 + (int)(hash(i, 11) % (unsigned)sndsz)); TEST_RUN(c0, 0);
        sim_begin(1); TEST_SEND_H(c1, h1, (char*)sbuf, 1 + (int)(hash(i, 12) % (unsigned)sndsz)); TEST_RUN(c1, 1);
        sim_begin(1); while ((ret = TEST_RECV_H(c1, h1, (char*)rbuf, sizeof(rbuf))) > 0) r1 += ret;
        sim_begin(0); while ((ret = TEST_RECV_H(c0, h0, (char*)rbuf, sizeof(rbuf))) > 0) r0 += ret;
    }
    s0.rto = APP_CONN(c0, h0)->counter_resend_rto; s0.fast = APP_CONN(c0, h0)->counter_resend_fast; s0.first = APP_CONN(c0, h0)->counter_send_1sttime;
    s0.cwnd = APP_CONN(c0, h0)->cwnd; s0.ssth = APP_CONN(c0, h0)->ssthresh; s0.wait = APP_CONN(c0, h0)->wait_snd; s0.fail = APP_CONN(c0, h0)->counter_send_failed; s0.rttd = APP_CONN(c0, h0)->rttd;
    s1.rto = APP_CONN(c1, h1)->counter_resend_rto; s1.fast = APP_CONN(c1, h1)->counter_resend_fast; s1.first = APP_CONN(c1, h1)->counter_send_1sttime;
    s1.maxrto = APP_CONN(c1, h1)->counter_reach_maxrto;
    s0.tx = APP_CONN(c0, h0)->counter_txfull; s0.rx = APP_CONN(c1, h1)->counter_rxfull; // frames c0 sent vs frames c1 received: the real air loss
    s1.tx = APP_CONN(c1, h1)->counter_txfull; s1.rx = APP_CONN(c0, h0)->counter_rxfull;
    s0.ooo = s0.far = s0.dup = s0.adup = s0.spur = s0.rfl = 0;
#ifdef NEW_COUNTERS
    s0.ack = APP_CONN(c0, h0)->counter_ack_recv; s0.mack = APP_CONN(c0, h0)->counter_ack_mack; s0.hole = APP_CONN(c0, h0)->counter_ack_hole;
    s0.tev = APP_CONN(c0, h0)->counter_timeout_evt; s0.rbk = APP_CONN(c0, h0)->counter_rto_backoff;
#endif
#ifdef RECV_OOO
    s0.ooo = APP_CONN(c1, h1)->counter_recv_ooo; s0.far = APP_CONN(c1, h1)->counter_recv_far; // c1's view of what c0 sent
#endif
#ifdef DUP_REPORT
    s0.dup  = APP_CONN(c1, h1)->counter_recv_dup; // what c1 actually received a second time
    s0.adup = APP_CONN(c0, h0)->counter_ack_dup;  // ... and what c0 was told about it
    s0.flr  = APP_CONN(c0, h0)->rto_floor;
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
    s0.d1 = APP_CONN(c1, h1)->counter_ooo_d1_24; s0.d25 = APP_CONN(c1, h1)->counter_ooo_d25_64; s0.dgt = APP_CONN(c1, h1)->counter_ooo_dgt64;
#ifdef OOO_HIST2
    s0.d65 = APP_CONN(c1, h1)->counter_ooo_d65_128; s0.d128 = APP_CONN(c1, h1)->counter_ooo_dgt128;
#endif
    s0.mbs = APP_CONN(c1, h1)->counter_mack_bits_tx; s0.mbr = APP_CONN(c0, h0)->counter_mack_bits_rx; // c1's reports, what c0 got
#endif
    TEST_FREE(c0); TEST_FREE(c1); // everything must be returned, so the residual is a leak
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
    //    **握上手了**就必须真的传过数据 ✓：判据用"服务端拿到了 accept 句柄"（app1.conn != 0）
    //    来界定 ⇒ 老路径（不下发 accept）与连不上的一轮都不适用 ✓，不靠环境变量猜 ✓。
    //    （曾经没有这条：一个字节没传也判 OK ✗。）
    if (app1.conn != 0) {
        check(r1 > 0 && r0 > 0, "handshake completed but no application data got through");
    }
    check(r1 >= 0 && r0 >= 0, "negative byte count");
    printf("       verdict -> %s\n", g_fail ? "FAIL" : "OK");
    return 0;   // 判定结果体现在上面那一行：run.sh 读它
}
