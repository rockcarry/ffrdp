// 加密回调（v2）：载荷在**协议层**交给应用 —— 发送前叫 FFRDP_ENCRYPT、收到后叫 FFRDP_DECRYPT，
// 都是**原地改、长度不变** ⇒ 对 ffrdp_send / ffrdp_recv 完全透明（应用只看见字节流 ✓）。
//
// 本测试用最朴素的可逆字节异或当"密码"，不是为了安全，而是为了让三件事不含糊：
//   ① 发侧真被调过（enc > 0）；② 收侧真被调过（dec > 0）；
//   ③ **线上真的是密文** —— 若协议漏加密，收侧"解密"出来的就是 'A'^ks != 'A'，
//      下面逐字节比对 'A' 的检查会立刻红 ⇒ 这个检查本身就是"线上是密文"的证据 ✓。
//
// 三段：
//   A 大块发送   ⇒ 走"攒满一个 smss"那条出口 ✓
//   B 每步 1 字节 + flush ⇒ 走"超时 / flush 的短帧"那条出口 ✓（两条出口都要挂上加密 ✓）
//   C 回调故意不改 ⇒ 证明"加密由应用决定"、协议不强制 ✓（回调被调到，但载荷原样出去 ✓）
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

static int g_accept, g_enc, g_dec, g_srv_h;
static long g_enc_bytes, g_dec_bytes;
static int g_cipher = 1;      // 0 = 回调故意不做变换（对照 ✓）

// 固定 keystream（按帧内字节下标取 ⇒ 加解密互逆、且与帧边界无关 ✓）
static void xform(void *buf, int len)
{
    unsigned char *p = (unsigned char*)buf; int i;
    for (i = 0; i < len; i++) p[i] ^= (unsigned char)(0x5A + i);
}

// 事件回调：**持着协议锁**被调用 ⇒ 只做原地变换 / 记数，绝不回调 ffrdp_*（否则自锁 ✓）
static long on_event(void *cbctx, const FFRDP_EVENT *ev)
{
    (void)cbctx;
    if (ev->type == FFRDP_ACCEPT)  { g_accept++; if (g_srv_h < 0) g_srv_h = ev->conn; }
    else if (ev->type == FFRDP_ENCRYPT) { if (g_cipher) xform(ev->buf, ev->len); g_enc++; g_enc_bytes += ev->len; }
    else if (ev->type == FFRDP_DECRYPT) { if (g_cipher) xform(ev->buf, ev->len); g_dec++; g_dec_bytes += ev->len; }
    return FFRDP_NOT_CONSUMED;
}

// 一轮：建一对上下文（不给 hs 键 ⇒ v2 默认就是握手 ✓，也不给加密键 ⇒ 有回调就够了 ✓），
// 跑若干步，双向收发，检查字节是否完好。chunk = 每次发的字节数（1 + flush ⇒ 逼出短帧 ✓）
// want：本段至少要传过去多少字节才算数（按形状给 ✓ —— 每步 1 字节那段本来就慢）
static void phase(const char *name, int chunk, int flush_each, int want)
{
    void *p_srv, *p_c1;
    FFRDPCONTEXT *srv, *c1;
    FFRDP_LOWLEVEL *ll_srv, *ll_c1;
    // sbuf 要 >= 每步发送量（chunk）✗：发 2048 字节却只备 64 字节 ⇒ 越界读栈 ⇒
    // 发出去的本来就是垃圾，跟加密一点关系都没有（这个坑我踩过 ✓）。
    char sbuf[4096], rbuf[512];
    int i, n, up = 0, down = 0, bad = 0;
    long enc0 = g_enc, dec0 = g_dec, encb0 = g_enc_bytes, decb0 = g_dec_bytes;

    printf("── %s\n", name);
    sim_reset(); sim_config(0.0, 0, 4, 0);
    g_accept = 0; g_srv_h = -1;
    memset(sbuf, 'A', sizeof(sbuf));

    p_srv = ffrdp_init("ffrdps://127.0.0.1:8001,mss:1024,fec:0", on_event, NULL);
    p_c1  = ffrdp_init("ffrdpc://127.0.0.1:8001,mss:1024,fec:0", on_event, NULL);
    check(p_srv && p_c1, "两个上下文都建起来了（默认走握手）");
    if (!p_srv || !p_c1) return;
    srv = (FFRDPCONTEXT*)p_srv; c1 = (FFRDPCONTEXT*)p_c1;
    ll_srv = srv->lowlevel; ll_c1 = c1->lowlevel;
    sim_ll_own(ll_srv, A_SRV); sim_ll_own(ll_c1, A_C1);
    sim_ll_route(ll_srv, A_C1, 1); sim_ll_route(ll_c1, A_SRV, 0);

    for (i = 0; i < 3000; i++) {
        sim_setstep(i);
        if (g_srv_h >= 0) { ffrdp_send(srv, g_srv_h, sbuf, chunk, 0); if (flush_each) ffrdp_flush(srv, g_srv_h); }
        ffrdp_send(c1, 0, sbuf, chunk, 0);                  if (flush_each) ffrdp_flush(c1, 0);
        sim_begin(1); sim_ll_run(1, p_c1);
        sim_begin(0); sim_ll_run(0, p_srv);
        if (g_srv_h >= 0)
            while ((n = ffrdp_recv(srv, g_srv_h, rbuf, sizeof(rbuf), 0)) > 0)
                { int k; for (k = 0; k < n; k++) if (rbuf[k] != 'A') bad++; up += n; }
        while ((n = ffrdp_recv(c1, 0, rbuf, sizeof(rbuf), 0)) > 0)
            { int k; for (k = 0; k < n; k++) if (rbuf[k] != 'A') bad++; down += n; }
        if (g_srv_h >= 0 && up > want && down > want) break;
    }
    printf("     （跑了 %d 步：ACCEPT=%d enc=%ld 帧/%ld 字节 dec=%ld 帧/%ld 字节，上行 %d 下行 %d，坏字节 %d）\n",
           i + 1, g_accept, g_enc - enc0, g_enc_bytes - encb0, g_dec - dec0, g_dec_bytes - decb0, up, down, bad);

    check(g_accept == 1, "服务端接受了这条连接（ACCEPT ×1）");
    check(g_srv_h >= 1, "句柄来自 accept（不是 0 号主连接）");
    check(g_enc > enc0 && g_dec > dec0, "加/解密回调都被调到（双向都走回调）");
    // 收尾时还可能有在途数据（协议是对称的 ✓）⇒ 相等是**错的**判据 ✗；
    // 该判的是：这一面确实解过、而且解的没有加密的多 ✓。
    check(g_dec_bytes - decb0 > 0 && g_enc_bytes - encb0 >= g_dec_bytes - decb0,
          "本段解过字节、且解密字节数 <= 加密字节数（差值是还没交付完的在途数据）");
    check(up > want && down > want && bad == 0, "双向数据完整到达且逐字节是 'A'（⇒ 线上是密文 ✓）");
    ffrdp_exit(p_c1); ffrdp_exit(p_srv);
}

int main(void)
{
    phase("A 大块发送（出口 ①：攒满一个 smss）", 2048, 0, 4096);
    phase("B 每步 1 字节 + flush（出口 ②：超时 / flush 的短帧）", 1, 1, 1024);

    printf("── C 回调故意不做任何变换（对照段）\n");
    sim_reset(); sim_config(0.0, 0, 4, 0);
    g_cipher = 0;
    phase("C 回调不改载荷（协议不该强制加密）", 2048, 0, 4096);
    g_cipher = 1;
    check(g_enc > 0 && g_dec > 0, "回调存在时事件一定被调到（改不改由应用决定）");

    printf("       verdict -> %s\n", g_fail ? "FAIL" : "OK");
    return g_fail ? 1 : 0;
}
