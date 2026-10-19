// ①-45：接收侧**只有接收环这一块存储**（recv_buff 已删）⇒ 应用读路径要自己管游标。
// 这个用例就是那套游标的判据（v2-only ✓）：
//
//   A 延迟读 + 分片读（97 字节一次）：逐字节**按顺序**正确（内容用图案判），读空之后槽全部还回去 ✓
//   B 回调（RECVDATA 返回 CONSUMED）与 ffrdp_recv **混着来**：被回调取走的帧要靠"READ 记号"跳过，
//     两条路加起来刚好是发出去的字节数（少一份/多一份都是 bug ✗）
//   C 应用完全不读 ⇒ 环被未读帧占满 ⇒ 通告窗归零；应用一读，窗口**立刻**重开
//     （不是等对端 RTO 才发现 ✓ —— 那一条是 ①-45 顺手补的，这里钉住它 ✓）
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../ffrdp.h"
#include "simdefs.h"
#include "../../ffrdp.c"

#define MKADDR(port) ((uint64_t)(0x7F000001ULL << 16) | (uint64_t)(port))
#define A_SRV MKADDR(8001)
#define A_C1  MKADDR(40001)
#define MSS   1024

static int g_fail;
static void check(int ok, const char *what) { printf("  %s %s\n", ok ? "✓" : "✗", what); if (!ok) g_fail++; }

// 发送图案：按**全局偏移**算 ⇒ 顺序错一位就立刻能看出来 ✓
// （g_pattern = 0 时退化成常量 'A'：混着消费的那一段用不了"按偏移判"，两条路各自都不连续 ✗）
static int g_pattern = 1;
static unsigned char fillb(long off) { return g_pattern ? (unsigned char)((off * 7 + 3) & 0xFF) : (unsigned char)'A'; }

struct app {
    int  consume_every;   // 每第 N 次交付在回调里取走（0 = 从不取走 ✓）
    int  n;               // 回调被调的次数
    long got;             // 回调里取走的字节数
    long bad;             // 回调里看到的字节与图案/顺序不符的个数
};
static int g_accept, g_srv_h;

static long on_event(void *cbctx, const FFRDP_EVENT *ev)
{
    struct app *a = (struct app*)cbctx;
    if (ev->type == FFRDP_ACCEPT) { g_accept++; if (g_srv_h < 0) g_srv_h = ev->conn; return FFRDP_NOT_CONSUMED; }
    if (ev->type == FFRDP_RECVDATA && a) {
        // 持着协议锁被调用 ✓ ⇒ 只读字节 / 记数，绝不回调 ffrdp_* ✓
        unsigned char *p = (unsigned char*)ev->buf;
        int k;
        a->n++;
        if (a->consume_every && (a->n % a->consume_every) == 0) {
            a->got += ev->len;                       // 这一帧归应用（回调里取走 ✓）
            return FFRDP_CONSUMED;
        }
        // 不取走：协议里还留一份，稍后由 ffrdp_recv 再交一遍 ✓
        // 只有"全不取走"这一段才谈得上按偏移判序（否则两条路各自都不连续 ✗）
        if (!a->consume_every && g_pattern)
            for (k = 0; k < ev->len; k++) if (p[k] != fillb(a->got + k)) a->bad++;
        if (!a->consume_every) a->got += ev->len;
        return FFRDP_NOT_CONSUMED;
    }
    return FFRDP_NOT_CONSUMED;
}

static long g_step;   // 全局步数（虚拟时钟 1 步 = 1 ms ✓）
static void drive(void *p_c1, void *p_srv)
{
    sim_setstep((int)g_step++);
    sim_begin(1); sim_ll_run(1, p_c1);
    sim_begin(0); sim_ll_run(0, p_srv);
}

// 建一对（默认走握手）+ 登记路由 + **把握手推完**
// 注：握手第一条 SYN 会落到仿真网络"未登记路由"的旧回退里（1-cursid）⇒ 对端要等一次 HS
// 重传（约 500 ms 虚拟时间）才收到。所以这里先把时钟推到 ACCEPT 出现，各段就不用管这 500 步了 ✓
static int make_pair(void **p_srv, void **p_c1, struct app *app_srv, struct app *app_c1)
{
    int i;
    sim_reset(); sim_config(0.0, 0, 4, 0);
    g_step = 0; g_accept = 0; g_srv_h = -1;
    *p_srv = ffrdp_init("ffrdps://127.0.0.1:8001,mss:1024,fec:0", on_event, app_srv);
    *p_c1  = ffrdp_init("ffrdpc://127.0.0.1:8001,mss:1024,fec:0", on_event, app_c1);
    if (!*p_srv || !*p_c1) return -1;
    {   FFRDPCONTEXT *s = (FFRDPCONTEXT*)*p_srv, *c = (FFRDPCONTEXT*)*p_c1;
        sim_ll_own(s->lowlevel, A_SRV); sim_ll_own(c->lowlevel, A_C1);
        sim_ll_route(s->lowlevel, A_C1, 1); sim_ll_route(c->lowlevel, A_SRV, 0);
    }
    for (i = 0; i < 900 && g_srv_h < 0; i++) drive(*p_c1, *p_srv);   // ≈ 握手重传的一次周期 ✓
    for (i = 0; i < 20; i++) drive(*p_c1, *p_srv);                   // 让 SYN-ACK/ACK 收尾 ✓
    if (g_srv_h < 0) return -1;
    return 0;
}

int main(void)
{
    void *p_srv = NULL, *p_c1 = NULL;
    struct app app_srv, app_c1;

    // ── A：延迟读 + 97 字节分片读 ─────────────────────────────────────────
    printf("── A 延迟读 + 分片读（97 B 一次）：顺序与内容都要对，读空后槽要还回去\n");
    memset(&app_srv, 0, sizeof(app_srv)); memset(&app_c1, 0, sizeof(app_c1));
    g_pattern = 1;
    if (make_pair(&p_srv, &p_c1, &app_srv, &app_c1) != 0) { printf("  建/握手失败 ✗\n"); return 1; }
    {   FFRDPCONTEXT *srv = (FFRDPCONTEXT*)p_srv;
        FFRDPCONN    *sc;
        uint8_t       sbuf[8 * MSS], rbuf[97];
        long          sent = 0, rd = 0, rdbad = 0;
        int           i, live_after_send = 0;

        for (i = 0; i < (int)sizeof(sbuf); i++) sbuf[i] = fillb(i);

        // A1：把 8 KB 推出去，**应用先不读**（帧都留在环里 ✓）
        for (i = 0; i < 400 && sent < (long)sizeof(sbuf); i++) {
            int n = ffrdp_send(p_c1, 0, (char*)sbuf + sent, (int)sizeof(sbuf) - (int)sent, 0);
            if (n > 0) sent += n;
            drive(p_c1, p_srv);
        }
        for (i = 0; i < 20; i++) drive(p_c1, p_srv);   // 让 8 帧都到对面（网络有 4 ms 延迟 ✓）
        sc = ffrdp_conn_lookup(srv, g_srv_h);
        if (sc) live_after_send = (int)sc->rcv_live;
        printf("     （客户端交出 %ld / %d B，服务端环里活帧 %d）\n", sent, (int)sizeof(sbuf), live_after_send);
        check(sent == (long)sizeof(sbuf), "客户端把 8 KB 全交出去了（窗口够 ✓）");
        check(live_after_send == 8, "应用没读之前 8 帧都还占着槽（延迟读确实留在环里 ✓）");

        // A2：97 字节一次地读，逐字节对图案（顺序错一位就会红 ✓）
        for (i = 0; i < 3000; i++) {
            int n;
            while ((n = ffrdp_recv(p_srv, g_srv_h, (char*)rbuf, (int)sizeof(rbuf), 0)) > 0) {
                int k;
                for (k = 0; k < n; k++) if (rbuf[k] != fillb(rd + k)) rdbad++;
                rd += n;
            }
            sc = ffrdp_conn_lookup(srv, g_srv_h);
            if (rd >= sent && app_srv.got >= sent && sc && sc->rcv_live == 0) break;
            drive(p_c1, p_srv);
        }
        check(rd == sent, "分片读把 8 KB 一字节不差地取出来了");
        check(rdbad == 0, "取出来的字节**按顺序**正确（图案逐字节对 ✓）");
        check(app_srv.bad == 0, "RECVDATA 回调看到的那份也按序正确（NOT_CONSUMED 不改变内容 ✓）");
        sc = ffrdp_conn_lookup(srv, g_srv_h);
        check(sc && sc->rcv_live == 0, "读空之后接收环的活帧归零（槽全还回去了 ✓）");
        check(sc && seq_distance(sc->rcv_read_seq, sc->recv_seq) == 0, "读游标追上了交付点 ✓");
    }
    ffrdp_exit(p_c1); ffrdp_exit(p_srv);

    // ── B：回调 CONSUMED 与 ffrdp_recv 混着来 ──────────────────────────────
    printf("── B 一半帧在回调里取走、一半留给 ffrdp_recv：两条路加起来刚好是发出去的量\n");
    memset(&app_srv, 0, sizeof(app_srv)); memset(&app_c1, 0, sizeof(app_c1));
    app_srv.consume_every = 2;   // 每第 2 次交付在回调里取走 ⇒ READ 记号夹在"要读的帧"中间 ✓
    g_pattern = 0;               // 两条路各自不连续 ⇒ 只对总量、不按偏移判序 ✓
    if (make_pair(&p_srv, &p_c1, &app_srv, &app_c1) != 0) { printf("  建/握手失败 ✗\n"); return 1; }
    {   uint8_t sbuf[8 * MSS], rbuf[512];
        long    sent2 = 0, rd2 = 0, rdbad2 = 0;
        int     i, k, guard = 0;

        memset(sbuf, 'A', sizeof(sbuf));
        for (i = 0; i < 3000; i++) {
            int n;
            if (sent2 < (long)sizeof(sbuf)) {
                n = ffrdp_send(p_c1, 0, (char*)sbuf + sent2, (int)sizeof(sbuf) - (int)sent2, 0);
                if (n > 0) sent2 += n;
            }
            drive(p_c1, p_srv);
            while ((n = ffrdp_recv(p_srv, g_srv_h, (char*)rbuf, (int)sizeof(rbuf), 0)) > 0) {
                for (k = 0; k < n; k++) if (rbuf[k] != 'A') rdbad2++;
                rd2 += n;
            }
            if (sent2 == (long)sizeof(sbuf) && rd2 + app_srv.got >= sent2 && app_srv.n > 4) {
                if (++guard > 50) break;   // 再多跑几圈，让尾巴也走完 ✓
            }
        }
        printf("     （发出 %ld B；回调取走 %ld B / %d 次；ffrdp_recv 取到 %ld B）\n",
               sent2, app_srv.got, app_srv.n, rd2);
        check(rd2 > 0 && app_srv.got > 0, "两条路都真的走了（不是只有一条在干活 ✓）");
        check(rd2 + app_srv.got == sent2, "两条路加起来**刚好**等于发出的字节（没漏、没重 ✓）");
        check(rdbad2 == 0 && app_srv.bad == 0, "两条路的字节都正确 ✓");
        {   FFRDPCONN *sc2 = ffrdp_conn_lookup((FFRDPCONTEXT*)p_srv, g_srv_h);
            check(sc2 && sc2->rcv_live == 0, "全部取完之后环里没有活帧（READ 帧也被跳过并还槽 ✓）");
        }
    }
    ffrdp_exit(p_c1); ffrdp_exit(p_srv);

    // ── C：完全不读 ⇒ 窗口归零 ⇒ 应用一读，窗口**立刻**重开 ────────────────
    printf("── C 应用完全不读：环被未读帧占满、通告窗归零；读开后窗口立刻重开\n");
    memset(&app_srv, 0, sizeof(app_srv)); memset(&app_c1, 0, sizeof(app_c1));
    g_pattern = 1;
    if (make_pair(&p_srv, &p_c1, &app_srv, &app_c1) != 0) { printf("  建/握手失败 ✗\n"); return 1; }
    {   FFRDPCONTEXT *srv = (FFRDPCONTEXT*)p_srv;
        FFRDPCONN    *sc;
        uint8_t       sbuf[4 * MSS], rbuf[4096];
        long          sent3 = 0, rd3 = 0, rdbad3 = 0;
        int           i, k, stalled_at = -1, resume_steps = -1;

        for (i = 0; i < (int)sizeof(sbuf); i++) sbuf[i] = fillb(i);
        // C1：一路发、一路不读 ⇒ 环被填满（通告窗 = 环槽数 - rcv_live 掉到 0）
        for (i = 0; i < 6000; i++) {
            if (ffrdp_send(p_c1, 0, (char*)sbuf, (int)sizeof(sbuf), 0) > 0) sent3 += (long)sizeof(sbuf);
            drive(p_c1, p_srv);
            sc = ffrdp_conn_lookup(srv, g_srv_h);
            if (sc && sc->rcv_live >= FFRDP_RCV_FRAME_RING) { stalled_at = i; break; }
        }
        sc = ffrdp_conn_lookup(srv, g_srv_h);
        check(sc != NULL, "服务端连接还在 ✓");
        if (sc) {
            printf("     （第 %d 步停下：环里活帧 %u / %d，rcv_read_seq %u vs recv_seq %u）\n",
                   stalled_at, sc->rcv_live, FFRDP_RCV_FRAME_RING, sc->rcv_read_seq, sc->recv_seq);
            check(sc->rcv_live == FFRDP_RCV_FRAME_RING, "环被未读帧占满（应用不读 ⇒ 应收的全在环里 ✓）");
            for (k = 0; k < 3; k++) drive(p_c1, p_srv);   // 让"窗口 0"那条 ack 真的发出去 ✓
            check(sc->rx_wnd_zero == 1, "最后一次通告的窗口是 0（对端被我们挡住了 ✓）");
        }
        // C2：应用开始读 ⇒ 窗口重开、**对端马上就能再送进来**（不必等它的 RTO ✓）
        // 判据必须看"真收到新数据"，不能只看 ffrdp_send 是否被受理 —— 后者只是把帧灌进发送环，
        // 窗口为 0 时它也照样受理（挡在 tick 里 ✓）。
        for (i = 0; i < 3000; i++) {
            int n;
            while ((n = ffrdp_recv(p_srv, g_srv_h, (char*)rbuf, (int)sizeof(rbuf), 0)) > 0) {
                for (k = 0; k < n; k++) if (rbuf[k] != fillb(rd3 + k)) rdbad3++;
                rd3 += n;
            }
            if (ffrdp_send(p_c1, 0, (char*)sbuf, (int)sizeof(sbuf), 0) > 0) sent3 += (long)sizeof(sbuf);
            drive(p_c1, p_srv);
            sc = ffrdp_conn_lookup(srv, g_srv_h);
            // 判据：recv_seq 越过"停住时那一轮环容量" = 真的收到了新数据（数字跟着环大小走 ✓）
            if (resume_steps < 0 && sc && seq_distance(sc->recv_seq, FFRDP_RCV_FRAME_RING) > 0) resume_steps = i;
            if (resume_steps >= 0 && sc && sc->rcv_live == 0) break;
        }
        printf("     （读开之后第 %d 步对端就送进了新数据；共读回 %ld B，坏字节 %ld，发出 %ld B）\n",
               resume_steps, rd3, rdbad3, sent3);
        check(resume_steps >= 0 && resume_steps < 60, "读开之后新数据**在 60 步内**就到了（不等 RTO ✓）");
        check(rd3 > 0 && rdbad3 == 0, "积压的帧读回来逐字节正确（256 帧的积压也没错位 ✓）");
    }
    ffrdp_exit(p_c1); ffrdp_exit(p_srv);

    printf("       verdict -> %s\n", g_fail ? "FAIL" : "OK");
    return g_fail ? 1 : 0;
}
