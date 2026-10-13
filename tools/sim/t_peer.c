// 多对端路由自测（P0）—— 只测夹具，不测协议。
//
// 为什么需要它：v2 的服务器要用"一个 lowlevel + 一张 conn 表"服务 N 个对端，第一件事就是
// "按对端地址把帧分发到正确的连接"。真协议还没走到那一步，所以先把夹具验好：
//
//   slot 0 = 服务端，slot 1 / 2 = 两个对端
//   ① 对端发的包带自己的地址 ⇒ 服务端侧读出来必须是那个地址（分发的前提）
//   ② 服务端按地址回包 ⇒ 必须落到对应那个对端的 slot（不能串台）
//   ③ 未登记地址的 slot 仍走旧路径（源地址报 0）⇒ 老的两端测试行为不变
//
// 它不包含 ffrdp.c：测的就是 simnet.c + 假 lowlevel（simll.c）这一层夹具本身。
#include <stdio.h>
#include <string.h>
#include "simdefs.h"
#include "../../lowlevel.h"

#define A_SRV   0x0A00000100005001ULL
#define A_PEER1 0x0A00000100001001ULL
#define A_PEER2 0x0A00000100001002ULL

static int g_fail;
static void check(int ok, const char *what) { if (!ok) { printf("       FAIL: %s\n", what); g_fail++; } }

static FFRDP_LOWLEVEL *g_fl;
static int      g_nrecv;
static uint64_t g_rxaddr[4];
static int      g_rxv[4];

// 桩回调：每收到一帧，记录"谁发的、内容是什么"，然后**按地址**原样回一个包。
// 回包这一步就是在验"ll->send(addr) 能不能按地址回到正确的对端"。
static long stub_cb(void *cbctx, int type, void *buf, int len)
{
    (void)cbctx;
    if (type == FFRDP_LOWLEVEL_TYPE_RECV) {
        FFRDP_LOWLEVEL_MSG *m = (FFRDP_LOWLEVEL_MSG*)buf;
        if (g_nrecv < 4) {
            g_rxaddr[g_nrecv] = m->addr;
            g_rxv[g_nrecv]    = m->len > 0 ? ((unsigned char*)m->buf)[0] : -1;
            g_nrecv++;
        }
        if (m->addr && m->len > 0) g_fl->send(g_fl, m->addr, (uint8_t*)m->buf, m->len);
    }
    return 0;
}

int main(void)
{
    static int  marker;          // 非 NULL 的 cbctx：sim_ll_run() 靠它把实例找回来
    char        sbuf[8], rbuf[16];
    int         n;

    sim_reset();
    sim_slot(0, A_SRV);          // 0 = 服务端，1/2 = 两个对端
    sim_slot(1, A_PEER1);
    sim_slot(2, A_PEER2);

    g_fl = sim_ll_init((void*)"none", stub_cb, &marker);
    check(g_fl != NULL, "sim_ll_init returned NULL");
    if (!g_fl) { printf("       verdict -> FAIL\n"); return 0; }
    sim_ll_own(g_fl, A_SRV);
    check(sim_ll_route(g_fl, A_PEER1, 1) == 0, "route to peer1 failed");
    check(sim_ll_route(g_fl, A_PEER2, 2) == 0, "route to peer2 failed");

    // ① 两个对端各发一帧给服务端（slot 0）：包里带的是各自的地址。
    //    这里用显式版本 —— 旧路径 sim_send() 是"发到 1-cursid"，slot 2 会算出 -1 ✗，
    //    它只对"一对端"的老测试有意义。
    sim_begin(1); sbuf[0] = '1'; check(sim_send_from(0, A_PEER1, sbuf, 1) == 1, "peer1 send");
    sim_begin(2); sbuf[0] = '2'; check(sim_send_from(0, A_PEER2, sbuf, 1) == 1, "peer2 send");
    // 顺带钉住护栏：旧路径从 slot 2 发不会崩、也不会串台（直接丢弃 ✓）
    sim_begin(2); check(sim_send(sbuf, 1) == 1, "legacy sim_send from slot 2 must not crash");

    // 服务端跑一步：应当收到两帧，且来源地址分别是两个对端（这就是 conn 查表的输入）
    sim_begin(0); sim_ll_run(0, &marker);
    check(g_nrecv == 2, "server should see exactly two frames");
    check(g_nrecv > 0 && g_rxaddr[0] == A_PEER1 && g_rxv[0] == '1', "frame 1 is not peer1's");
    check(g_nrecv > 1 && g_rxaddr[1] == A_PEER2 && g_rxv[1] == '2', "frame 2 is not peer2's");

    // ② 回包按地址路由：peer1 只能收到自己那一帧，peer2 同理（不能串台）
    sim_begin(1); n = sim_recv(rbuf, sizeof(rbuf));
    check(n == 1 && rbuf[0] == '1', "peer1 did not get exactly its own echo");
    check(sim_recv_src() == A_SRV, "peer1 saw the wrong source addr");
    sim_begin(2); n = sim_recv(rbuf, sizeof(rbuf));
    check(n == 1 && rbuf[0] == '2', "peer2 did not get exactly its own echo");
    check(sim_recv_src() == A_SRV, "peer2 saw the wrong source addr");

    // ③ 旧路径：不登记地址时，行为与以前一样（0 发 1 收），且源地址报 0 ⇒ 上层用老常量
    sim_reset();
    sim_begin(0); sim_send(sbuf, 1);
    sim_begin(1); n = sim_recv(rbuf, sizeof(rbuf));
    check(n == 1, "legacy path: the peer did not receive the frame");
    check(sim_recv_src() == 0, "legacy path: source addr should stay 0");

    g_fl->exit(g_fl);
    printf("       t_peer: multi-peer routing %s\n", g_fail ? "FAIL" : "OK");
    printf("       verdict -> %s\n", g_fail ? "FAIL" : "OK");   // 让 run.sh 的判定汇总读得到
    return 0;
}
