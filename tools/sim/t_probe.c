// A′：尾部探测（notes §26.96）。要钉两件事：
//   A 健康链路上**一次都不探** —— RTO 贴着 RTT 走（≈1.2×RTT）时提前问一句反而更慢 ✗，
//     所以门槛是 RTO ≥ 4×(RTT+2)，达不到就零动作 ✓（这是"负优化"那道防线 ✓）。
//   B 静默一段时间（RTO 已经被 ×1.5 退避抬起来）之后，探测能把"等满 RTO"变成"提前 1/4 RTO 重传" ✓。
//     效果用"链路恢复后多少步重新收到数据"来量。
//
// 这个测试要能编译 HEAD 侧（那里还没有 counter_probe）⇒ 用 HAVE_TAILPROBE 探测，HEAD 侧直接判红 ✓。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../ffrdp.h"
#include "simdefs.h"
#include "../../ffrdp.c"

#define MKADDR(port) ((uint64_t)(0x7F000001ULL << 16) | (uint64_t)(port))
#define A_SRV MKADDR(8001)
#define A_C1  MKADDR(40001)

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

// 推进 n 步：每步两侧各发一批、各跑一圈协议；返回这期间两侧合起来收到的字节数 ✓
static long pump(int n, int send)
{
    long got = 0;
    int i, r;
    for (i = 0; i < n; i++) {
        sim_setstep(g_step++);
        if (send) {
            ffrdp_send(p_c1, 0, sbuf, 1024, 0);
            if (g_srv_h >= 0) ffrdp_send(p_srv, g_srv_h, sbuf, 1024, 0);
        }
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

int main(void)
{
    FFRDPCONTEXT *c1;
    FFRDPCONN    *cc;
    long got;
    int  i, steps, probes_before = 0;
    uint32_t rto_stall = 0;

    printf("A′：尾部探测（健康链路不探、静默后提前 1/4 RTO 恢复）\n");
    memset(sbuf, 'P', sizeof(sbuf));
    sim_reset(); sim_config(0.0, 0, 4, 0);
    g_accept = 0; g_srv_h = -1; g_step = 0;

    p_srv = ffrdp_init("ffrdps://127.0.0.1:8001,mss:1024,fec:0", on_event, NULL);
    p_c1  = ffrdp_init("ffrdpc://127.0.0.1:8001,mss:1024,fec:0", on_event, NULL);
    if (!p_srv || !p_c1) { check(0, "两个上下文都建起来了"); return 1; }
    c1 = (FFRDPCONTEXT*)p_c1; cc = c1->table[0];
    sim_ll_own(((FFRDPCONTEXT*)p_srv)->lowlevel, A_SRV); sim_ll_own(c1->lowlevel, A_C1);
    sim_ll_route(((FFRDPCONTEXT*)p_srv)->lowlevel, A_C1, 1); sim_ll_route(c1->lowlevel, A_SRV, 0);

    // 握手（第一条 SYN 一定丢：init 在 own/route 之前就发了 ⇒ 要等秒级重传 ✓）
    for (i = 0; i < 2500 && !up(); i++) pump(1, 0);
    check(up(), "握手完成");
    if (!up()) return 1;

    // ── A 健康链路：零动作 ─────────────────────────────────────────────
    got = pump(600, 1);
    check(got > 0, "健康链路上确实在传数据");
    printf("       健康期：rto=%u rtts=%u\n", cc->rto, cc->rtts);
#ifdef HAVE_TAILPROBE
    probes_before = (int)cc->counter_tailprobe;
    check(probes_before == 0, "健康链路（RTO ≈ RTT）一次都不探 ✓（门槛 RTO ≥ 4×(RTT+2) ⇒ 零动作 ✓）");
#else
    check(0, "A′（尾部探测）在这一侧还不存在（没有 counter_probe）");
#endif

    // ── B 静默 400 ms：只能靠 RTO 超时 + ×1.5 退避 ──────────────────────
    sim_config(1.0, 0, 4, 0);
    pump(400, 1);
    rto_stall = cc->rto;
    printf("       静默 400 ms 后：rto=%u rtts=%u swnd=%u cwnd=%u wait_snd=%u\n",
           cc->rto, cc->rtts, cc->swnd, cc->cwnd, cc->wait_snd);
    check(rto_stall >= 4 * (cc->rtts + 2), "RTO 已被退避抬到 4×(RTT+2) 以上（探针门槛达成 ✓）");

    // ── 放开链路：数"多少步之后重新收到数据" ────────────────────────────
    sim_config(0.0, 0, 4, 0);
    steps = 0;
    for (i = 0; i < 4000; i++) { steps++; if (pump(1, 1) > 0) break; }
    printf("       恢复用时 %d 步（此时 rto=%u）\n", steps, cc->rto);
    check(steps < 4000, "链路放开后数据重新流动 ✓");
#ifdef HAVE_TAILPROBE
    check((int)cc->counter_tailprobe > probes_before, "静默后确实发出了尾部探测 ✓");
    printf("       probe=%u probe_lost=%u resend_rto=%u resend_fast=%u\n",
           cc->counter_tailprobe, cc->counter_tailprobe_lost, cc->counter_resend_rto,
           cc->counter_resend_fast);
    // 恢复必须**早于**"等满一个 RTO"：能提前就说明探针把 3/4 处用上了 ✓
    check((uint32_t)steps < rto_stall, "恢复用时小于退避后的 RTO ⇒ 探针确实提前动手了 ✓");
#else
    check(0, "没有尾部探测 ⇒ 只能等满 RTO（这一侧应当比工作区慢 ✗）");
#endif

    ffrdp_exit(p_c1); ffrdp_exit(p_srv);
    check(sim_alloc() == sim_freed(), "没有泄漏 ✓");
    printf(g_fail ? "*** CHECK FAILED: %d 项不通过 ***\n" : "=== CHECK OK: 尾部探测的两条都对 ===\n", g_fail);
    return g_fail ? 1 : 0;
}
