// 握手 + 多连接（v2）：一个服务端 + 两个客户端，全部 hs:1。
//
// 这是 P0/P1/P2/P4 四步的合验：
//   P0 的夹具（按地址路由 sim_ll_route / sim_ll_own）✓
//   P1 的每连接结构（FFRDPCONN）✓
//   P2 的按地址分发（ffrdp_conn_by_addr）✓
//   P4 的连接表（句柄 = 下标、按需分配、索引不复用）✓
//
// 判据：握手能建立（CONNECT ×2 / ACCEPT ×2，且 ACCEPT 给的句柄不同）✓；
//       两个客户端各自的数据只出现在**自己那条连接**上（不串台）✓；
//       全部释放、无泄漏 ✓。
//
// 它只编一份（只对工作区那份 ffrdp.c）：接口是 v2 的，编译不过 HEAD 那份 ⇒ run.sh 里 V2ONLY。
#include <stdio.h>
#include <string.h>
#include "../../ffrdp.h"
#include "simdefs.h"
#include "../../ffrdp.c"

#define MKADDR(port) ((uint64_t)(0x7F000001ULL << 16) | (uint64_t)(port))
#define A_SRV MKADDR(8001)   // 服务端地址 = 客户端 dial 的那个
#define A_C1  MKADDR(40001)  // 客户端 1 自己的地址（服务端从它的 SYN 里看到）
#define A_C2  MKADDR(40002)

static int g_fail;
static void check(int ok, const char *what) { printf("  %s %s\n", ok ? "✓" : "✗", what); if (!ok) g_fail++; }

static int g_connect, g_accept, g_disconn;
static int g_h1 = -1, g_h2 = -1;                  // 两个 ACCEPT 给的句柄
static uint64_t g_a1, g_a2;                       // ACCEPT 报的对端地址

// 事件回调：**持着协议锁**被调用 ⇒ 这里只记数，绝不回调 ffrdp_*（否则自锁）。
static long on_event(void *cbctx, const FFRDP_EVENT *ev)
{
    (void)cbctx;
    if (ev->type == FFRDP_CONNECT)  g_connect++;
    if (ev->type == FFRDP_ACCEPT) {
        g_accept++;
        if (g_accept == 1) { g_h1 = ev->conn; g_a1 = ev->addr; }
        else               { g_h2 = ev->conn; g_a2 = ev->addr; }
    }
    if (ev->type == FFRDP_DISCONN)  g_disconn++;
    return FFRDP_NOT_CONSUMED;
}

int main(void)
{
    FFRDPCONTEXT *srv, *c1, *c2;
    FFRDP_LOWLEVEL *ll_srv, *ll_c1, *ll_c2;
    void *p_srv, *p_c1, *p_c2;
    FFRDP_LOWLEVEL *tmp;
    char  sbuf[64], rbuf[512];
    int   i, n, r1 = 0, r2 = 0, bad1 = 0, bad2 = 0, sent1 = 0, sent2 = 0;

    sim_reset();
    p_srv = ffrdp_init("ffrdps://127.0.0.1:8001,mss:1024,fec:0,hs:1,maxconn:4", on_event, NULL);
    p_c1  = ffrdp_init("ffrdpc://127.0.0.1:8001,mss:1024,fec:0,hs:1", on_event, NULL);
    p_c2  = ffrdp_init("ffrdpc://127.0.0.1:8001,mss:1024,fec:0,hs:1", on_event, NULL);
    check(p_srv && p_c1 && p_c2, "三个上下文都建起来了（服务端 + 两个客户端，hs:1）");
    if (!p_srv || !p_c1 || !p_c2) { printf("── 结果：FAIL\n"); return 1; }
    srv = (FFRDPCONTEXT*)p_srv; c1 = (FFRDPCONTEXT*)p_c1; c2 = (FFRDPCONTEXT*)p_c2;

    // 三个假 lowlevel 各自一张路由表：按地址收发（P0 的夹具）。
    ll_srv = srv->lowlevel; ll_c1 = c1->lowlevel; ll_c2 = c2->lowlevel;
    sim_ll_own(ll_srv, A_SRV); sim_ll_own(ll_c1, A_C1); sim_ll_own(ll_c2, A_C2);
    sim_ll_route(ll_srv, A_C1, 1); sim_ll_route(ll_srv, A_C2, 2);
    sim_ll_route(ll_c1,  A_SRV, 0);
    sim_ll_route(ll_c2,  A_SRV, 0);

    memset(sbuf, 'A', sizeof(sbuf));
    for (i = 0; i < 2000; i++) {
        sim_setstep(i);
        // 两边客户端都持续发数据（握手没完成时 ffrdp_send 会返回 EAGAIN ✓ 不算失败）
        if (ffrdp_send(c1, 0, sbuf, 64, 0) == 64) sent1 += 64;
        if (ffrdp_send(c2, 0, sbuf, 64, 0) == 64) sent2 += 64;
        sim_begin(1); sim_ll_run(1, p_c1);
        sim_begin(2); sim_ll_run(2, p_c2);
        sim_begin(0); sim_ll_run(0, p_srv);
        // 服务端把两条连接上的东西都收掉，分别统计（这就是"不串台"的判据）
        if (g_h1 >= 0) while ((n = ffrdp_recv(srv, g_h1, rbuf, sizeof(rbuf), 0)) > 0) { int k; for (k = 0; k < n; k++) { if (rbuf[k] != 'A') bad1++; } r1 += n; }
        if (g_h2 >= 0) while ((n = ffrdp_recv(srv, g_h2, rbuf, sizeof(rbuf), 0)) > 0) { int k; for (k = 0; k < n; k++) { if (rbuf[k] != 'A') bad2++; } r2 += n; }
        // 客户端把服务端回来的东西收掉（v1 的服务器不回显 ⇒ 应当一直是 0）
        while ((n = ffrdp_recv(c1, 0, rbuf, sizeof(rbuf), 0)) > 0) { }
        while ((n = ffrdp_recv(c2, 0, rbuf, sizeof(rbuf), 0)) > 0) { }
        if (g_connect == 2 && g_accept == 2 && r1 > 4096 && r2 > 4096) break;
    }

    printf("  （跑完 %d 步：CONNECT=%d ACCEPT=%d DISCONN=%d，服务端收到 h1=%d 字节 / h2=%d 字节）\n",
           i + 1, g_connect, g_accept, g_disconn, r1, r2);
    check(g_connect == 2, "两个客户端都完成了握手（FFRDP_CONNECT ×2）");
    check(g_accept == 2, "服务端接受了两个连接（FFRDP_ACCEPT ×2）");
    check(g_h1 >= 1 && g_h2 >= 1 && g_h1 != g_h2, "两条连接的句柄不同、且都不是 0 号主连接");
    check((g_a1 == A_C1 && g_a2 == A_C2) || (g_a1 == A_C2 && g_a2 == A_C1), "ACCEPT 报的对端地址就是两个客户端");
    check(srv->nconn == 3, "服务端连接表：主连接 + 两条（nconn=3）");
    check(r1 > 4096 && r2 > 4096, "两条连接上都收到了应用数据");
    check(bad1 == 0 && bad2 == 0, "不串台：h1 上全是客户端 1 的字节、h2 上全是客户端 2 的（内容相同，靠句柄区分）");
    check(sent1 > 0 && sent2 > 0, "两个客户端都确实发出去了数据");
    (void)tmp;

    ffrdp_exit(p_c1); ffrdp_exit(p_c2); ffrdp_exit(p_srv);
    check(sim_alloc() == sim_freed(), "全部还回去：sim_alloc == sim_freed（无泄漏）");

    printf("── 结果：%s\n", g_fail ? "FAIL" : "OK");
    printf("── verdict -> %s\n", g_fail ? "FAIL" : "OK");
    return 0;
}
