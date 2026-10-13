// FEC（v2）—— 冗余帧到底能不能把丢掉的帧补回来 ✓
//
// 为什么单独一个测试：`fec` 是协议里唯一**从没被真跑过**的机制 ✗ ——
// e2e / 各单测（连真机 test.c 也是）都写死 `fec:0` ✓，于是「xor 冗余 + 恢复」这段代码
// 一直只靠读 ✓、没有判据 ✓。
//
// 三段（都走 `hs:0` ✓ —— 握手期不发数据 ✓；`mss` 取 4 的倍数 ✓：异或只覆盖
// `(4+smss)/4` 个字 ✓，非 4 对齐的尾部本来就不受保护 ✓）：
//
//   A `fec:3`（每组 2 数据 + 1 冗余）丢 1 个数据帧 ⇒ **恢复** ✓：
//     `counter_fec_ok = 1` ✓，而且应用侧读到的字节**逐字节正确** ✓
//     （恢复出的帧会进重排队列 ⇒ 顺序也不乱 ✓）。
//   B `fec:4`（每组 3 数据 + 1 冗余）丢 2 个数据帧 ⇒ **恢复不了** ✓：
//     `counter_fec_failed = 1` ✓（冗余到了、但缺得不是恰好 1 个 ⇒ 放弃 ✓）。
//   C `fec:2`（每组 1 数据 + 1 冗余）丢那 1 个数据帧 ⇒ ⚠️ **现在也恢复不了** ✗：
//     冗余帧到达时走的是「组变」分支（被当成新组的基底 ✓）⇒ 既不计数也不恢复 ✓。
//     钉的是**现状** ✓ —— 数学上 fec:2 是能恢复的（那时 rxcnt 正好 = rdc-2 ✓），
//     是「组变」那段把冗余帧也记进了 rxcnt ✗。这条判据的价值就在于把它变成
//     「已知行为」而不是「没人知道」✓。
//
// ⚠️ v2-only ✓。
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

static int      g_step;
static void    *g_cli;              // phase() 里记下客户端（要读它的重传计数 ✓）
static uint8_t  g_buf[8 * MSS];     // 发送内容（可判的图案 ✓）
static uint8_t  g_rx[8 * MSS];      // 收侧累积
static char     g_drop[4096];

// 只驱动一侧（另一侧不动 ⇒ 包留在它那个 slot 的队列里 ✓）
static void drive_one(int side, void *ctxt, int steps)
{
    int i;
    for (i = 0; i < steps; i++) { sim_setstep(g_step++); sim_begin(side); sim_ll_run(side, ctxt); }
}

// 双侧一起驱动（ARQ 兜底那一段要用 ✓：让重传、ack 都跑起来 ✓）
static void drive_both(void *p_c1, void *p_srv, int steps)
{
    int i;
    for (i = 0; i < steps; i++) {
        sim_setstep(g_step++);
        sim_begin(1); sim_ll_run(1, p_c1);
        sim_begin(0); sim_ll_run(0, p_srv);
    }
}

// 收侧把应用层能读到的东西全读出来（不等待 ✓）
static int drain_rx(void *ctxt, int handle)
{
    int total = 0, n;
    for (;;) {
        n = ffrdp_recv(ctxt, handle, (char*)g_rx + total, (int)sizeof(g_rx) - total, 0);
        if (n <= 0) break;
        total += n;
    }
    return total;
}

// 一段的公共骨架：建一对（都 hs:0 + 给定 fec ✓）、发 len 字节、精确丢掉前 drop_n 个包、
// 再让服务端处理剩下的 ✓；返回服务端的上下文（失败返回 NULL）
static FFRDPCONTEXT *phase(const char *tag, int fec, int len, int drop_n,
                           int *out_queued_after_send, int *out_rx, int *out_bad)
{
    void *p_srv, *p_c1;
    FFRDPCONTEXT *srv;
    char params[160];
    int  i, n, total = 0, bad = 0;

    printf("── %s\n", tag);
    sim_reset(); sim_config(0.0, 0, 0, 0);
    g_step = 0;
    memset(g_rx, 0, sizeof(g_rx));
    for (i = 0; i < (int)sizeof(g_buf); i++) g_buf[i] = (uint8_t)((i * 7 + 3) & 0xFF);

    snprintf(params, sizeof(params), "ffrdps://127.0.0.1:8001,mss:%d,fec:%d,hs:0", MSS, fec);
    p_srv = ffrdp_init(params, NULL, NULL);
    snprintf(params, sizeof(params), "ffrdpc://127.0.0.1:8001,mss:%d,fec:%d,hs:0", MSS, fec);
    p_c1  = ffrdp_init(params, NULL, NULL);
    if (!p_srv || !p_c1) { check(0, "两个上下文都建起来了"); if (p_srv) ffrdp_exit(p_srv); if (p_c1) ffrdp_exit(p_c1); return NULL; }
    srv = (FFRDPCONTEXT*)p_srv;
    g_cli = p_c1;
    sim_ll_own(srv->lowlevel, A_SRV); sim_ll_own(((FFRDPCONTEXT*)p_c1)->lowlevel, A_C1);
    sim_ll_route(srv->lowlevel, A_C1, 1); sim_ll_route(((FFRDPCONTEXT*)p_c1)->lowlevel, A_SRV, 0);

    n = ffrdp_send(p_c1, 0, (char*)g_buf, len, 0);
    check(n == len, "客户端把整条消息交给协议（原子 ✓）");
    drive_one(1, p_c1, 8);                                   // 只驱动客户端 ⇒ 帧全落到服务端的 slot 0
    *out_queued_after_send = sim_queued(0);
    printf("     （客户端发出后，服务端队列里有 %d 个包 ✓）\n", *out_queued_after_send);

    for (i = 0; i < drop_n; i++) {                           // 精确丢包：从队头读走并丢掉 ✓
        sim_begin(0);
        n = sim_recv(g_drop, (int)sizeof(g_drop));
        if (n <= 0) { check(0, "精确丢包：队头可取 ✓"); break; }
    }
    check(sim_queued(0) == *out_queued_after_send - drop_n, "丢包后队列数对得上 ✓");

    drive_one(0, p_srv, 8);                                  // 让服务端处理剩下的帧
    total = drain_rx(p_srv, 0);
    *out_rx = total;
    for (i = 0; i < total; i++) if (g_rx[i] != g_buf[i]) bad++;
    *out_bad = bad;
    printf("     （应用侧读到 %d 字节；与发送内容不符 %d 字节；fec_ok=%u fec_failed=%u fec_rx=%u）\n",
           total, bad, srv->conn.counter_fec_ok, srv->conn.counter_fec_failed,
           srv->conn.counter_fec_rx);
    return srv;
}

int main(void)
{
    FFRDPCONTEXT *srv;
    int q = 0, rx = 0, bad = 0, i;

    // ── A：fec:3 + 丢 1 个数据帧 ⇒ 应当被恢复 ✓
    srv = phase("A fec:3（每组 2 数据 + 1 冗余），丢 1 个数据帧", 3, 2 * MSS, 1, &q, &rx, &bad);
    if (srv) {
        check(q == 3, "队列里正好 3 个（2 数据 + 1 冗余 ✓）");
        check(srv->conn.counter_fec_rx == 2, "服务端只收到 2 个 fec 帧（丢的那个确实没到 ✓）");
        check(srv->conn.counter_fec_ok == 1, "**冗余帧把丢掉的帧恢复了**（counter_fec_ok = 1 ✓）");
        check(srv->conn.counter_fec_failed == 0, "没有失败计数 ✓");
        check(rx == 2 * MSS, "应用侧读到完整的 2048 字节（丢的那一帧被补回来了 ✓）");
        check(bad == 0, "逐字节与发送内容一致 ⇒ 恢复出的就是原来那帧、顺序也没乱 ✓");
        check(((FFRDPCONTEXT*)g_cli)->conn.counter_resend_rto == 0,
              "整段**一次重传都没有** ⇒ 那份数据只可能是 FEC 补回来的（不是 ARQ 干的 ✗）✓");
        ffrdp_exit(srv);
    }

    // ── B：fec:4 + 丢 2 个数据帧 ⇒ 恢复不了，要计 failed ✓
    srv = phase("B fec:4（每组 3 数据 + 1 冗余），丢 2 个数据帧", 4, 3 * MSS, 2, &q, &rx, &bad);
    if (srv) {
        check(q == 4, "队列里正好 4 个（3 数据 + 1 冗余 ✓）");
        check(srv->conn.counter_fec_ok == 0, "恢复不了 ⇒ counter_fec_ok = 0 ✓");
        check(srv->conn.counter_fec_failed == 1, "**缺得不是恰好 1 个 ⇒ counter_fec_failed = 1 ✓**（不静默 ✓）");
        check(rx == 0, "应用侧**一个字节都拿不到** ✓：第 3 份虽然过了 FEC 层，但它前面有空洞 ⇒\n              只能等在重排队列里 ✓（有序交付 ✓ —— 绝不把第 3 份当成第 1 份交出去 ✓）");
        // 再跑一段：FEC 没补上时，**ARQ 必须兜底**（否则数据就真丢了 ✗）
        drive_both(g_cli, srv, 2000);
        rx = drain_rx(srv, 0); bad = 0;
        for (i = 0; i < rx; i++) if (g_rx[i] != g_buf[i]) bad++;
        printf("     （ARQ 兜底：resend_rto=%u resend_fast=%u ✓）\n",
               ((FFRDPCONTEXT*)g_cli)->conn.counter_resend_rto,
               ((FFRDPCONTEXT*)g_cli)->conn.counter_resend_fast);
        check(((FFRDPCONTEXT*)g_cli)->conn.counter_resend_rto > 0 ||
              ((FFRDPCONTEXT*)g_cli)->conn.counter_resend_fast > 0,
              "客户端重传过（RTO 或快速重传 ✓ —— 这就是「ARQ 兜底」的证据 ✓）");
        check(rx == 3 * MSS && bad == 0, "ARQ 最终把三份都补齐了、逐字节正确 ✓（FEC 不管的，重传管 ✓）");
        ffrdp_exit(srv);
    }

    // ── C：fec:2 的现状（已知缺陷，钉住 ✓）
    srv = phase("C fec:2（每组 1 数据 + 1 冗余），丢那 1 个数据帧", 2, MSS, 1, &q, &rx, &bad);
    if (srv) {
        check(q == 2, "队列里 2 个（1 数据 + 1 冗余 ✓）");
        check(srv->conn.counter_fec_rx == 1, "服务端只收到那个冗余帧 ✓");
        check(srv->conn.counter_fec_ok == 0 && srv->conn.counter_fec_failed == 0,
              "⚠️ 既没恢复、也没计失败 ⇒ **fec:2 实际上等于没有冗余** ✗（现状，见文件头说明 ✓）");
        check(rx == 0, "此刻应用侧一个字节都拿不到（冗余确实没起作用 ✓）");
        drive_both(g_cli, srv, 2000);
        rx = drain_rx(srv, 0); bad = 0;
        for (i = 0; i < rx; i++) if (g_rx[i] != g_buf[i]) bad++;
        check(rx == MSS && bad == 0, "但 **ARQ 兜住了** ⇒ 数据最终完好送达 ✓（fec:2 只是白搭冗余，不是丢数据 ✗）");
        ffrdp_exit(srv);
    }

    printf("       verdict -> %s\n", g_fail ? "FAIL" : "OK");
    return g_fail ? 1 : 0;
}
