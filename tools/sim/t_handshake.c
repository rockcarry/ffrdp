// 握手 + 多连接（v2）—— 四个阶段，把 P0/P1/P2/P4 四步的成果合起来验：
//
//   P0 的夹具（按地址路由 sim_ll_route / sim_ll_own）✓
//   P1 的每连接结构（FFRDPCONN）✓
//   P2 的按地址分发（ffrdp_conn_by_addr）✓
//   P4 的连接表（句柄 = 下标、按需分配、索引不复用）✓
//
// 阶段：
//   1) 正常：一个服务端 + 两个客户端（hs:1）—— CONNECT×2 / ACCEPT×2 / 句柄不同 / 不串台
//      1b) FIN：从某个客户端的地址发一个 FIN ⇒ 服务端 DISCONN 并把那条连接放掉（表槽清空、水位不回收）
//   2) 握手丢包：把客户端第一个 SYN 丢掉 ⇒ 客户端重传 ⇒ 仍然连得上（hs_retry ≥ 2）
//   3) 握手超时：对端从不回答 ⇒ 客户端 HS_FAILED ⇒ 之后 send/recv 一律报"已断"（CLOSED）
//
// 它只编一份（接口是 v2 的）⇒ run.sh 里 V2ONLY。
#include <stdio.h>
#include <string.h>
#include "../../ffrdp.h"
#include "simdefs.h"
#include "../../ffrdp.c"

#define MKADDR(port) ((uint64_t)(0x7F000001ULL << 16) | (uint64_t)(port))
#define A_SRV MKADDR(8001)   // 服务端地址 = 客户端 dial 的那个
#define A_C1  MKADDR(40001)  // 客户端 1 自己的地址（服务端从它的 SYN 里看到）
#define A_C2  MKADDR(40002)
#define A_X   MKADDR(40010)  // 阶段 4/5 里"只会发 SYN 的裸对端"
#define A_Y   MKADDR(40011)

static int g_fail;
static void check(int ok, const char *what) { printf("  %s %s\n", ok ? "✓" : "✗", what); if (!ok) g_fail++; }

static int g_connect, g_accept, g_disconn;
static int g_h1 = -1, g_h2 = -1;
static uint64_t g_a1, g_a2;

// 事件回调：**持着协议锁**被调用 ⇒ 只记数，绝不回调 ffrdp_*（否则自锁）。
static long on_event(void *cbctx, const FFRDP_EVENT *ev)
{
    (void)cbctx;
    if (ev->type == FFRDP_CONNECT) g_connect++;
    if (ev->type == FFRDP_DISCONN) g_disconn++;
    if (ev->type == FFRDP_ACCEPT) {
        g_accept++;
        if (g_accept == 1) { g_h1 = ev->conn; g_a1 = ev->addr; }
        else               { g_h2 = ev->conn; g_a2 = ev->addr; }
    }
    return FFRDP_NOT_CONSUMED;
}

// 组装一个握手帧（帧类型 35 + 子类型）
static int hs_frame(char *out, int sub) { out[0] = (char)FFRDP_FRAME_TYPE_HANDSHAKE; out[1] = (char)sub; return 2; }

int main(void)
{
    FFRDPCONTEXT *srv, *c1, *c2, *sc, *cc;
    void *p_srv, *p_c1, *p_c2, *p_sc, *p_cc;
    FFRDP_LOWLEVEL *ll_srv, *ll_c1, *ll_c2, *ll_sc, *ll_cc;
    char  sbuf[64], rbuf[512], fin[2], hsbuf[2], drop[2048];
    int   i, n, r1 = 0, r2 = 0, bad1 = 0, bad2 = 0, sent1 = 0, sent2 = 0;

    /* ── 阶段 1：正常握手 + 多连接不串台 ─────────────────────────────────── */
    printf("── 阶段 1：一个服务端 + 两个客户端（hs:1）\n");
    sim_reset();
    p_srv = ffrdp_init("ffrdps://127.0.0.1:8001,mss:1024,fec:0,hs:1,maxconn:4", on_event, NULL);
    p_c1  = ffrdp_init("ffrdpc://127.0.0.1:8001,mss:1024,fec:0,hs:1", on_event, NULL);
    p_c2  = ffrdp_init("ffrdpc://127.0.0.1:8001,mss:1024,fec:0,hs:1", on_event, NULL);
    check(p_srv && p_c1 && p_c2, "三个上下文都建起来了");
    if (!p_srv || !p_c1 || !p_c2) { printf("── 结果：FAIL\n"); return 1; }
    srv = (FFRDPCONTEXT*)p_srv; c1 = (FFRDPCONTEXT*)p_c1; c2 = (FFRDPCONTEXT*)p_c2;
    ll_srv = srv->lowlevel; ll_c1 = c1->lowlevel; ll_c2 = c2->lowlevel;
    sim_ll_own(ll_srv, A_SRV); sim_ll_own(ll_c1, A_C1); sim_ll_own(ll_c2, A_C2);
    sim_ll_route(ll_srv, A_C1, 1); sim_ll_route(ll_srv, A_C2, 2);
    sim_ll_route(ll_c1, A_SRV, 0); sim_ll_route(ll_c2, A_SRV, 0);

    memset(sbuf, 'A', sizeof(sbuf));
    for (i = 0; i < 2000; i++) {
        sim_setstep(i);
        if (ffrdp_send(c1, 0, sbuf, 64, 0) == 64) sent1 += 64;
        if (ffrdp_send(c2, 0, sbuf, 64, 0) == 64) sent2 += 64;
        sim_begin(1); sim_ll_run(1, p_c1);
        sim_begin(2); sim_ll_run(2, p_c2);
        sim_begin(0); sim_ll_run(0, p_srv);
        if (g_h1 >= 0) while ((n = ffrdp_recv(srv, g_h1, rbuf, sizeof(rbuf), 0)) > 0) { int k; for (k = 0; k < n; k++) if (rbuf[k] != 'A') bad1++; r1 += n; }
        if (g_h2 >= 0) while ((n = ffrdp_recv(srv, g_h2, rbuf, sizeof(rbuf), 0)) > 0) { int k; for (k = 0; k < n; k++) if (rbuf[k] != 'A') bad2++; r2 += n; }
        while (ffrdp_recv(c1, 0, rbuf, sizeof(rbuf), 0) > 0) { }
        while (ffrdp_recv(c2, 0, rbuf, sizeof(rbuf), 0) > 0) { }
        if (g_connect == 2 && g_accept == 2 && r1 > 4096 && r2 > 4096) break;
    }
    printf("  （跑了 %d 步：CONNECT=%d ACCEPT=%d，h1=%d 字节 / h2=%d 字节）\n", i + 1, g_connect, g_accept, r1, r2);
    check(g_connect == 2, "两个客户端都完成了握手（CONNECT ×2）");
    check(g_accept == 2, "服务端接受了两个连接（ACCEPT ×2）");
    check(g_h1 >= 1 && g_h2 >= 1 && g_h1 != g_h2, "两条连接句柄不同、且都不是 0 号主连接");
    check((g_a1 == A_C1 && g_a2 == A_C2) || (g_a1 == A_C2 && g_a2 == A_C1), "ACCEPT 报的对端地址就是两个客户端");
    check(srv->nconn == 3, "服务端连接表：主连接 + 两条（nconn=3）");
    check(r1 > 4096 && r2 > 4096 && bad1 == 0 && bad2 == 0, "两条连接各收到自己的数据、不串台");
    check(sent1 > 0 && sent2 > 0, "两个客户端都确实发出去了数据");
    // isdead 现在按句柄问（v2 ✓）：失联是**每连接**的状态 ✓
    check(ffrdp_isdead(srv, g_h1) == 0 && ffrdp_isdead(srv, g_h2) == 0, "按句柄问：两条连接都活着（0）");
    // 句柄现在是"代号*16 + 槽" ⇒ 只有负数算参数错 ✓；像 FFRDP_MAXCONN(16) 这种"某个旧代号"的句柄
    // 查表对不上 ⇒ 按"那条连接已经没了"报已断(1 ✓)，绝不会误打到新连接上 ✓（槽位回收的前提 ✓）。
    check(ffrdp_isdead(srv, -1) == -1, "负句柄 ⇒ -1（参数错）");
    check(ffrdp_isdead(srv, FFRDP_MAXCONN) == 1, "未知/陈旧句柄 ⇒ 已断(1)，不报参数错 ✓");

    /* ── 阶段 1b：FIN ⇒ DISCONN + 释放那条连接 ──────────────────────────── */
    printf("── 阶段 1b：客户端 1 那一侧发 FIN\n");
    {
        int h1 = g_h1;
        sim_begin(0); sim_send_from(0, A_C1, fin, hs_frame(fin, FFRDP_HS_FIN));   // 以客户端 1 的身份投一个 FIN
        sim_begin(0); sim_ll_run(0, p_srv);
        check(g_disconn == 1, "服务端收到 FIN ⇒ 报了 DISCONN");
        check(srv->table[h1] == NULL, "那条连接被放掉（表槽清空）");
        check(ffrdp_isdead(srv, h1) == 1, "那条连接被放掉 ⇒ 按句柄问是**死**（1）");
        check(srv->nconn == 3, "水位不回收（索引永不复用 ✓）");
    }
    ffrdp_exit(p_c1); ffrdp_exit(p_c2); ffrdp_exit(p_srv);

    /* ── 阶段 2：握手丢包 ⇒ 重传后仍连上 ────────────────────────────────── */
    printf("── 阶段 2：把客户端第一个 SYN 丢掉，看它重传\n");
    g_connect = g_accept = g_disconn = 0; g_h1 = g_h2 = -1;
    sim_reset();
    p_srv = ffrdp_init("ffrdps://127.0.0.1:8001,mss:1024,fec:0,hs:1", on_event, NULL);
    p_c1  = ffrdp_init("ffrdpc://127.0.0.1:8001,mss:1024,fec:0,hs:1", on_event, NULL);
    check(p_srv && p_c1, "服务端 + 客户端建起来了");
    if (!p_srv || !p_c1) { printf("── 结果：FAIL\n"); return 1; }
    srv = (FFRDPCONTEXT*)p_srv; c1 = (FFRDPCONTEXT*)p_c1;
    sim_ll_own(srv->lowlevel, A_SRV); sim_ll_own(c1->lowlevel, A_C1);
    sim_ll_route(srv->lowlevel, A_C1, 1); sim_ll_route(c1->lowlevel, A_SRV, 0);

    // 注意时序：客户端是在 ffrdp_init 里就发 SYN 的 ✓，而那时路由表还没登记 ⇒ 那个 SYN 本身就是丢掉的 ✓。
    // 这里再把 slot 0 里可能等着的东西一并倒掉，确保服务端从一开始就看不到任何 SYN ⇒ 只能靠重传 ✓。
    sim_setstep(0);
    sim_begin(1); sim_ll_run(1, p_c1);
    sim_begin(0); n = sim_recv(drop, (int)sizeof(drop));                // 倒掉（可能没东西 ⇒ -1）
    printf("  （丢掉 %d 字节；客户端 init 时的 SYN 已经先丢了 ⇒ 服务端一个 SYN 都没看到）\n", n);
    for (i = 1; i < 1500; i++) {
        sim_setstep(i);
        sim_begin(1); sim_ll_run(1, p_c1);
        sim_begin(0); sim_ll_run(0, p_srv);
        if (g_accept == 1) break;
    }
    check(g_connect == 1 && g_accept == 1, "重传之后握手仍然成功");
    check(c1->conn.hs_retry >= 2, "客户端重传过（hs_retry ≥ 2，即首发 + ≥1 次重传）");
    ffrdp_exit(p_c1); ffrdp_exit(p_srv);

    /* ── 阶段 3：对端从不回答 ⇒ 超时判死 ───────────────────────────────── */
    printf("── 阶段 3：对端从不回答（handshake_ms:1000）\n");
    g_connect = g_accept = g_disconn = 0;
    sim_reset();
    p_cc = ffrdp_init("ffrdpc://127.0.0.1:8001,mss:1024,fec:0,hs:1,handshake_ms:1000", on_event, NULL);
    p_sc = ffrdp_init("ffrdps://127.0.0.1:8001,mss:1024,fec:0,hs:1", on_event, NULL);
    check(p_cc && p_sc, "客户端与服务端建起来了（服务端故意一次都不驱动）");
    if (!p_cc || !p_sc) { printf("── 结果：FAIL\n"); return 1; }
    cc = (FFRDPCONTEXT*)p_cc; sc = (FFRDPCONTEXT*)p_sc;
    sim_ll_own(cc->lowlevel, A_C2); sim_ll_own(sc->lowlevel, A_SRV);
    sim_ll_route(cc->lowlevel, A_SRV, 0);
    for (i = 0; i < 1300; i++) { sim_setstep(i); sim_begin(1); sim_ll_run(1, p_cc); }   // 只驱动客户端
    check(g_connect == 0 && g_disconn == 1, "握手超时 ⇒ 报了 DISCONN、且从未 CONNECT");
    check(cc->conn.hs_state == HS_FAILED, "客户端进入 HS_FAILED");
    memset(sbuf, 'Z', sizeof(sbuf));
    check(ffrdp_send(cc, 0, sbuf, 16, 0) == FFRDP_CLOSED, "此后 send ⇒ CLOSED(0)");
    check(ffrdp_recv(cc, 0, rbuf, sizeof(rbuf), 0) == FFRDP_CLOSED, "此后 recv(timeout=0) ⇒ CLOSED(0)（判死排在等不等之前 ✓）");
    ffrdp_exit(p_cc); ffrdp_exit(p_sc);

    /* ── 阶段 4：服务端半开连接：计时封顶 + 半开期间的数据一律丢 ───────────── */
    printf("── 阶段 4：服务端收到 SYN、却没人完成握手（handshake_ms:1000）\n");
    g_connect = g_accept = g_disconn = 0;
    sim_reset();
    p_sc = ffrdp_init("ffrdps://127.0.0.1:8001,mss:1024,fec:0,hs:1,handshake_ms:1000,maxconn:8", on_event, NULL);
    check(p_sc != NULL, "服务端建起来了");
    if (!p_sc) { printf("── 结果：FAIL\n"); return 1; }
    sc = (FFRDPCONTEXT*)p_sc;

    sim_setstep(0);
    sim_begin(0); sim_send_from(0, A_X, hsbuf, hs_frame(hsbuf, FFRDP_HS_SYN));   // 裸 SYN：对面没有协议栈
    sim_begin(0); sim_ll_run(0, p_sc);
    check(sc->nconn == 2, "为这个 SYN 建了一条半开连接（nconn=2）");
    check(sc->table[1] && sc->table[1]->peer_addr == A_X, "记着 SYN 的来源地址");
    check(sc->table[1] && sc->table[1]->hs_state == HS_SYN_RCVD, "状态 = HS_SYN_RCVD（半开）");
    check(g_accept == 0, "半开**不算** ACCEPT（对端还没完成握手）");

    {   // 半开期间来的数据帧必须丢（设计稿：未握手一律丢弃）
        int before = sc->table[1]->counter_rx_foreign;
        sim_begin(0); sim_send_from(0, A_X, "hello", 5);
        sim_begin(0); sim_ll_run(0, p_sc);
        check(sc->table[1] && sc->table[1]->counter_rx_foreign >= before + 1, "半开连接上的数据帧被丢掉（计数 +1）");
        check(ffrdp_recv(sc, 1, rbuf, sizeof(rbuf), 0) == FFRDP_EAGAIN, "半开连接上读不到数据（丢掉而不是排队）");
    }

    for (i = 1; i < 1400; i++) { sim_setstep(i); sim_begin(0); sim_ll_run(0, p_sc); }
    check(sc->table[1] == NULL, "超过 handshake_ms ⇒ 半开放掉（表槽清空）");
    check(sc->nconn == 2, "水位不回收（nconn 只是遍历上界 ✓）");
    check(g_disconn == 1 && g_accept == 0, "报了 DISCONN、且始终没有 ACCEPT");

    sim_setstep(2000);
    sim_begin(0); sim_send_from(0, A_Y, hsbuf, hs_frame(hsbuf, FFRDP_HS_SYN));
    sim_begin(0); sim_ll_run(0, p_sc);
    // **槽位回收 ✓**：1 号空出来了 ⇒ 新 SYN 重新拿 1 号，但代号 +1 ⇒ 句柄 = 1*16+1 = 17 ✓
    check(sc->table[1] && sc->table[1]->peer_addr == A_Y, "新 SYN 复用 1 号槽（槽位回收 ✓）");
    check(sc->table[1] && sc->table[1]->handle == 17, "复用槽的句柄换了代号（17 = 1*16+1 ✓ 陈旧句柄不会撞上 ✓）");
    check(ffrdp_isdead(sc, 1) == 1, "旧句柄 1 现在算无效/已死（代号对不上 ✓）");
    ffrdp_exit(p_sc);

    /* ── 阶段 5：maxconn 满了就不再接（设计稿接受的取舍）────────────────── */
    printf("── 阶段 5：maxconn:2 ⇒ 第二个 SYN 被拒\n");
    g_connect = g_accept = g_disconn = 0;
    sim_reset();
    p_sc = ffrdp_init("ffrdps://127.0.0.1:8001,mss:1024,fec:0,hs:1,maxconn:2,handshake_ms:1000", on_event, NULL);
    check(p_sc != NULL, "服务端建起来了（maxconn:2 ⇒ 除主连接外只多接一条）");
    if (!p_sc) { printf("── 结果：FAIL\n"); return 1; }
    sc = (FFRDPCONTEXT*)p_sc;

    sim_setstep(0);
    sim_begin(0); sim_send_from(0, A_X, hsbuf, hs_frame(hsbuf, FFRDP_HS_SYN));
    sim_begin(0); sim_ll_run(0, p_sc);
    check(sc->nconn == 2 && sc->table[1] != NULL, "第一个 SYN 建了 1 号连接");
    sim_begin(0); sim_send_from(0, A_Y, hsbuf, hs_frame(hsbuf, FFRDP_HS_SYN));
    sim_begin(0); sim_ll_run(0, p_sc);
    check(sc->table[0]->counter_hs_refused == 1, "第二个 SYN 被拒、并计入 counter_hs_refused（不静默 ✓）");
    check(sc->nconn == 2 && sc->table[2] == NULL, "没有建出第三条连接");

    // 5b：那个半开连接过了 handshake_ms 会被放掉 ⇒ **槽位回收 ✓**：新 SYN 立刻能进来（句柄换代号 ✓）。
    // 这正是 6 客户端那轮"服务端一生只接 15 条"的病根（notes §26.100 ✓）。
    {   int k;
        int refused = sc->table[0]->counter_hs_refused;
        for (k = 1; k < 1200; k++) { sim_setstep(k); sim_begin(0); sim_ll_run(0, p_sc); }
        check(sc->table[1] == NULL, "半开连接超时被放掉（1 号槽空了 ✓）");
        check(sc->nconn == 2, "水位不回收（仍是 2 ✓）");
        sim_begin(0); sim_send_from(0, A_Y, hsbuf, hs_frame(hsbuf, FFRDP_HS_SYN));
        sim_begin(0); sim_ll_run(0, p_sc);
        check(sc->table[1] != NULL, "槽空出来了 ⇒ 新 SYN **接得上**（槽位回收 ✓，不再「一生只接 maxconn-1 条」✗）");
        check(sc->table[1] && sc->table[1]->peer_addr == A_Y, "确实是新对端占了这个槽 ✓");
        check(sc->table[1] && sc->table[1]->handle == 17, "句柄 = 17（代号 1 ⇒ 与旧句柄 1 不撞 ✓）");
        check(sc->table[0]->counter_hs_refused == (uint32_t)refused, "没再记「拒连」（这次是接上了 ✓）");
        check(sc->nconn == 2, "水位没变（复用同一个槽 ✓）");
    }
    ffrdp_exit(p_sc);

    /* ── 阶段 6：主动关闭（ffrdp_close）—— 四次挥手里"我方发起"的那一半 ──── */
    // 要验的三件事：① close 受理后进 FIN_WAIT、**先不报** DISCONN（还没等到对方 ✓）；
    //               ② 收到对端的 FIN 才报 DISCONN 并收摊；③ 期间连接一直都在（不做半关 ✗）。
    // 手法照这个文件的习惯：直接注入握手帧驱动服务端 ✓（不需要真路由 ✓）。
    printf("── 阶段 6：ffrdp_close（主动关：我方发 FIN，等对端 FIN 才断）\n");
    g_connect = g_accept = g_disconn = 0;
    sim_reset();
    p_srv = ffrdp_init("ffrdps://127.0.0.1:8001,mss:1024,fec:0,hs:1", on_event, NULL);
    check(p_srv != NULL, "服务端建起来了");
    if (p_srv) {
        FFRDPCONTEXT *s6 = (FFRDPCONTEXT*)p_srv;
        int           st = 0;

        sim_setstep(st++); sim_begin(0); sim_send_from(0, A_X, hsbuf, hs_frame(hsbuf, FFRDP_HS_SYN));
        sim_begin(0); sim_ll_run(0, p_srv);
        sim_setstep(st++); sim_begin(0); sim_send_from(0, A_X, hsbuf, hs_frame(hsbuf, FFRDP_HS_ACK));
        sim_begin(0); sim_ll_run(0, p_srv);
        check(g_accept == 1 && s6->table[1] != NULL && s6->table[1]->hs_state == HS_UP,
              "先握手成功（连接 UP ✓，ACCEPT ×1）");

        check(ffrdp_close(p_srv, 1) == 0, "ffrdp_close(服务端, accept 句柄) 受理 ✓");
        check(s6->table[1] != NULL && s6->table[1]->hs_state == HS_FIN_WAIT,
              "进入 FIN_WAIT（FIN 已发、等对端 ✓）");
        check(g_disconn == 0, "还没等到对端 FIN ⇒ **先不报** DISCONN ✓");
        check(s6->table[1] != NULL, "连接仍在（不做半关，但也没提前拆 ✗）");

        sim_setstep(st++); sim_begin(0); sim_send_from(0, A_X, hsbuf, hs_frame(hsbuf, FFRDP_HS_FIN));
        sim_begin(0); sim_ll_run(0, p_srv);
        check(g_disconn == 1, "收到对端 FIN ⇒ 报 DISCONN ✓");
        check(s6->table[1] == NULL, "连接被收掉（表槽清空 ✓）");
        check(ffrdp_isdead(p_srv, 1) == 1, "按句柄问 ⇒ 已死（1 ✓）");
        check(ffrdp_close(p_srv, 1) == -1, "对已关掉的句柄再 close ⇒ -1（无效句柄 ✓）");
        // 已放掉的句柄上 send/recv：**必须报错，不能崩** ✓
        //（真机上这里原本是空指针解引用 ⇒ SIGSEGV：对端主动关连接后应用照旧 recv 就踩到 ✓）
        check(ffrdp_recv(p_srv, 1, rbuf, sizeof(rbuf), 0) == FFRDP_CONN_INVALID,
              "对已放掉的句柄 recv ⇒ CONN_INVALID（不崩 ✓）");
        check(ffrdp_send(p_srv, 1, rbuf, 1, 0) == FFRDP_CONN_INVALID,
              "对已放掉的句柄 send ⇒ CONN_INVALID（不崩 ✓）");
        // 已放掉的句柄上 send/recv：**必须报错，不能崩** ✓
        //（真机上这里原本是空指针解引用 ⇒ SIGSEGV：对端主动关连接后应用照旧 recv 就踩到 ✓）
        check(ffrdp_recv(p_srv, 1, rbuf, sizeof(rbuf), 0) == FFRDP_CONN_INVALID,
              "对已放掉的句柄 recv ⇒ CONN_INVALID（不崩 ✓）");
        check(ffrdp_send(p_srv, 1, rbuf, 1, 0) == FFRDP_CONN_INVALID,
              "对已放掉的句柄 send ⇒ CONN_INVALID（不崩 ✓）");
        // 已放掉的句柄上 send/recv：**必须报错，不能崩** ✓
        //（真机上这里原本是空指针解引用 ⇒ SIGSEGV：对端主动关连接后应用照旧 recv 就踩到 ✓）
        check(ffrdp_recv(p_srv, 1, rbuf, sizeof(rbuf), 0) == FFRDP_CONN_INVALID,
              "对已放掉的句柄 recv ⇒ CONN_INVALID（不崩 ✓）");
        check(ffrdp_send(p_srv, 1, rbuf, 1, 0) == FFRDP_CONN_INVALID,
              "对已放掉的句柄 send ⇒ CONN_INVALID（不崩 ✓）");
        check(s6->nconn == 2, "水位不回收（索引永不复用 ✓）");
        ffrdp_exit(p_srv);
    }

    /* ── 阶段 7：客户端侧收到对端 FIN（四次挥手的另一半）────────────────── */
    // 服务端那条路阶段 6 验过了；客户端这条**不能少** ✗：对端先关时，回 FIN 是两边都要做的事 ✓。
    // 与阶段 6 的区别：客户端只有主连接 ⇒ 收 FIN 后**不能摘表** ✗（表槽永不回收 ✓），
    // 而是置 HS_FAILED ⇒ isdead 报 1 ✓、send/recv 报 CLOSED(0) ✓。
    printf("── 阶段 7：客户端收到对端 FIN\n");
    g_connect = g_accept = g_disconn = 0;
    sim_reset();
    p_c1 = ffrdp_init("ffrdpc://127.0.0.1:8001,mss:1024,fec:0,hs:1", on_event, NULL);
    check(p_c1 != NULL, "客户端建起来了");
    if (p_c1) {
        FFRDPCONTEXT *c7 = (FFRDPCONTEXT*)p_c1;
        int           st = 0;
        uint64_t      srv_addr = 0;

        sim_setstep(st++); sim_begin(0); sim_ll_run(0, p_c1);          // 把 init 时那个 SYN 发出去/丢掉
        srv_addr = c7->table[0] ? c7->table[0]->peer_addr : 0;         // 对端地址就用它自己的 peer_addr ✓
        sim_setstep(st++); sim_begin(0); sim_send_from(0, srv_addr, hsbuf, hs_frame(hsbuf, FFRDP_HS_SYN_ACK));
        sim_begin(0); sim_ll_run(0, p_c1);
        check(g_connect == 1 && c7->table[0] && c7->table[0]->hs_state == HS_UP,
              "先握手成功（客户端 UP ✓）");

        sim_setstep(st++); sim_begin(0); sim_send_from(0, srv_addr, hsbuf, hs_frame(hsbuf, FFRDP_HS_FIN));
        sim_begin(0); sim_ll_run(0, p_c1);
        check(g_disconn == 1, "对端 FIN ⇒ 报 DISCONN ✓");
        check(c7->table[0] != NULL, "主连接**仍在表里**（表槽永不回收 ✓，不像服务端那样摘掉 ✗）");
        check(ffrdp_isdead(p_c1, 0) == 1, "客户端主连接 ⇒ 已死（isdead = 1 ✓）");
        check(ffrdp_recv(p_c1, 0, rbuf, sizeof(rbuf), 0) == FFRDP_CLOSED, "此后 recv ⇒ CLOSED(0) ✓");
        check(ffrdp_send(p_c1, 0, rbuf, 1, 0) == FFRDP_CLOSED, "此后 send ⇒ CLOSED(0) ✓");
        ffrdp_exit(p_c1);
    }

    /* ── 阶段 8：**客户端主动** close（我方发 FIN，等对端 FIN）──────────────── */
    // 阶段 6 验的是服务端那一侧的主动关；这里补客户端这一侧（两半都要对称 ✓）：
    //   ① close 受理 ⇒ 进 FIN_WAIT、FIN 真的从低层发出去 ✓、**先不报** DISCONN ✓
    //   ② 收到对端 FIN ⇒ 报 DISCONN ✓ + 主连接**不摘表**（置 HS_FAILED ✓）
    printf("── 阶段 8：客户端主动 close（hs:1）\n");
    g_connect = g_accept = g_disconn = 0;
    sim_reset();
    p_c1 = ffrdp_init("ffrdpc://127.0.0.1:8001,mss:1024,fec:0,hs:1", on_event, NULL);
    check(p_c1 != NULL, "客户端建起来了");
    if (p_c1) {
        FFRDPCONTEXT *c8 = (FFRDPCONTEXT*)p_c1;
        uint64_t      srv_addr;
        int           st = 0, q0;
        sim_setstep(st++); sim_begin(0); sim_ll_run(0, p_c1);
        srv_addr = c8->table[0] ? c8->table[0]->peer_addr : 0;
        sim_setstep(st++); sim_begin(0); sim_send_from(0, srv_addr, hsbuf, hs_frame(hsbuf, FFRDP_HS_SYN_ACK));
        sim_begin(0); sim_ll_run(0, p_c1);
        check(g_connect == 1 && c8->table[0] && c8->table[0]->hs_state == HS_UP, "先握手成功 ✓");

        q0 = sim_queued(1);
        check(ffrdp_close(p_c1, 0) == 0, "ffrdp_close(客户端, 0) 受理 ✓");
        check(c8->table[0] && c8->table[0]->hs_state == HS_FIN_WAIT, "进入 FIN_WAIT ✓");
        sim_setstep(st++); sim_begin(0); sim_ll_run(0, p_c1);
        check(sim_queued(1) > q0, "FIN **真的发出去了**（对端 slot 队列增加 ✓）");
        check(g_disconn == 0, "还没等到对端 FIN ⇒ 先不报 DISCONN ✓");

        sim_setstep(st++); sim_begin(0); sim_send_from(0, srv_addr, hsbuf, hs_frame(hsbuf, FFRDP_HS_FIN));
        sim_begin(0); sim_ll_run(0, p_c1);
        check(g_disconn == 1, "收到对端 FIN ⇒ 报 DISCONN ✓");
        check(c8->table[0] != NULL, "主连接**仍在表里**（不像服务端那样摘掉 ✗）");
        check(ffrdp_isdead(p_c1, 0) == 1, "此后 isdead ⇒ 1 ✓");
        check(ffrdp_recv(p_c1, 0, rbuf, sizeof(rbuf), 0) == FFRDP_CLOSED, "此后 recv ⇒ CLOSED(0) ✓");
        ffrdp_exit(p_c1);
    }

    /* ── 阶段 9：`hs:0` 下的 ffrdp_close（没有 FIN 这东西 ⇒ 只做本地拆除 ✓）────── */
    printf("── 阶段 9：hs:0 下的 ffrdp_close（本地拆除 ✓）\n");
    g_connect = g_accept = g_disconn = 0;
    sim_reset();
    p_sc = ffrdp_init("ffrdps://127.0.0.1:8001,mss:1024,fec:0,hs:0", on_event, NULL);
    check(p_sc != NULL, "服务端（hs:0）建起来了");
    if (p_sc) {
        sc = (FFRDPCONTEXT*)p_sc;
        sim_setstep(0);
        sim_begin(0); sim_send_from(0, A_X, "hello", 5);      // 老路径：第一帧就学会对端 ✓
        sim_begin(0); sim_ll_run(0, p_sc);
        check((sc->table[0]->flags & FLAG_CONNECTED) != 0, "老路径对端已学会（FLAG_CONNECTED ✓）");
        check(ffrdp_close(p_sc, 0) == 0, "ffrdp_close 受理 ✓");
        check(g_disconn == 1, "报了一次 DISCONN ✓（老路径没有 FIN 可发 ⇒ 就地拆除 ✓）");
        check(sc->table[0] == NULL, "主连接被本地拆除（表槽清空 ✓）");
        check(ffrdp_isdead(p_sc, 0) == 1, "此后按句柄问 ⇒ 已死（1 ✓）");
        check(ffrdp_close(p_sc, 0) == -1, "重复 close ⇒ -1（无效句柄 ✓）");
        ffrdp_exit(p_sc);
    }

    check(sim_alloc() == sim_freed(), "全部还回去：sim_alloc == sim_freed（无泄漏）");
    printf("── 结果：%s\n", g_fail ? "FAIL" : "OK");
    printf("── verdict -> %s\n", g_fail ? "FAIL" : "OK");
    (void)ll_sc; (void)ll_cc; (void)ll_c1; (void)ll_c2; (void)ll_srv;
    return 0;
}
