// ffrdp_flush（v2）：把"还没攒满的那个半帧"立刻发出去 ✓。三条承诺，这里一条一条钉：
//
//   A 不调 flush ⇒ 半帧就该等着（对照组）—— 证明 B 的"到达"是 flush 的功劳，不是网络快 ✓
//   B `conn >= 0` ⇒ **只对那一条连接**生效。服务端的主连接占 0 号、accept 出来的是 1..N，
//     所以这里用的是 accept 句柄（≥ 1）✓。修前固定刷 `table[0]` ⇒ B 必红 ✓
//   C flush 是**一次性请求**：调过一次之后，后面新产生的半帧不该继续被立刻发出 ✗
//     （否则等于把"自动短帧冲刷"永久打开 —— 那正是 A2 要单独评估、而且要能被量出来的事 ✓）
//
// 时序一律用"≪ 500 ms"来卡：20 ms 内到 = flush 干的 ✓；20 ms 内不到 = 还在等 FFRDP_FLUSH_TIMEOUT ✓。
// 每个场景**各自 fresh 一套上下文**：半帧是会跨场景残留的（B 没发出去，C 的字节就接在同一条半帧上 ✗），
// 那样断言之间会互相污染。
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
static char  sbuf[256], rbuf[256];

static long on_event(void *cbctx, const FFRDP_EVENT *ev)
{
    (void)cbctx;
    if (ev->type == FFRDP_ACCEPT) { g_accept++; if (g_srv_h < 0) g_srv_h = ev->conn; }
    return FFRDP_NOT_CONSUMED;
}

// 推进 n 步（每步跑两侧各一圈协议），返回这期间客户端从 ffrdp_recv 取到的字节数 ✓
static long pump(int n)
{
    long got = 0;
    int i, r;
    for (i = 0; i < n; i++) {
        sim_setstep(g_step++);
        sim_begin(1); sim_ll_run(1, p_c1);
        sim_begin(0); sim_ll_run(0, p_srv);
        while ((r = ffrdp_recv(p_c1, 0, rbuf, (int)sizeof(rbuf), 0)) > 0) got += r;
    }
    return got;
}

// 半帧在不在（本测试的前提：100 字节 ≪ smss 1024 ⇒ 必须进 cur_new_node ✓）
static int half_frame(void)
{
    FFRDPCONN *c = ((FFRDPCONTEXT*)p_srv)->table[g_srv_h];
    return c && c->cur_new_node;
}

// fresh 一套上下文并握手；失败返回 0 ✓
static int boot(const char *name)
{
    FFRDPCONTEXT *srv, *c1;
    int i, up = 0;

    printf("── %s\n", name);
    sim_reset(); sim_config(0.0, 0, 4, 0);
    g_accept = 0; g_srv_h = -1; g_step = 0;
    p_srv = ffrdp_init("ffrdps://127.0.0.1:8001,mss:1024,fec:0", on_event, NULL);
    p_c1  = ffrdp_init("ffrdpc://127.0.0.1:8001,mss:1024,fec:0", on_event, NULL);
    if (!p_srv || !p_c1) { check(0, "两个上下文都建起来了（默认走握手）"); return 0; }
    srv = (FFRDPCONTEXT*)p_srv; c1 = (FFRDPCONTEXT*)p_c1;
    sim_ll_own(srv->lowlevel, A_SRV); sim_ll_own(c1->lowlevel, A_C1);
    sim_ll_route(srv->lowlevel, A_C1, 1); sim_ll_route(c1->lowlevel, A_SRV, 0);

    // 步子给足 2500 步 ⇒ **第一条 SYN 一定丢**：`ffrdp_init` 在 own/route 装好之前就发了 ✗，
    // 得等握手重传（秒级）才会来第二条。这与真实环境无关（真实环境里路由一开始就在 ✓）。
    for (i = 0; i < 2500 && !up; i++) {
        pump(1);
        up = (g_srv_h >= 0 && srv->table[g_srv_h] && c1->table[0]
              && srv->table[g_srv_h]->hs_state == HS_UP && c1->table[0]->hs_state == HS_UP);
    }
    if (!up) { check(0, "握手完成"); printf("    [dbg] accept=%d srv_h=%d steps=%d\n", g_accept, g_srv_h, g_step); return 0; }
    check(g_srv_h >= 1, "握手完成，且 accept 拿到的是 1 号及以后的句柄（0 号是服务端主连接 ✓）");
    return 1;
}

static void teardown(const char *what)
{
    // 用 exit（直接把上下文拆掉）而不是 close：close 只是发 FIN，收敛要等秒级定时器，
    // 泄漏检查会被"还在飞"的帧耽误 ✗（这一条只关心内存有没有还回去 ✓）。
    ffrdp_exit(p_c1); ffrdp_exit(p_srv);
    check(sim_alloc() == sim_freed(), what);
}

int main(void)
{
    long got;

    printf("ffrdp_flush：只刷指定的那条连接、且只是一次性请求\n");

    // ── A 对照组：不调 flush，半帧就该等着 ──────────────────────────────
    if (boot("A 不调 flush（对照）：100 字节应当还卡在半帧里")) {
        check(ffrdp_send(p_srv, g_srv_h, sbuf, 100, 0) == 100, "服务端发出 100 字节");
        check(half_frame(), "此刻确实是半帧（还没攒满 smss ✓）");
        got = pump(20);
        check(got == 0, "20 ms 内客户端什么也没收到（≪ 500 ms 超时 ✓）");
        got = pump(520);
        check(got == 100, "再推进 520 ms，超时把它送走了 ✓（老行为没变）");
        teardown("没有泄漏（上下文/帧都还回去了 ✓）");
    }

    // ── B 目标：对 accept 句柄 flush，半帧必须立刻走 ──────────────────
    // ── C 一次性：flush 过之后，新半帧不该继续被立刻发 ─────────────────
    if (boot("B/C 对 1 号连接 flush，且只生效这一次")) {
        check(ffrdp_send(p_srv, g_srv_h, sbuf, 100, 0) == 100, "服务端发 100 字节");
        check(half_frame(), "又生成一个半帧 ✓");
        ffrdp_flush(p_srv, g_srv_h);
        got = pump(20);
        check(got == 100, "B：20 ms 内客户端就收到了（修前固定刷 0 号 ⇒ 这里必红 ✓）");
        check(ffrdp_send(p_srv, g_srv_h, sbuf, 100, 0) == 100, "C：flush 之后再发 100 字节");
        got = pump(20);
        check(got == 0, "C：20 ms 内没收到（FLAG_FLUSH 应已撤掉 ✓；不撤 ⇒ 这里会收到 100 ✗）");
        got = pump(520);
        check(got == 100, "C：它仍然按 500 ms 超时走了 ✓");
        teardown("没有泄漏（上下文/帧都还回去了 ✓）");
    }

    printf(g_fail ? "*** CHECK FAILED: %d 项不通过 ***\n" : "=== CHECK OK: ffrdp_flush 的三条承诺都对 ===\n", g_fail);
    return g_fail ? 1 : 0;
}
