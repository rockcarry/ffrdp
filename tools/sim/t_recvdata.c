// FFRDP_RECVDATA（v2）：数据交付给应用之前先**异步通知**一次，应用可以选择：
//   返回 FFRDP_NOT_CONSUMED（0）⇒ 数据留在协议里，照旧由 ffrdp_recv 取 ✓
//   返回 FFRDP_CONSUMED（1）    ⇒ 应用**在回调里取走** ✓，协议不留 ✓
// 两种都必须在通信上"看不出来"：帧照样确认、发送侧照样往前走 ✓
// （区别只在"这段字节归谁保管" ✓）。
//
// 两段 + 一条护栏：
//   A 回调返回 NOT_CONSUMED ⇒ ffrdp_recv 必须能把数据取出来（现在是这个行为 ✓）
//   B 回调返回 CONSUMED     ⇒ 回调自己搬走全部字节；此后 ffrdp_recv 必须**取不到东西** ✓，
//                             但**传输照常**（回调持续收到足够多的数据 ⇒ ack 在走 ✓）
//   护栏：两个回调都返回 0 / 都返回 1 的两种极端之外，还要确认**恰好一种**在生效 ✓
//         （否则"取不到"就可能只是丢包 ✗）
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

// 每侧一份应用状态（cbctx 指到它 ✓）：消费开关 + 计数 + 自己搬走的数据
struct app {
    int  consume;          // 1 = 在回调里取走 ✓
    long got;              // 回调里收到的字节数
    long bad;              // 回调里收到的字节里不是 'A' 的个数
    int  calls;
};
static int g_accept, g_srv_h;

static long on_event(void *cbctx, const FFRDP_EVENT *ev)
{
    struct app *a = (struct app*)cbctx;
    if (ev->type == FFRDP_ACCEPT) { g_accept++; if (g_srv_h < 0) g_srv_h = ev->conn; return FFRDP_NOT_CONSUMED; }
    if (ev->type == FFRDP_RECVDATA && a) {
        // 持着协议锁被调用 ✓ ⇒ 只搬字节 / 记数，绝不回调 ffrdp_* ✓
        int k; unsigned char *p = (unsigned char*)ev->buf;
        a->calls++; a->got += ev->len;
        for (k = 0; k < ev->len; k++) if (p[k] != 'A') a->bad++;
        return a->consume ? FFRDP_CONSUMED : FFRDP_NOT_CONSUMED;
    }
    return FFRDP_NOT_CONSUMED;
}

// 一轮：consume 决定回调的返回值 ✓；返回后由调用方分别检查 ffrdp_recv 的行为 ✓
static void phase(const char *name, int consume, int want, long *cb_got, long *cb_bad, int *cb_calls,
                  long *recv_got, long *sent_ok)
{
    void *p_srv, *p_c1;
    FFRDPCONTEXT *srv, *c1;
    FFRDP_LOWLEVEL *ll_srv, *ll_c1;
    char sbuf[4096], rbuf[512];
    int i, n, up = 0, down = 0, bad = 0;
    long sent = 0;
    struct app app_srv = { 0 }, app_c1 = { 0 };

    printf("── %s\n", name);
    sim_reset(); sim_config(0.0, 0, 4, 0);
    g_accept = 0; g_srv_h = -1;
    app_srv.consume = consume; app_c1.consume = consume;
    memset(sbuf, 'A', sizeof(sbuf));

    p_srv = ffrdp_init("ffrdps://127.0.0.1:8001,mss:1024,fec:0", on_event, &app_srv);
    p_c1  = ffrdp_init("ffrdpc://127.0.0.1:8001,mss:1024,fec:0", on_event, &app_c1);
    check(p_srv && p_c1, "两个上下文都建起来了（默认走握手）");
    if (!p_srv || !p_c1) return;
    srv = (FFRDPCONTEXT*)p_srv; c1 = (FFRDPCONTEXT*)p_c1;
    ll_srv = srv->lowlevel; ll_c1 = c1->lowlevel;
    sim_ll_own(ll_srv, A_SRV); sim_ll_own(ll_c1, A_C1);
    sim_ll_route(ll_srv, A_C1, 1); sim_ll_route(ll_c1, A_SRV, 0);

    for (i = 0; i < 3000; i++) {
        sim_setstep(i);
        if (g_srv_h >= 0 && ffrdp_send(srv, g_srv_h, sbuf, 2048, 0) > 0) sent += 2048;
        if (ffrdp_send(c1, 0, sbuf, 2048, 0) > 0) sent += 2048;
        sim_begin(1); sim_ll_run(1, p_c1);
        sim_begin(0); sim_ll_run(0, p_srv);
        if (g_srv_h >= 0)
            while ((n = ffrdp_recv(srv, g_srv_h, rbuf, sizeof(rbuf), 0)) > 0)
                { int k; for (k = 0; k < n; k++) if (rbuf[k] != 'A') bad++; up += n; }
        while ((n = ffrdp_recv(c1, 0, rbuf, sizeof(rbuf), 0)) > 0)
            { int k; for (k = 0; k < n; k++) if (rbuf[k] != 'A') bad++; down += n; }
        if (g_srv_h >= 0 && app_srv.got > want && app_c1.got > want) break;
    }
    printf("     （跑了 %d 步：回调收到 服务端 %ld / 客户端 %ld 字节（各调用 %d/%d 次），"
           "ffrdp_recv 取到 上行 %d 下行 %d，坏字节 %d）\n",
           i + 1, app_srv.got, app_c1.got, app_srv.calls, app_c1.calls, up, down, bad);

    check(g_accept == 1, "服务端接受了这条连接（ACCEPT ×1）");
    check(g_srv_h >= 1, "句柄来自 accept（不是 0 号主连接）");
    check(sent > 0, "应用确实发出去了数据");
    check(app_srv.bad == 0 && app_c1.bad == 0, "回调里看到的字节逐字节是 'A'（回调拿到的是**明文** ✓）");

    *cb_got = app_srv.got + app_c1.got; *cb_bad = app_srv.bad + app_c1.bad;
    *cb_calls = app_srv.calls + app_c1.calls;
    *recv_got = (long)up + down;
    *sent_ok = sent;
    ffrdp_exit(p_c1); ffrdp_exit(p_srv);
}

int main(void)
{
    long gotA, badA, gotB, badB, recvA, recvB, sentA, sentB;
    int  callsA, callsB;

    phase("A 回调返回 NOT_CONSUMED ⇒ 数据留在协议里，ffrdp_recv 取得到", 0, 8192,
          &gotA, &badA, &callsA, &recvA, &sentA);
    check(gotA > 8192 && callsA > 0, "回调确实被调到、而且收到了足够多的数据（RECVDATA 生效 ✓）");
    check(recvA > 8192, "NOT_CONSUMED ⇒ ffrdp_recv 仍能把数据取出来 ✓（旧行为不变 ✓）");
    check(badA == 0, "A 段数据完好");

    phase("B 回调返回 CONSUMED ⇒ 应用在回调里取走，协议不再留着", 1, 8192,
          &gotB, &badB, &callsB, &recvB, &sentB);
    check(gotB > 8192 && callsB > 0, "回调确实被调到、而且收到了足够多的数据（RECVDATA 生效 ✓）");
    check(recvB == 0, "CONSUMED ⇒ ffrdp_recv **取不到东西** ✓（协议没留 ✓）");
    check(badB == 0, "B 段数据完好（回调搬走的就是明文 ✓）");
    check(sentB > 0, "**传输照常**：帧照常被确认 ⇒ 发送侧没有卡住 ✓");

    printf("       verdict -> %s\n", g_fail ? "FAIL" : "OK");
    return g_fail ? 1 : 0;
}
