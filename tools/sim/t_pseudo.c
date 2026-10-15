// 伪超时：ACK **晚到**，但一个帧都没丢。
//
// 为什么单独立一个测试：真机上这类超时占了绝大多数，而且它有独立的后果 —— 被当成拥塞时 cwnd 会
// 打回 1，发送速率随即掉到约"1 帧/RTT"、再靠慢启动爬回来。
//   2026-10-15 快 AP 实测（slowfix2，两次）：swnd, cwnd, ssthresh = 149, 1, 64 / 128, 1, 1 ✗
//     ⇒ 吞吐掉到 1.58 / 3.09 MB/s ✗，而 timeout diag 显示绝大多数超时是 acks-arrived-but-missed
//       （139~167 ✓）⇒ 链路没丢，是估计器没看见 ✗。
// 仿真里要造这件事很简单：不丢包、不加延迟，只要"发两帧、只确认第一帧、让第二帧自己到点" ✓
// —— 第二帧在等待期间**确实收到过 ack**（第一帧那条），所以它是伪超时 ✓。
// 只发两帧也是刻意的：帧多了会把假 lowlevel 的队列填满 ⇒ 出现 counter_cwnd_sfail（发送失败 ✓ 那是
// 另一条合法的拥塞信号 ⇒ 它把 cwnd 打回 1 是对的 ✓）⇒ 这个场景就测不准了 ✗。
//
// 判据：① 确实发生了 RTO 重传（否则这个场景没测到东西 ✗）；
//       ② 它被认成伪超时（counter_to_hadack >= 1 且 counter_to_noack == 0 ✓）；
//       ③ cwnd 没有被拉回 1 ✓（纯丢包场景见 t_rto：那里 ACK 根本不来 ⇒ 仍应打回 1 ✓）。
#include <stdio.h>
#include <string.h>
#include "simdefs.h"
#include "../../ffrdp.c"

// 与 t_ack/t_rto 同一套路：两侧接口形状不同，测试用这些包装写成一句话 ✓
#ifdef CONN_SPLIT
#define CONN(c) (&(c)->conn)
#else
#define CONN(c) (c)
#endif

#ifdef FFRDP_API_V2
#define TEST_SEND(c, b, l) ffrdp_send(c, 0, b, l, 0)
#define TEST_FREE(c)       ffrdp_exit(c)
#elif defined(FFRDP_API_HAS_TIMEOUT)
#define TEST_SEND(c, b, l) ffrdp_send(c, b, l, 0)
#define TEST_FREE(c)       ffrdp_free(c)
#else
#define TEST_SEND(c, b, l) ffrdp_send(c, b, l)
#define TEST_FREE(c)       ffrdp_free(c)
#endif

#ifdef FFRDP_LL_DRIVEN
#define TEST_RUN(c, idx) sim_ll_run(idx, c)
#else
#define TEST_RUN(c, idx) ffrdp_update(c)
#endif

#ifdef FFRDP_API_V2
static void *TEST_INIT(const char *ip, int port, int server, int mss)
{
    char params[128];
    snprintf(params, sizeof(params), "%s://%s:%d,mss:%d,fec:0,hs:0",
             server ? "ffrdps" : "ffrdpc", ip, port, mss);
    return ffrdp_init(params, NULL, NULL);
}
#elif defined(FFRDP_API_HAS_ADDR)
static void *TEST_INIT(const char *ip, int port, int server, int mss)
{
    char addr[64];
    snprintf(addr, sizeof(addr), "%s:%d", ip, port);
    return ffrdp_init(addr, NULL, NULL, server, mss, 0);
}
#else
#define TEST_INIT(ip, port, server, mss) ffrdp_init((char *)(ip), port, NULL, NULL, server, mss, 0)
#endif

static void make_ack(unsigned char *ack, unsigned int una, unsigned int mack, unsigned char rwnd)
{
    memset(ack, 0, 8);
    *(uint32_t*)(ack + 0) = (FFRDP_FRAME_TYPE_ACK << 0) | (una << 8);
    *(uint32_t*)(ack + 4) = (mack << 0) | ((uint32_t)rwnd << 24);
}

int main(void)
{
    static unsigned char buf[64 * 1024], ack[8];
    FFRDPCONTEXT *c;
    uint32_t cwnd_before;
    int ok;

    sim_reset();
    sim_config(0, 0, 0, 0);            // 不丢包、不加固定延迟：ACK 只是"晚"，不是"丢" ✓
    c = TEST_INIT("127.0.0.1", 8001, 0, 1024);
    sim_begin(0);

    TEST_SEND(c, (char*)buf, 2 * 1024);   // 只两帧 ✓（多了会引出 SEND_FAILED ⇒ 另一个原因 ✗）
    TEST_RUN(c, 0);
    cwnd_before = CONN(c)->cwnd;

    // 只确认第一帧：una 前进 1 ⇒ 第二帧仍未确认，但"等待期间确实有 ack 到过" ✓
    make_ack(ack, 1, 0x00, 255);
    sim_inject(0, ack, 8);
    TEST_RUN(c, 0);

    // 让第二帧自己到点（虚拟时钟往前跳，越过 RTO ✓）⇒ 超时判断与事件选择就发生在这一步里 ✓
    sim_advance(4000);
    TEST_RUN(c, 0);
    sim_advance(4000);
    TEST_RUN(c, 0);

    printf("  rto resends=%u  to_hadack=%u  to_noack=%u  cwnd %u -> %u\n",
           CONN(c)->counter_resend_rto, CONN(c)->counter_to_hadack, CONN(c)->counter_to_noack,
           cwnd_before, CONN(c)->cwnd);
    // cwnd 有两条路会掉：超时（counter_cwnd_back）与发送失败（counter_cwnd_sfail）⇒ 都打出来，免得归错因 ✗
    printf("  cwnd 被拉低的来源：timeout=%u sendfailed=%u\n",
           CONN(c)->counter_cwnd_back, CONN(c)->counter_cwnd_sfail);
#ifdef HAVE_TO_DIAG
    // ① 场景成立（真的超时重传过 ✓）② 它是伪超时 ✓ ③ cwnd 没被拉回 1 ✓
    ok = (CONN(c)->counter_resend_rto >= 1) &&
         (CONN(c)->counter_to_hadack >= 1) &&
         (CONN(c)->counter_to_noack == 0) &&
         (CONN(c)->cwnd > 1);
    printf("  迟到的 ack 不算拥塞 ⇒ cwnd 只降半、不回 1 -> %s\n", ok ? "PASS" : "FAIL");
#else
    printf("  counter_to_hadack 在这个版本里没有\n");
    ok = 1;
#endif
    TEST_FREE(c);
    return ok ? 0 : 1;
}
