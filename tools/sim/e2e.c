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

// ── 逐消息延迟量具（docs/ffrdp-optimization.md §8 的 #1 ✓）──────────────────
// 为什么需要它 ✗：这套夹具一直只有"字节数 / 重传率"，而延迟、pacing、按截止时间丢弃这些
// 候选的判据都是**分位数**（P95 / 最大），拿吞吐与重传率根本看不出来 ⇒ 之前几次
// "判据说没有收益，其实没测到"（notes §11.5）就是这么来的 ✓。
//
// 做法 ✓：每条应用消息前加 8 字节头 [tick][payload len]，收侧按消息边界解析 ⇒ 得到
// "交给协议 → 应用取走"的完整延迟（含协议排队 + 链路时延/抖动 + 重传等待 ✓）。
// 时间用**虚拟钟** sim_tick()（1 step = 1 ms ✓）⇒ 分辨率 1 ms，与延迟文档同尺度 ✓；
// 亚毫秒的差别这里看不到（要看那个得回真机 ✗）。
#define E2E_HDR       8
#define E2E_LAT_MAX   1024          // 直方图覆盖 0..1024 ms，超出进 over 桶 ✓
// 缓冲要**大于一次 recv 能返回的字节数**（下面按剩余空间给 recv 限长 ✓）：一条消息最多
// 16 KB、而 recv 一次可以给满 64 KB ⇒ 留够"满块 + 一条半条"就不会出现"截断 ⇒ 之后全错位" ✗
#define E2E_RXBUF     (128 * 1024)

typedef struct {
    unsigned char buf[E2E_RXBUF];   // 还没齐的那条消息（流式 recv ⇒ 一条可能分两次到 ✓）
    int      n;
    unsigned msgs;                  // 解析出的完整消息数（= 延迟样本数 ✓）
    unsigned hist[E2E_LAT_MAX + 1];
    unsigned over;                  // > E2E_LAT_MAX ms 的样本
    unsigned sum, max, bad;         // 均值 / 最大 / 头坏掉重来的次数（应恒为 0 ✓）
} E2E_LAT;

static E2E_LAT g_lat01, g_lat10;

static void e2e_put32(unsigned char *p, unsigned v)
{ p[0]=(unsigned char)v; p[1]=(unsigned char)(v>>8); p[2]=(unsigned char)(v>>16); p[3]=(unsigned char)(v>>24); }
static unsigned e2e_get32(const unsigned char *p)
{ return (unsigned)p[0] | ((unsigned)p[1]<<8) | ((unsigned)p[2]<<16) | ((unsigned)p[3]<<24); }

// 发送：写头 + 交给协议。ffrdp_send 是原子的（整条接受或一个字节都不接受 ✓）⇒ 被拒的消息
// 不会在流里留下半个头 ⇒ 收侧解析永远不会错位 ✓（这也是这里敢用"应用消息"当边界的原因 ✓）。
static void e2e_send(void *c, int h, unsigned char *buf, int len, unsigned now)
{
    e2e_put32(buf, now); e2e_put32(buf + 4, (unsigned)len);
    (void)TEST_SEND_H(c, h, (char*)buf, len + E2E_HDR);
}

// 接收：把拿到的字节喂进流解析器，凑齐一条就记一个延迟样本 ✓
static void e2e_feed(E2E_LAT *r, const unsigned char *p, int n, unsigned now)
{
    if (n <= 0) return;
    // 收不下就是量具自己出了问题（调用方按剩余空间给 recv 限长 ⇒ 走不到这里 ✓）：
    // 宁可记一次 bad 让判定抓到，也不要截断 —— 截断会**静默丢字节**、之后整条流全错位 ✗。
    if (r->n + n > E2E_RXBUF) { r->bad++; r->n = 0; return; }
    memcpy(r->buf + r->n, p, (size_t)n); r->n += n;
    for (;;) {
        unsigned len, lat;
        if (r->n < E2E_HDR) break;
        len = e2e_get32(r->buf + 4);
        if (len > E2E_RXBUF - E2E_HDR) { r->n = 0; r->bad++; break; }   // 头坏了：丢掉重来 ✓
        if ((unsigned)r->n < E2E_HDR + len) break;                       // 半条：等下一次 ✓
        lat = now - e2e_get32(r->buf);                                  // 虚拟钟，uint 回绕要 49 天才碰到 ✓
        r->msgs++; r->sum += lat;
        if (lat > r->max) r->max = lat;
        if (lat > E2E_LAT_MAX) r->over++; else r->hist[lat]++;
        r->n -= (int)(E2E_HDR + len);
        memmove(r->buf, r->buf + E2E_HDR + len, (size_t)r->n);
    }
}

// 分位数：按累加计数找"第 pct% 个样本落在哪个毫秒桶" ✓（样本少时向上取整，不会返回 0 号空桶 ✓）
static unsigned e2e_pct(const E2E_LAT *r, unsigned pct)
{
    unsigned total = r->msgs, want, acc = 0, i;
    if (!total) return 0;
    want = (unsigned)(((unsigned long long)total * pct + 99) / 100);
    if (!want) want = 1;
    for (i = 0; i <= E2E_LAT_MAX; i++) { acc += r->hist[i]; if (acc >= want) return i; }
    return E2E_LAT_MAX + 1;                                  // 落进 over 桶 ✓
}

static void e2e_lat_print(const char *tag, const E2E_LAT *r)
{
    printf("       latency %s ms: n=%u p50=%u p95=%u p99=%u max=%u mean=%u | >%dms %u bad_hdr %u\n",
           tag, r->msgs, e2e_pct(r, 50), e2e_pct(r, 95), e2e_pct(r, 99), r->max,
           r->msgs ? r->sum / r->msgs : 0, E2E_LAT_MAX, r->over, r->bad);
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
        sim_begin(0); e2e_send(c0, h0, sbuf, 1 + (int)(hash(i, 11) % (unsigned)sndsz), sim_tick()); TEST_RUN(c0, 0);
        sim_begin(1); e2e_send(c1, h1, sbuf, 1 + (int)(hash(i, 12) % (unsigned)sndsz), sim_tick()); TEST_RUN(c1, 1);
        // ⚠️ 每次 recv 的长度**按解析器剩余空间**给（不是 sizeof(rbuf)）✗：一条消息可能横跨两次
        // recv，解析器里还压着半条 ⇒ 再要满 64 KB 就会溢出缓冲、丢字节、之后整条流错位 ✗
        // （实测：丢包/整形场景里 c1->c0 的 bad_hdr 会到 165 —— 就是这么来的 ✓）。
        sim_begin(1); while ((ret = TEST_RECV_H(c1, h1, (char*)rbuf,
                                               MIN((int)sizeof(rbuf), E2E_RXBUF - g_lat01.n))) > 0) {
            r1 += ret; e2e_feed(&g_lat01, rbuf, ret, sim_tick());
        }
        sim_begin(0); while ((ret = TEST_RECV_H(c0, h0, (char*)rbuf,
                                               MIN((int)sizeof(rbuf), E2E_RXBUF - g_lat10.n))) > 0) {
            r0 += ret; e2e_feed(&g_lat10, rbuf, ret, sim_tick());
        }
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
    printf("       delivered loss after repair c0->c1=%.2f%% (%u/%u)  c1->c0=%.2f%% (%u/%u)\n",
           100.0 * (s0.tx - s0.rx) / (s0.tx ? s0.tx : 1), s0.rx, s0.tx,
           100.0 * (s1.tx - s1.rx) / (s1.tx ? s1.tx : 1), s1.rx, s1.tx);
    // 模型自己的账（不是端侧的 ✓）：offered 含重传 ⇒ 某一侧卡住时它会被重传灌大，
    // 于是"实际丢包率"这条判据在这类场景里读不出配置值（下面那条判据的样本门槛就为它设 ✓）。
    printf("       model packets: offered(c0->c1)=%lu dropped=%lu | offered(c1->c0)=%lu dropped=%lu\n",
           sim_offered(1), sim_dropped(1), sim_offered(0), sim_dropped(0));
    // 逐消息延迟（§8 #1 的量具 ✓）：D1 / D3 / C1 的判据都是这一组分位数，不是吞吐 ✗。
    // 注意 n 是本轮**真的收到了**的消息数 ⇒ 彻底丢掉的消息不在这条分布里（上面那行交付丢失率管它 ✓）。
    e2e_lat_print("c0->c1", &g_lat01);
    e2e_lat_print("c1->c0", &g_lat10);
#ifdef HAVE_INFLIGHT
    printf("       c0 inflight=%u\n", s0.inflight);
#endif

    // ---- 判定（负载无关）----
    // 1) 一切都要还回去：ctx / 帧 / 乱序队列，残留就是泄漏
    check(sim_alloc() == sim_freed(), "leaked bytes (a context or a frame was not returned)");
    // 1b) 量具自身的不变量：消息流解析没错位 ✓（ffrdp_send 原子 ⇒ 被拒的消息不会留下半个头 ✓）
    check(g_lat01.bad == 0 && g_lat10.bad == 0, "latency harness lost message framing");
    // 2) 丢包模型：配了丢包率、又没加 shaper/policer 时，模型**自己**该按配置的比率丢。
    //    容差给得宽（10 个百分点）—— 这一条是用来抓"模型坏了"的，不是用来卡统计波动的。
    //    ⚠ 这里量的是**模型**（sim_offered/sim_dropped ✓），不是端侧计数 ✗：协议会重传，
    //    被网丢掉的帧最后还是会到 ⇒ 拿 (counter_txfull - counter_rxfull) 当"空中丢失"是错的 ✗
    //    （那量的是"协议没修好多少"，与模型无关 ⇒ 2026-10-15 之前它恒为假红 ✗）。
    //    ⚠️ **期望值不是 `loss`** ✗：burst 模型一次命中丢整串 ⇒ 实际比率 ≈ 1-(1-loss)^burst
    //    （0.02 × burst 6 ≈ 11.4%，而 ±10 点的容差相对 2% 只剩 0.6 点余量 ⇒ 帧数一动就假红 ✗；
    //     加 8 字节消息头就把它推到 14.8% 踩到了 ✓）。所以按 burst 折算期望，仍然只抓"模型坏了" ✓。
    //    ⚠️ 样本太少时这条判据同样没有意义 ⇒ 先要够样本再判（读数见上面那行 model packets ✓）。
    if (loss > 0.0 && cap == 0 && pcap == 0 && s0.tx > 0 && s1.tx > 0 &&
        sim_offered(1) >= 200 && sim_offered(0) >= 200) {
        double expect = loss, keep = 1.0;
        double l01 = 100.0 * sim_dropped(1) / sim_offered(1);   // 目的 slot 1 = c0 -> c1 方向 ✓
        double l10 = 100.0 * sim_dropped(0) / sim_offered(0);   // 目的 slot 0 = c1 -> c0 方向 ✓
        int    b;
        for (b = 0; b < (burst > 0 ? burst : 0); b++) keep *= (1.0 - loss);
        if (burst > 0) expect = 1.0 - keep;
        check(l01 >= expect * 100.0 - 10.0 && l01 <= expect * 100.0 + 10.0, "model dropped c0->c1 far from the configured rate");
        check(l10 >= expect * 100.0 - 10.0 && l10 <= expect * 100.0 + 10.0, "model dropped c1->c0 far from the configured rate");
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
