// B2：在途闸门"能升"（`ffrdp-optimization.md` §8 的 #4 / notes §11.7 边界 2）。要钉三件事：
//   A 干净的高 RTT 链路上闸门**能**升过默认的 64 帧 —— 否则 64 就是硬天花板（仿真里 64→128 交付 +102%）。
//   B 有常驻队列的链路上闸门**不许**升 —— 那正是"把窗口还回去换延迟"要防的形状 ✗。
//   C 升上去之后，探测仍然能把闸门砍回 64 以下 —— 证明 B2 没有把下降路径关掉。
//     这一条是本次改动最容易踩的坑：原来三处判据都拿 FFRDP_INFLIGHT_MAX 当"最高"，cap_kept 一旦
//     被升过 64，`cap_kept < FFRDP_INFLIGHT_MAX` 恒假 ⇒ giveback 与事后修复双双失效 ✗。
//
// HEAD 侧（没有 counter_cap_rise、也没有上升路径）用 HAVE_CAPRISE 探测 ⇒ A 直接判红 ✓。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../ffrdp.h"
#include "simdefs.h"
#include "../../ffrdp.c"

#define MKADDR(port) ((uint64_t)(0x7F000001ULL << 16) | (uint64_t)(port))
#define A_SRV MKADDR(8001)
#define A_C1  MKADDR(40001)

#define CAP_DEFAULT 64u     // FFRDP_INFLIGHT_MAX
#define CAP_CEIL    128u    // FFRDP_INFLIGHT_CEIL

#ifdef HAVE_CAPRISE
static uint32_t rise_cnt(FFRDPCONN *c) { return c->counter_cap_rise; }
#else
static uint32_t rise_cnt(FFRDPCONN *c) { (void)c; return 0; }
#endif

static int g_fail;
static void check(int ok, const char *what) { printf("  %s %s\n", ok ? "✓" : "✗", what); if (!ok) g_fail++; }

static int g_accept, g_srv_h;
static void *p_srv, *p_c1;
static int   g_step;
static char  sbuf[2048], rbuf[2048];

static long on_event(void *cbctx, const FFRDP_EVENT *ev)
{
    (void)cbctx;
    if (ev->type == FFRDP_ACCEPT) { g_accept++; if (g_srv_h < 0) g_srv_h = ev->conn; }
    return FFRDP_NOT_CONSUMED;
}

static long pump(int n)
{
    long got = 0;
    int i, r;
    for (i = 0; i < n; i++) {
        sim_setstep(g_step++);
        ffrdp_send(p_c1, 0, sbuf, 1024, 0);
        if (g_srv_h >= 0) ffrdp_send(p_srv, g_srv_h, sbuf, 1024, 0);
        sim_begin(1); sim_ll_run(1, p_c1);
        sim_begin(0); sim_ll_run(0, p_srv);
        while ((r = ffrdp_recv(p_c1, 0, rbuf, (int)sizeof(rbuf), 0)) > 0) got += r;
        while (g_srv_h >= 0 && (r = ffrdp_recv(p_srv, g_srv_h, rbuf, (int)sizeof(rbuf), 0)) > 0) got += r;
    }
    return got;
}

static int up(void)
{
    FFRDPCONTEXT *srv = (FFRDPCONTEXT*)p_srv, *c1 = (FFRDPCONTEXT*)p_c1;
    return g_srv_h >= 0 && srv->table[g_srv_h] && c1->table[0]
        && srv->table[g_srv_h]->hs_state == HS_UP && c1->table[0]->hs_state == HS_UP;
}

static FFRDPCONN *conn0(void) { return ((FFRDPCONTEXT*)p_c1)->table[0]; }

// 建一对上下文：delay = 单向时延（RTT = 2*delay），shaper 可选（rate KB/s + queue KB）
static int pair_up(int delay, double rate, int qmax)
{
    int i;
    sim_reset();
    sim_config(0.0, 0, delay, 0);
    if (rate > 0) sim_shaper(rate * 1024.0 / 1000.0, qmax * 1024);
    g_accept = 0; g_srv_h = -1; g_step = 0;
    p_srv = ffrdp_init("ffrdps://127.0.0.1:8001,mss:1024,fec:0", on_event, NULL);
    p_c1  = ffrdp_init("ffrdpc://127.0.0.1:8001,mss:1024,fec:0", on_event, NULL);
    if (!p_srv || !p_c1) return 0;
    sim_ll_own(((FFRDPCONTEXT*)p_srv)->lowlevel, A_SRV);
    sim_ll_own(((FFRDPCONTEXT*)p_c1)->lowlevel, A_C1);
    sim_ll_route(((FFRDPCONTEXT*)p_srv)->lowlevel, A_C1, 1);
    sim_ll_route(((FFRDPCONTEXT*)p_c1)->lowlevel, A_SRV, 0);
    for (i = 0; i < 8000 && !up(); i++) pump(1);
    return up();
}

static void pair_down(void)
{
    if (p_c1) ffrdp_exit(p_c1);
    if (p_srv) ffrdp_exit(p_srv);
    p_c1 = p_srv = NULL;
}

// A：干净的高 RTT 链路（RTT 200 ms），闸门应当自己升过 64
static void case_a(void)
{
    int i;
    uint32_t firsts, resends;
    printf("A 干净 200ms 链路上闸门能升（默认 %u，天花板 %u）\n", CAP_DEFAULT, CAP_CEIL);
    check(pair_up(100, 0, 0), "握手完成");
    if (!up()) return;

    for (i = 0; i < 60000 && conn0()->cap_kept <= CAP_DEFAULT; i++) pump(1);
    check(conn0()->cap_kept > CAP_DEFAULT, "闸门升过了默认值（cap_kept > 64）");
    check(rise_cnt(conn0()) > 0, "确实走的是 B2 的上升路径（counter_cap_rise > 0）");
    firsts  = conn0()->counter_send_1sttime;
    resends = conn0()->counter_resend_rto + conn0()->counter_resend_fast;
    printf("       升到 %u 帧（%d 步）：首发 %u / 重传 %u（%.2f%%）\n",
           conn0()->cap_kept, i, firsts, resends, firsts ? 100.0 * resends / firsts : 0.0);
    check(!firsts || resends * 100 < firsts * 5, "升的过程中重传率没有抬头（< 5%）");
}

// C：升上去之后，探测仍要能把它砍回 64 以下（下降路径没被 cap_kept 与上限的耦合卡死）
static void case_c(void)
{
    int i, cut = 0;
    printf("C 升上去之后探测仍能把闸门砍回来\n");
    if (conn0()->cap_kept <= CAP_DEFAULT) { check(0, "前提不成立：A 没升上去 ⇒ 这一条测不到"); return; }
    // 突然给它一个瓶颈（600 KB/s、200 KB 队列）⇒ 探测应当发现"窗口里有队列"并把上限砍下来
    sim_shaper(600 * 1024.0 / 1000.0, 200 * 1024);
    for (i = 0; i < 60000 && !cut; i++) {
        pump(1);
        if (conn0()->probe_state != 0 && conn0()->inflight_max < CAP_DEFAULT) cut = 1;
    }
    check(cut, "探测把上限砍到了 64 帧以下（下降路径还活着）");
    printf("       当前 cap_kept=%u inflight_max=%u probes=%u\n",
           conn0()->cap_kept, conn0()->inflight_max, conn0()->counter_probe);
}

// D：RTT 地板的离群保护（真机 §26.97 复测踩到：200 ms 链路上一个 60 ms 的假样本把 rtt_min 钉死，
//    于是 B2 的"没队列"恒假、探测的"排队"恒真 ⇒ cap 被 revert 到 40、rise 一次都没动 ✗）
#ifdef HAVE_RTTGUARD
static void case_d(void)
{
    FFRDPCONN *c = conn0();
    uint32_t m0 = c->rtt_min, s0 = c->rtts, d0 = c->rttd, t0 = (uint32_t)c->rttm;

    printf("D RTT 地板：离群低样本不许钉死地板，同量级的新低仍然认\n");
    c->rtt_min = 201; c->rtts = 201; c->rttd = 0; c->rttm = 201;

    ffrdp_rtt_sample(c, 60);                       // 3.35 倍的低样本 ⇒ 只当噪声
    check(c->rtt_min == 201, "60 ms 的离群样本没有下调地板");
    ffrdp_rtt_sample(c, 190);                      // 与平滑值同量级 ⇒ 正常下调
    check(c->rtt_min == 190, "190 ms 与平滑值同量级 ⇒ 地板照常下调");
    ffrdp_rtt_sample(c, 195);                      // 不低了 ⇒ 地板不动
    check(c->rtt_min == 190, "更高的样本不动地板");

    check(FFRDP_PROBE_IDLE_SLACK(201u) >= 12u, "200 ms 链路的\"没队列\"容差随 RTT 缩放（>= 12 ms）");
    check(FFRDP_PROBE_IDLE_SLACK(20u) == FFRDP_PROBE_RTT_DROP_MS, "20 ms 链路仍是 2 ms（旧行为不变）");

    c->rtt_min = m0; c->rtts = s0; c->rttd = d0; c->rttm = (int32_t)t0;
}
#else
static void case_d(void)
{
    printf("D RTT 地板：离群低样本不许钉死地板\n");
    check(0, "HEAD 侧没有 rtt_min 离群保护（这一侧本来就该红 ✗）");
}
#endif

// B：有常驻队列的链路（同样的 RTT，整形器 600 KB/s、队列 200 KB）⇒ 闸门不许升
static void case_b(void)
{
    int i;
    printf("B 有常驻队列的链路上闸门不许升\n");
    check(pair_up(100, 600, 200), "握手完成");
    if (!up()) return;
    for (i = 0; i < 60000; i++) pump(1);
    check(rise_cnt(conn0()) == 0, "一次都没升（counter_cap_rise == 0）");
    check(conn0()->cap_kept <= CAP_DEFAULT, "cap_kept 没有越过默认值（队列还在就不该还窗口）");
    printf("       60 秒后 cap_kept=%u cap=%u inflight_max=%u rtt_min=%u rtts=%u\n",
           conn0()->cap_kept, conn0()->cap, conn0()->inflight_max,
           conn0()->rtt_min, conn0()->rtts);
}

int main(void)
{
    printf("B2：在途闸门「能升」（顶死 + 没队列 + 没浪费才升，revert 后一个周期不升）\n");
    memset(sbuf, 'P', sizeof(sbuf));

    case_a();
    if (p_c1) case_c();
    if (p_c1) case_d();
    pair_down();

    case_b();
    pair_down();

    check(sim_alloc() == sim_freed(), "没有泄漏 ✓");
    printf(g_fail ? "*** CHECK FAILED: %d 项不通过 ***\n" : "=== CHECK OK: 闸门的升/降三条都对 ===\n", g_fail);
    return g_fail ? 1 : 0;
}
