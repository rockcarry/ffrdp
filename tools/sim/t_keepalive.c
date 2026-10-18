// keepalive（v2：把 `idle_ms` 真正接上）—— 回答一个具体问题：
//   连接建好之后**双方都不发数据**，会不会被超时判成断连？
//
// 现在的语义（也就是要钉住的）：
//   * 空闲**要主动确权** ✓：半程（`idle_ms / 2`）没听到对端 ⇒ 发一个 QUERY 去敲它 ✓；
//   * 对端**活着**（回了 ack）⇒ 一直不掉 ✓（**不误杀** ✓）；
//   * 对端**哑了**（不回）⇒ 整整 `idle_ms` 没听到 ⇒ 判死：双方各自报 `DISCONN` ✓；
//     服务端把连接收掉（表槽清空、句柄失效 ✓），客户端的主连接置 `HS_FAILED`
//     （`isdead` ⇒ 1 ✓，随后的 `recv` 报 `CLOSED` ✓）。
//
// 两段（`idle_ms` 设成 300ms 以便快跑 ✓ —— 它同时决定"多久探一次"和"多久判死" ✓）：
//   A 空闲 × 活着：干净链路、双方都不发应用数据 ⇒ 空跑 5×idle_ms ⇒ **不能掉** ✓，
//     而且要能看见探测真的在发（`counter_send_query` 在涨 ✓ —— 不是靠"什么都不做"活着 ✓）。
//   B 空闲 × 对端哑掉：先把连接建好（干净链路 ✓），再把丢包率拉成 **100%** ⇒
//     对端的话进不来 ⇒ 应当在 `idle_ms` 后判死 ✓。
//
// ⚠️ 一个必须写下来的坑（我在这里踩过 ✗）：keepalive 的"听到对端"要刷在
//    **按地址解出来的那条连接**（`cc = ffrdp_conn_by_addr()`）上 ✓，不能刷在 `table[0]` ✗ ——
//    否则服务端那条 accept 连接永远"没听到" ✓✗ ⇒ 一到 `idle_ms` 就被误判死（实测每个
//    e2e 场景都在 5012ms 被杀 ✓）。这条教训在 ffrdp.c 的注释里也留了 ✓。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../ffrdp.h"
#include "simdefs.h"
#include "../../ffrdp.c"

#define MKADDR(port) ((uint64_t)(0x7F000001ULL << 16) | (uint64_t)(port))
#define A_SRV MKADDR(8001)
#define A_C1  MKADDR(40001)
#define IDLE_MS 300

static int g_fail;
static void check(int ok, const char *what) { printf("  %s %s\n", ok ? "✓" : "✗", what); if (!ok) g_fail++; }

struct app { int accept_n; int conn; int disconn_n; };

// 事件回调：**持着协议锁**被调用 ⇒ 只记数 / 记句柄，绝不回调 ffrdp_*（否则自锁 ✓）
static long on_event(void *cbctx, const FFRDP_EVENT *ev)
{
    struct app *a = (struct app*)cbctx;
    if (!a) return FFRDP_NOT_CONSUMED;
    if (ev->type == FFRDP_ACCEPT)       { a->accept_n++; a->conn = ev->conn; }
    else if (ev->type == FFRDP_DISCONN) a->disconn_n++;
    return FFRDP_NOT_CONSUMED;
}

static int g_step;   // 单调步号：假网络把"延迟 N 步后投递"的包存着 ⇒ 时钟不能倒回 ✗
static void drive(int steps, void *p_c1, void *p_srv)
{
    int i;
    for (i = 0; i < steps; i++) {
        sim_setstep(g_step++);
        sim_begin(1); sim_ll_run(1, p_c1);
        sim_begin(0); sim_ll_run(0, p_srv);
    }
}

// 建一对上下文（默认走握手 ✓，idle_ms 由参数给 ✓），都登记好路由
static int make_pair(void **pp_srv, void **pp_c1, struct app *as, struct app *ac)
{
    char params[160];
    memset(as, 0, sizeof(*as)); memset(ac, 0, sizeof(*ac));
    as->conn = ac->conn = -1;
    snprintf(params, sizeof(params), "ffrdps://127.0.0.1:8001,mss:1024,fec:0,idle_ms:%d", IDLE_MS);
    *pp_srv = ffrdp_init(params, on_event, as);
    snprintf(params, sizeof(params), "ffrdpc://127.0.0.1:8001,mss:1024,fec:0,idle_ms:%d", IDLE_MS);
    *pp_c1  = ffrdp_init(params, on_event, ac);
    if (!*pp_srv || !*pp_c1) return -1;
    sim_ll_own(((FFRDPCONTEXT*)*pp_srv)->lowlevel, A_SRV);
    sim_ll_own(((FFRDPCONTEXT*)*pp_c1)->lowlevel,  A_C1);
    sim_ll_route(((FFRDPCONTEXT*)*pp_srv)->lowlevel, A_C1,  1);
    sim_ll_route(((FFRDPCONTEXT*)*pp_c1)->lowlevel,  A_SRV, 0);
    return 0;
}

int main(void)
{
    void *p_srv, *p_c1;
    FFRDPCONTEXT *srv, *c1;
    struct app a_srv, a_c1;
    int conn0;
    char b[8];

    // ── A：空闲 × 活着 ────────────────────────────────────────────────────
    printf("── A 空闲 × 活着（干净链路，双方都不发应用数据）\n");
    sim_reset(); sim_config(0.0, 0, 4, 0);
    g_step = 0;
    if (make_pair(&p_srv, &p_c1, &a_srv, &a_c1) != 0) { printf("  建上下文失败 ✗\n"); return 1; }
    srv = (FFRDPCONTEXT*)p_srv; c1 = (FFRDPCONTEXT*)p_c1;

    drive(IDLE_MS * 2, p_c1, p_srv);                   // 先握上手（600ms）
    check(a_srv.accept_n == 1, "服务端接受了连接（ACCEPT ×1）");
    check(a_srv.conn >= 1, "句柄来自 accept（不是 0 号主连接 ✓）");
    conn0 = a_srv.conn;

    drive(IDLE_MS * 5, p_c1, p_srv);                   // 纯空闲跑 5×idle_ms
    printf("     （空闲 5×idle_ms 后：srv 探测计数=%u  srv disconn=%d  c1 disconn=%d）\n",
           srv->table[conn0] ? srv->table[conn0]->counter_send_query : 0, a_srv.disconn_n, a_c1.disconn_n);
    check(a_srv.disconn_n == 0 && a_c1.disconn_n == 0, "**双方都没被误杀**（空闲但活着 ✓）");
    check(srv->table[conn0] != NULL, "服务端那条连接还在 ✓");
    check(ffrdp_isdead(srv, conn0) == 0 && ffrdp_isdead(c1, 0) == 0, "isdead 两边都报 0（活着 ✓）");
    check(srv->table[conn0] && srv->table[conn0]->counter_send_query >= 2,
          "空闲期确实在发**探活 QUERY** ✓（不是靠\"什么都不做\"活着 ✓）");
    ffrdp_exit(p_c1); ffrdp_exit(p_srv);

    // ── B：空闲 × 对端哑掉 ────────────────────────────────────────────────
    printf("── B 空闲 × 对端哑掉（先建好连接，再把链路丢包拉成 100%%）\n");
    sim_reset(); sim_config(0.0, 0, 4, 0);
    g_step = 0;
    if (make_pair(&p_srv, &p_c1, &a_srv, &a_c1) != 0) { printf("  建上下文失败 ✗\n"); return 1; }
    srv = (FFRDPCONTEXT*)p_srv; c1 = (FFRDPCONTEXT*)p_c1;

    drive(IDLE_MS * 2, p_c1, p_srv);                   // 先握上手（600ms）✓
    check(a_srv.accept_n == 1, "先把连接正常建起来 ✓");
    conn0 = a_srv.conn;

    sim_config(1.0, 0, 4, 0);                          // 之后谁都听不到谁 ✓
    drive(IDLE_MS * 3, p_c1, p_srv);
    printf("     （哑掉后跑 3×idle_ms：srv disconn=%d  c1 disconn=%d）\n", a_srv.disconn_n, a_c1.disconn_n);
    check(a_srv.disconn_n == 1, "服务端报了一次 DISCONN（判死 ✓）");
    check(srv->table[conn0] == NULL, "服务端把那条连接收掉了（表槽清空 ✓）");
    check(ffrdp_isdead(srv, conn0) == 1, "句柄已失效 ⇒ isdead 报 1 ✓");
    check(a_c1.disconn_n == 1, "客户端也报了 DISCONN ✓");
    check(ffrdp_isdead(c1, 0) == 1, "客户端主连接 ⇒ isdead 报 1（HS_FAILED ✓）");
    check(ffrdp_recv(c1, 0, b, sizeof(b), 0) == 0, "客户端 recv 报 CLOSED（0 ✓ —— 应用看得出来 ✓）");
    ffrdp_exit(p_c1); ffrdp_exit(p_srv);

    // ── C：判死口径的两条修正（状态层面的钉子 ✓）────────────────────────────
    // 1) 首帧久等 ≠ 连接已死：只要**还在听得到对端**，就交给重传 / 接收窗口去修 ✗→✓
    //    （6 客户端那轮：`rtts 2298 ms`、`rto 2000` ⇒ 老口径 5 s 只够两次 RTO，先误判死了 ✓）
    // 2) 静默上限跟着 RTT 放大（≥ 4 RTT）：RTT 到秒级时，固定 5 s 只够一个来回 ⇒ 会反复重连 ✓
    printf("── C 判死口径：首帧久等要对端也静默；静默上限跟着 RTT 放大\n");
    sim_reset(); sim_config(0.0, 0, 4, 0);
    g_step = 0;
    if (make_pair(&p_srv, &p_c1, &a_srv, &a_c1) != 0) { printf("  建上下文失败 ✗\n"); return 1; }
    srv = (FFRDPCONTEXT*)p_srv; c1 = (FFRDPCONTEXT*)p_c1;
    drive(IDLE_MS * 2, p_c1, p_srv);                   // 先握上手 ✓
    conn0 = a_srv.conn;
    {   FFRDPCONN *c;
        {   static char big[1300];                       // > smss(1024) ⇒ 只有攒满的帧才进 send_list ✓
            ffrdp_send(p_srv, conn0, big, (int)sizeof(big), 0);
        }
        c = ffrdp_conn_lookup(srv, conn0);              // 句柄带代号 ⇒ 用查表入口 ✓
        check(c && c->send_list_head != NULL, "服务端有一条待确认的帧（钉判死口径用 ✓）");
        if (c && c->send_list_head) {
            c->send_list_head->flags   |= FLAG_FIRST_SEND;   // 假装它已经上过线 ✓
            c->send_list_head->tick_1sts = NOW_MS() - 6000;  // 首帧 6 s 没被确认 ✓
            c->tick_last_recv            = NOW_MS();         // 但刚刚听到对端 ✓
            check(ffrdp_isdead_locked(srv, c) == 0, "首帧 6s 未确认、但仍在听说对端 ⇒ **不判死** ✓");
            c->tick_last_recv            = NOW_MS() - 6000;  // 对端也静默了 ✓
            check(ffrdp_isdead_locked(srv, c) == 1, "对端也静默 6s ⇒ 判死 ✓（真断的时候照样收 ✓）");

            c->tick_last_recv = NOW_MS() - 1000;             // 静默 1 s（> idle_ms 300 ✓）
            c->rtts           = 2000;                        // RTT 2 s ⇒ 上限 = 4×RTT = 8 s
            check(ffrdp_isdead_locked(srv, c) == 0,
                  "RTT 2s 的链路上静默 1s ⇒ **不判死**（上限跟着 RTT 放大 ✓）");
            c->rtts = 10;                                    // RTT 10 ms ⇒ 用配置的 300 ms
            check(ffrdp_isdead_locked(srv, c) == 1, "RTT 10ms 的链路上静默 1s ⇒ 判死（配置值生效 ✓）");
        }
    }
    ffrdp_exit(p_c1); ffrdp_exit(p_srv);

    printf("       verdict -> %s\n", g_fail ? "FAIL" : "OK");
    return g_fail ? 1 : 0;
}
