// 尾巴丢失扫参：A′（尾部探针）的收益随 RTT / 静默时长怎么变 —— 只量**能归因的**那一段。
// 用法: ./t_sweep_new <delay> <silence_steps> [silence_loss]
//   静默期按 silence_loss（默认 1.0 = 全丢）走，之后恢复 0 丢包，数"放开后多少步第一次收到数据"。
//
// 为什么不用 e2e 判这一条：e2e 的丢包是均匀随机、按包序号哈希 ⇒ 探针多发一个字节就把后面
// 整套丢包实现换掉（实测 1–12 个探针能带来 ±75% 的吞吐差），方向有正有负、不可归因。
// 这里两侧只差 A′ 这一个机制、链路事件完全相同 ⇒ 差值就是它。
//
// ⚠️ 要真的两侧不同：A′ 已经进了 HEAD ⇒ base 侧必须用 REF=<加 A′ 之前的 ffrdp.c> 重编
//    （REF=/tmp/x.c TESTS=t_sweep bash run.sh），否则 base 与 work 逐字相同 —— 差 0 是
//    "没有对照"，不是"没有收益"。参考数值见提交信息：12 个组合全部非负，29–47% 恢复时间。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../ffrdp.h"
#include "simdefs.h"
#include "../../ffrdp.c"

#define MKADDR(port) ((uint64_t)(0x7F000001ULL << 16) | (uint64_t)(port))
#define A_SRV MKADDR(8001)
#define A_C1  MKADDR(40001)

#ifdef HAVE_TAILPROBE
static uint32_t probe_cnt(FFRDPCONN *c) { return c->counter_tailprobe; }
static uint32_t probe_lost(FFRDPCONN *c) { return c->counter_tailprobe_lost; }
#else
static uint32_t probe_cnt(FFRDPCONN *c) { (void)c; return 0; }
static uint32_t probe_lost(FFRDPCONN *c) { (void)c; return 0; }
#endif

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

int main(int argc, char **argv)
{
    FFRDPCONTEXT *c1;
    FFRDPCONN    *cc;
    int delay = argc > 1 ? atoi(argv[1]) : 4;
    int sil   = argc > 2 ? atoi(argv[2]) : 400;
    double sl = argc > 3 ? atof(argv[3]) : 1.0;
    uint32_t rto0;
    int i, steps = 0;

    memset(sbuf, 'P', sizeof(sbuf));
    sim_reset(); sim_config(0.0, 0, delay, 0);
    g_accept = 0; g_srv_h = -1; g_step = 0;
    p_srv = ffrdp_init("ffrdps://127.0.0.1:8001,mss:1024,fec:0", on_event, NULL);
    p_c1  = ffrdp_init("ffrdpc://127.0.0.1:8001,mss:1024,fec:0", on_event, NULL);
    c1 = (FFRDPCONTEXT*)p_c1; cc = c1->table[0];
    sim_ll_own(((FFRDPCONTEXT*)p_srv)->lowlevel, A_SRV); sim_ll_own(c1->lowlevel, A_C1);
    sim_ll_route(((FFRDPCONTEXT*)p_srv)->lowlevel, A_C1, 1); sim_ll_route(c1->lowlevel, A_SRV, 0);

    for (i = 0; i < 4000 && !up(); i++) pump(1, 0);
    if (!up()) { printf("HS-FAIL\n"); return 1; }
    pump(600, 1);                       // 健康期：建立 rtts / 窗口

    sim_config(sl, 0, delay, 0);
    pump(sil, 1);                       // 静默期
    rto0 = cc->rto;

    sim_config(0.0, 0, delay, 0);
    for (i = 0; i < 8000; i++) { steps++; if (pump(1, 1) > 0) break; }
    printf("delay=%-3d silence=%-4d rto=%-4u rtts=%-3u recover=%-4d probe=%-3u lost=%-3u rto_tx=%u\n",
           delay, sil, rto0, cc->rtts, steps, probe_cnt(cc), probe_lost(cc), cc->counter_resend_rto);
    ffrdp_exit(p_c1); ffrdp_exit(p_srv);
    return 0;
}
