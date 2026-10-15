// t_cwnd: 塌陷之后的恢复代价 —— "ssthresh 被已经塌掉的 cwnd 重算"到底值不值 ✓
//
// 被测的那一刀在 ffrdp_congestion_control(CEVENT_ACK_TIMEOUT) 里：
//     ssthresh = MAX(cwnd / 2, FFRDP_MIN_CWND_SIZE);   // cwnd 已经塌在 1 时 ⇒ 算出来是 1 ✗
//     cwnd     = FFRDP_MIN_CWND_SIZE;
// 而爬升规则是 `cwnd < ssthresh ? cwnd *= 2 : cwnd++` ⇒ **ssthresh=1 意味着慢启动那一档永远用不上**，
// 此后只剩 +1/ACK。真机 dump 里 cwnd<=2 的档位 14/277（≈5%，几乎都在 server 侧 ✓）⇒ 现象确实在
// 发生 ✓；但它"贵不贵"要看爬回去要多久 ⇒ 这个测试把那段恢复摆出来量 ✓。
//
// 场景（单侧 + 注入 ack，与 t_rto 同一套路 ✓；不丢包、不加延迟 ⇒ 过期只由第 ① 步人为造成 ✓）：
//   ① 只发 1 帧，然后跳过 100ms 一个 ack 都不给 ⇒ 这一帧过期（= 模拟丢包 ✓）
//   ② 连着两次 RTO，中间不给 ack：
//        第一次：cwnd 32 ⇒ ssthresh 16、cwnd 1（两侧一致 ✓，这是"正常一刀"）
//        第二次：cwnd 已经是 1 ⇒ **这就是被测的那一刀**（HEAD 侧 ssthresh 变 1 ✗ / 工作区保持 16 ✓）
//   ③ 之后每轮前进 5ms（< rto ⇒ 新发出去的帧不会过期 ✓）并"累计确认已发出的帧"（真实对端就是累积
//      确认 ✓），把每轮之后的 cwnd 打出来 ⇒ 恢复到 >= 64 用了几轮、多少虚拟毫秒，一眼可比 ✓
//
// 判据（本测试只做 ① 的前置测量 ⇒ 两侧都该通过 ✓）：
//   场景成立 —— 真的发生了 >= 2 次 RTO 重传、且塌陷后 cwnd == 1 ✓。
//   两侧的 cwnd 轨迹并排打出来，就是"要不要改 ssthresh"的价码 ✓（改不改由人看数决定 ✓）。
#include <stdio.h>
#include <string.h>
#include "simdefs.h"
#include "../../ffrdp.c"

// ── 结构拆分（P1）之后连接级字段挂在 ctxt->conn 下 ⇒ 两侧用同一句话 ✓ ──────────
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
    // hs:0：本测试要的是"单连接 + 裸帧"的老路径，不经过握手 ✓
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
    static unsigned char buf[64 * 1024], ack[8], drain[4096];
    FFRDPCONTEXT *c;
    uint32_t acked = 0, ssth_after, cwnd_after;
    int k, round64 = -1, ok;
    int msgs = 0;
    uint32_t ms_start, ms_at64 = 0;

    sim_reset();
    sim_config(0, 0, 0, 0);
    c = TEST_INIT("127.0.0.1", 8001, 0, 1024);
    sim_begin(0);

    // ① 1 帧（只让它一个过期 ✓），然后跳过 100ms 不给 ack ⇒ 它过期（模拟丢包 ✓）
    TEST_SEND(c, (char*)buf, 1 * 1024);
    TEST_RUN(c, 0);
    sim_advance(100);

    // ② 连着两次 RTO：第二次发生在 cwnd 已经塌到 1 的时候 ⇒ 被测的那一刀 ✓
    TEST_RUN(c, 0);
    sim_advance(100);
    TEST_RUN(c, 0);
    ssth_after = CONN(c)->ssthresh;
    cwnd_after = CONN(c)->cwnd;

    printf("  两次 RTO 之后: cwnd=%u ssthresh=%u  resend_rto=%u\n",
           cwnd_after, ssth_after, CONN(c)->counter_resend_rto);
#ifdef HAVE_SSTH_FLOOR
    printf("  ssthresh 被算到下限的次数: %u（HEAD 侧没有这个计数器）\n", CONN(c)->counter_ssth_floor);
#endif

    // ③ 逐轮恢复：每轮 = "应用继续喂数据（1KB/条 ✓）→ 前进 5ms → 跑一圈 → 累计确认已发出的帧"
    //    ⚠️ ffrdp_send 是"整条收下、或一个字节都不收"⇒ 一次交 64KB 会被应用队列整条拒掉 ✗
    //    （实测 wait_snd 一直是 0 ✓），所以按 1KB 一条、每轮交几条走 —— 真实应用就是这么持续喂的 ✓。
    ms_start = sim_tick();
    printf("  恢复（每轮 = 一个 RTT：前进 5ms ✓、对端把已发出的帧累积确认掉 ✓）: cwnd =");
    for (k = 0; k < 24; k++) {
        int j;
        for (j = 0; j < 64; j++) if (TEST_SEND(c, (char*)buf, 1 * 1024) > 0) msgs++;   // 饱和发送端 ✓
        sim_advance(5);
        TEST_RUN(c, 0);                                        // 这一轮该发的新帧发出去
        while (sim_recv(drain, (int)sizeof(drain)) > 0) { }     // 倒掉出去方向（队列满了会引出 SEND_FAILED ✗）
        acked += CONN(c)->inflight_used;                        // 已发出、未确认的帧数（tick 每圈刷新 ✓）
        make_ack(ack, acked, 0x00, 255);                        // 真实对端是累积确认 ⇒ 一次全确认 ✓
        sim_inject(0, ack, 8);
        TEST_RUN(c, 0);
        printf(" %u", CONN(c)->cwnd);
        if (round64 < 0 && CONN(c)->cwnd >= 64) { round64 = k + 1; ms_at64 = sim_tick() - ms_start; }
    }
    printf("\n  期间应用成功交给协议的消息数: %d（每条 1KB ⇒ 一帧 ✓）\n", msgs);
    if (round64 > 0)
        printf("  恢复到 cwnd>=64: 第 %d 轮，用时 %u ms（虚拟时钟 ✓）\n", round64, ms_at64);
    else
        printf("  24 轮之内没能回到 cwnd>=64 ✗\n");
    printf("  这 24 轮里的 RTO 重传总数: %u（恢复期间不该再多挨刀 ✓）\n", CONN(c)->counter_resend_rto);

    // 判据：只要求"场景成立"（① 的前置测量 ✓）—— 两侧都该通过 ✓
    ok = (CONN(c)->counter_resend_rto >= 2) && (cwnd_after == 1);
    printf("  场景成立（>=2 次 RTO 且塌到 cwnd=1）-> %s\n", ok ? "PASS" : "FAIL");
    TEST_FREE(c);
    return ok ? 0 : 1;
}
