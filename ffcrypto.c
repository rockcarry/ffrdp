/* ffcrypto.c —— 零依赖对称加密模块（默认后端：内置 ChaCha20）
 *
 * 设计要点（看注释就够，不需要读 ffrdp.c）：
 *   1) ffrdp 的回调是"原地改、长度不变"（ffrdp.h:69）⇒ 只能用流密码/计数器模式，不能带 tag。
 *   2) ChaCha20 是**连续密钥流**：同一 (连接,方向) 的第 N 个字节用密钥流第 N 个字节。
 *      ffrdp 保证 ENCRYPT 按发送流顺序、每帧一次；DECRYPT 按交付顺序、每帧一次
 *      ⇒ 应用侧只要维护"这条流已经用了多少字节"，加解密自然对齐。
 *   3) 计数器用**64 位**（djb 原始布局：state[12..13]=counter，state[14..15]=nonce），
 *      不用 IETF 的 32 位计数器 —— 32 位只有 256 GiB/密钥流，24 小时不间断的摄像头
 *      在 8 Mbps 下约 3 天就会回绕（回绕 = 密钥流复用 = 直接破）。
 *   4) 方向隔离：new 时用 ChaCha20 自己派生 key_c2s / key_s2c（counter=1 / 2 各取一块），
 *      不额外引入 SHA-256 之类的原语；再按本端角色把它们映射到 TX/RX
 *      （客户端 TX=c2s，服务端 TX=s2c）⇒ 两端对同一方向用的是**同一把**密钥，
 *      而上下行用的是不同的两把 ⇒ 同一条连接的两个方向不可能共用密钥流。
 *      ⚠️ 这一步不能做成"每端 TX/RX 各一把互不相干的密钥"：那样一端加密用的 key
 *         和另一端解密用的 key 就不同了（这个坑已经踩过一次，selftest 里钉住）。
 *   5) 多连接隔离：每个方向的字节流最前面 8 字节是**盐记录**（见下），进 nonce。
 *      不设盐 ⇒ 所有连接共用一段零盐密钥流 ⇒ 只适合单连接场景。
 *   6) 没有全局状态：所有状态在 FFC_CTX 里，一个实例配一个 ffrdp 上下文。
 *
 * 盐记录（8 字节，**原样放过、不加密**，它是 nonce 不是密钥）：
 *   发送方：先 ffc_set_salt(TX) 拿定盐，再把这 8 字节作为该方向的**第一批数据**发出去；
 *   接收方：什么都不用做 —— 模块在第一次 ffc_crypt(RX) 时把这 8 字节取出来当盐，
 *           之后的字节才解密。应用只需要在读流时**丢掉这 8 字节**（module 不能改长度）。
 *   "有没有盐记录"由**是否调过 ffc_set_salt** 决定（内部的 rec 标志）：调过 ⇒ 该流最前面
 *   8 字节原样放过、接收侧据此取盐；没调过 ⇒ 零盐、**一个字节都不跳**。两端必须口径一致
 *   （都调或都不调），否则密钥流错位。
 *   为什么必须做成"模块认得的记录"：ffrdp 会把它们当成普通数据交给 ENCRYPT ⇒ 若模块
 *   照常加密，接收端就没法先从里面读盐（先有鸡先有蛋）。所以双方都约定：这条流的前 8
 *   字节不解密。⚠️ 这个位置必须**严格**是流的最前面 8 字节，不能被别的数据抢先。
 *
 * 后端可选（当前只实现 builtin；另外两个是下一步，接口面只有两个函数要换）：
 *     -DFFC_BACKEND=0  内置 ChaCha20（默认，零依赖）
 *     -DFFC_BACKEND=1  OpenSSL（EVP_aes_256_ctr / EVP_chacha20，实测跨 update 连续）
 *     -DFFC_BACKEND=2  mbedTLS（mbedtls_aes_crypt_ctr / mbedtls_chacha20_update）
 */
#include "ffcrypto.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FFC_BACKEND_BUILTIN 0
#define FFC_BACKEND_OPENSSL 1
#define FFC_BACKEND_MBEDTLS 2

#ifndef FFC_BACKEND
#define FFC_BACKEND FFC_BACKEND_BUILTIN
#endif

#if FFC_BACKEND == FFC_BACKEND_OPENSSL
#error "FFC_BACKEND=OPENSSL 尚未实现（接口已留好：只需替换 stream_init / stream_xor 两个函数）"
#elif FFC_BACKEND == FFC_BACKEND_MBEDTLS
#error "FFC_BACKEND=MBEDTLS 尚未实现（接口已留好：只需替换 stream_init / stream_xor 两个函数）"
#elif FFC_BACKEND != FFC_BACKEND_BUILTIN
#error "FFC_BACKEND 取值非法（0=builtin 1=openssl 2=mbedtls）"
#endif

/* 盐记录的阶段 */
#define FFC_PH_NEW   0   /* 还没开始：第一次 crypt 时进入记录阶段 */
#define FFC_PH_SALT  1   /* 正在过 8 字节盐记录（原样放过）*/
#define FFC_PH_DATA  2   /* 之后都是密文 */

/* ── 一条 (连接,方向) 的流状态 ───────────────────────────────────────────────── */
typedef struct {
    uint32_t keyw[8];                 /* 本方向的 32 字节密钥（已派生）*/
    uint8_t  nonce[FFC_SALT_LEN];     /* 盐（两端必须一致）*/
    uint8_t  ks[64];                  /* 当前这一块的密钥流 */
    uint64_t ctr;                     /* 下一块的块号（64 位 ⇒ 永不回绕）*/
    int      pos;                     /* ks 里已经用掉多少字节；64 = 该生成下一块了 */
    int      live;                    /* 1 = 密钥流已就绪（盐到手之后）*/
    int      phase;                   /* FFC_PH_xxx */
    uint8_t  seeded;                  /* 1 = ffc_set_salt 给过 nonce（本地权威，不随线上字节变）*/
    uint8_t  left;                    /* 盐记录还剩几字节要放过 */
    uint8_t  hpos;                    /* 已从盐记录里收集到的字节数 */
    uint8_t  hsalt[FFC_SALT_LEN];     /* 从盐记录里收集到的字节 */
} FFC_STREAM;

struct ffc_ctx {
    uint32_t   keyw[2][8];                          /* [dir][word] */
    FFC_STREAM st[FFC_MAXCONN][2];
};

/* ── 内置 ChaCha20 ───────────────────────────────────────────────────────────── */
#define ROTL32(v, n) (((v) << (n)) | ((v) >> (32 - (n))))
#define QR(a, b, c, d)                     \
    do {                                   \
        a += b; d ^= a; d = ROTL32(d, 16); \
        c += d; b ^= c; b = ROTL32(b, 12); \
        a += b; d ^= a; d = ROTL32(d, 8);  \
        c += d; b ^= c; b = ROTL32(b, 7);  \
    } while (0)

static uint32_t le32(const void *p)
{
    const uint8_t *b = (const uint8_t*)p;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static void put32(void *p, uint32_t v)
{
    uint8_t *b = (uint8_t*)p;
    b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24);
}

// 纯块函数：只吃 16 个状态字，与"用哪种 nonce/counter 布局"无关 ⇒ 可以直接拿 RFC 8439 的向量验。
static void chacha20_block(uint32_t out[16], const uint32_t in[16])
{
    uint32_t x[16];
    int      i;
    memcpy(x, in, sizeof(x));
    for (i = 0; i < 10; i++) {   // 20 轮 = 10 次"列轮 + 对角轮"
        QR(x[0], x[4], x[8],  x[12]); QR(x[1], x[5], x[9],  x[13]);
        QR(x[2], x[6], x[10], x[14]); QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]); QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8],  x[13]); QR(x[3], x[4], x[9],  x[14]);
    }
    for (i = 0; i < 16; i++) out[i] = x[i] + in[i];
}

// 状态布局（djb 原始版）：常量 || key(8 字) || counter(64 位) || nonce(64 位)
static void chacha20_state(uint32_t st[16], const uint32_t keyw[8], const uint8_t nonce[FFC_SALT_LEN], uint64_t ctr)
{
    st[0] = 0x61707865; st[1] = 0x3320646e; st[2] = 0x79622d32; st[3] = 0x6b206574;  // "expand 32-byte k"
    memcpy(st + 4, keyw, 8 * sizeof(uint32_t));
    st[12] = (uint32_t)ctr;
    st[13] = (uint32_t)(ctr >> 32);
    st[14] = le32(nonce + 0);
    st[15] = le32(nonce + 4);
}

static void stream_arm(FFC_STREAM *s)   // 盐已就绪 ⇒ 把密钥流从字节 0 起好
{
    s->ctr  = 0;
    s->pos  = 64;
    s->live = 1;
}

static void stream_next_block(FFC_STREAM *s)
{
    uint32_t st[16], out[16];
    int      i;
    chacha20_state(st, s->keyw, s->nonce, s->ctr);
    chacha20_block(out, st);
    for (i = 0; i < 16; i++) put32(s->ks + 4 * i, out[i]);
    s->ctr++;
    s->pos = 0;
}

// 原地异或（调用方保证 live），跨调用连续：同一 (conn,dir) 的第 N 个字节对应密钥流第 N 个字节。
static void stream_xor(FFC_STREAM *s, uint8_t *p, int len)
{
    while (len > 0) {
        int n;
        if (s->pos == 64) stream_next_block(s);
        n = 64 - s->pos;
        if (n > len) n = len;
        while (n >= 8) {   // 8 字节一拍：memcpy 让编译器发一条 load/store（不假设对齐）
            uint64_t a, k;
            memcpy(&a, p, 8);
            memcpy(&k, s->ks + s->pos, 8);
            a ^= k;
            memcpy(p, &a, 8);
            p += 8; s->pos += 8; n -= 8; len -= 8;
        }
        while (n > 0) { *p++ ^= s->ks[s->pos++]; n--; len--; }
    }
}

/* ── 生命周期 ───────────────────────────────────────────────────────────────── */
FFC_CTX *ffc_new(const uint8_t key[FFC_KEY_LEN], int is_server)
{
    FFC_CTX *c;
    uint32_t psk[8], st[16], out[16], k[2][8];   // k[0] = c2s，k[1] = s2c
    uint8_t  zero[FFC_SALT_LEN] = {};
    int      i, d;

    if (!key) return NULL;
    c = (FFC_CTX*)calloc(1, sizeof(*c));
    if (!c) return NULL;

    for (i = 0; i < 8; i++) psk[i] = le32(key + 4 * i);

    // 派生两把方向密钥：c2s = block(ctr=1)，s2c = block(ctr=2)（各取前 32 字节）
    // 用 ChaCha20 自己当 KDF ⇒ 不额外引入 SHA-256；数据流从 ctr=0 开始，不与派生段重叠。
    for (d = 0; d < 2; d++) {
        chacha20_state(st, psk, zero, (uint64_t)(d + 1));
        chacha20_block(out, st);
        memcpy(k[d], out, sizeof(k[d]));
    }
    // 角色只决定"哪个方向对应哪把密钥"；两端给相反的角色，同一方向就落到同一把密钥上。
    memcpy(c->keyw[FFC_TX], k[is_server ? 1 : 0], sizeof(c->keyw[FFC_TX]));
    memcpy(c->keyw[FFC_RX], k[is_server ? 0 : 1], sizeof(c->keyw[FFC_RX]));
    return c;
}

void ffc_free(FFC_CTX *c)
{
    if (!c) return;
    memset(c, 0, sizeof(*c));   // 先把密钥/密钥流擦掉再还
    free(c);
}

/* ── 每连接 ─────────────────────────────────────────────────────────────────── */
void ffc_set_salt(FFC_CTX *c, int conn, int dir, const uint8_t salt[FFC_SALT_LEN])
{
    FFC_STREAM *s;
    if (!c || !salt || conn < 0 || conn >= FFC_MAXCONN || dir < 0 || dir > 1) return;
    s = &c->st[conn][dir];
    memcpy(s->keyw, c->keyw[dir], sizeof(s->keyw));
    memcpy(s->nonce, salt, FFC_SALT_LEN);
    s->phase = FFC_PH_SALT;   // 这条流的前 8 字节是盐记录本身（原样放过）
    s->seeded= 1;             // nonce 以本地设的为准（线上的那 8 字节只是照样发给对端）
    s->left  = FFC_SALT_LEN;
    s->hpos  = 0;
    s->live  = 0;
    s->pos   = 64;
    s->ctr   = 0;
}

void ffc_crypt(FFC_CTX *c, int conn, int dir, void *buf, int len)
{
    FFC_STREAM *s;
    uint8_t    *p = (uint8_t*)buf;

    if (!c || !buf || len <= 0) return;
    if (conn < 0 || conn >= FFC_MAXCONN || dir < 0 || dir > 1) return;
    s = &c->st[conn][dir];

    if (s->phase == FFC_PH_NEW) { // 第一次调用 ⇒ 进入盐记录阶段（不管有没有 set_salt）
        memcpy(s->keyw, c->keyw[dir], sizeof(s->keyw));
        s->phase = FFC_PH_SALT;
        s->left  = FFC_SALT_LEN;
        s->hpos  = 0;
        s->live  = 0;
        s->pos   = 64;
        s->ctr   = 0;
    }
    if (s->phase == FFC_PH_SALT) {
        int d = (len < (int)s->left) ? len : (int)s->left;
        // 两侧都把记录里的字节收下来：接收侧只能靠它（没有别的途径知道对端的盐），
        // 发送侧收下来只是为了"没 set_salt 时两端仍然一致"（否则发送用零盐、接收用线上那 8 字节）。
        memcpy(s->hsalt + s->hpos, p, (size_t)d);
        s->hpos += (uint8_t)d;
        p += d; len -= d; s->left = (uint8_t)(s->left - d);
        if (s->left == 0) {
            if (!s->seeded) memcpy(s->nonce, s->hsalt, FFC_SALT_LEN);  // 没设过就信线上那 8 字节
            s->phase = FFC_PH_DATA;
            stream_arm(s);
        }
        if (len <= 0) return;
    }
    if (!s->live) { // 兜底：状态被外部改坏时也别崩（按零盐继续）
        s->phase = FFC_PH_DATA;
        stream_arm(s);
    }
    stream_xor(s, p, len);
}

void ffc_drop(FFC_CTX *c, int conn)
{
    if (!c || conn < 0 || conn >= FFC_MAXCONN) return;
    memset(&c->st[conn], 0, sizeof(c->st[conn]));
}

/* ── 自检（不依赖任何已创建的实例）───────────────────────────────────────────── */
// RFC 8439 §2.3.2 的块向量（key = 00..1f，nonce = 000000090000004a00000000，counter = 1）
static int selftest_block(void)
{
    static const uint8_t key[32] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
        0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f
    };
    static const uint8_t expect[64] = {
        0x10,0xf1,0xe7,0xe4,0xd1,0x3b,0x59,0x15,0x50,0x0f,0xdd,0x1f,0xa3,0x20,0x71,0xc4,
        0xc7,0xd1,0xf4,0xc7,0x33,0xc0,0x68,0x03,0x04,0x22,0xaa,0x9a,0xc3,0xd4,0x6c,0x4e,
        0xd2,0x82,0x64,0x46,0x07,0x9f,0xaa,0x09,0x14,0xc2,0xd7,0x05,0xd9,0x8b,0x02,0xa2,
        0xb5,0x12,0x9c,0xd1,0xde,0x16,0x4e,0xb9,0xcb,0xd0,0x83,0xe8,0xa2,0x50,0x3c,0x4e
    };
    uint32_t st[16], out[16];
    uint8_t  got[64];
    int      i;

    st[0] = 0x61707865; st[1] = 0x3320646e; st[2] = 0x79622d32; st[3] = 0x6b206574;
    for (i = 0; i < 8; i++) st[4 + i] = le32(key + 4 * i);
    st[12] = 1; // counter（块函数与布局无关，这里按 RFC 的 IETF 写法摆状态字）
    st[13] = 0x09000000;
    st[14] = 0x4a000000;
    st[15] = 0x00000000;

    chacha20_block(out, st);
    for (i = 0; i < 16; i++) put32(got + 4 * i, out[i]);
    return memcmp(got, expect, sizeof(expect)) ? 1 : 0;
}

// 端到端 + 方向/连接隔离 + 两条出口等价（用真实实例，覆盖 new/派生/盐记录/状态机）
static int selftest_ctx(void)
{
    static uint8_t key[FFC_KEY_LEN], salt[FFC_SALT_LEN] = {1,2,3,4,5,6,7,8};
    static uint8_t salt2[FFC_SALT_LEN] = {9,9,9,9,9,9,9,9};
    static uint8_t orig[192], wire[200], wire2[200];
    FFC_CTX *cli, *srv;
    int      i, bad = 0;

    for (i = 0; i < FFC_KEY_LEN; i++) key[i] = (uint8_t)(0x40 + i);
    for (i = 0; i < (int)sizeof(orig); i++) orig[i] = (uint8_t)('a' + (i % 26));

    cli = ffc_new(key, 0);
    srv = ffc_new(key, 1);
    if (!cli || !srv) { ffc_free(cli); ffc_free(srv); return 1; }

    // ① 端到端：客户端 TX（盐记录 + 数据）→ 服务端 RX ⇒ 盐原样、数据还原
    memcpy(wire, salt, FFC_SALT_LEN);
    memcpy(wire + FFC_SALT_LEN, orig, sizeof(orig));
    ffc_set_salt(cli, 0, FFC_TX, salt);
    ffc_crypt(cli, 0, FFC_TX, wire, (int)sizeof(wire));
    memcpy(wire2, wire, sizeof(wire2));
    ffc_crypt(srv, 0, FFC_RX, wire2, (int)sizeof(wire2));            // 接收侧不用 set_salt
    if (memcmp(wire2, salt, FFC_SALT_LEN))                bad |= 1;  // 盐本身不能被动
    if (memcmp(wire2 + FFC_SALT_LEN, orig, sizeof(orig))) bad |= 2;  // 数据必须还原

    // ② 反方向：服务端 TX → 客户端 RX
    memcpy(wire, salt, FFC_SALT_LEN);
    memcpy(wire + FFC_SALT_LEN, orig, sizeof(orig));
    ffc_set_salt(srv, 0, FFC_TX, salt);
    ffc_crypt(srv, 0, FFC_TX, wire, (int)sizeof(wire));
    memcpy(wire2, wire, sizeof(wire2));
    ffc_crypt(cli, 0, FFC_RX, wire2, (int)sizeof(wire2));
    if (memcmp(wire2 + FFC_SALT_LEN, orig, sizeof(orig))) bad |= 4;

    // ③ 同一条连接、同盐：上下行必须是两段不同的密钥流（比较数据段）
    {
        memcpy(wire , salt, FFC_SALT_LEN); memcpy(wire  + FFC_SALT_LEN, orig, sizeof(orig));
        memcpy(wire2, salt, FFC_SALT_LEN); memcpy(wire2 + FFC_SALT_LEN, orig, sizeof(orig));
        ffc_set_salt(cli, 1, FFC_TX, salt);
        ffc_crypt(cli, 1, FFC_TX, wire, (int)sizeof(wire));
        ffc_set_salt(cli, 1, FFC_RX, salt);   // 用 set_salt 起一条"反过来"的流做对比
        ffc_crypt(cli, 1, FFC_RX, wire2, (int)sizeof(wire2));
        if (!memcmp(wire + FFC_SALT_LEN, wire2 + FFC_SALT_LEN, sizeof(orig))) bad |= 8;
    }

    // ④ 多连接隔离：同方向、同明文、不同盐 ⇒ 数据段密文必须不同（多 monitor 场景的前提）
    {
        memcpy(wire , salt , FFC_SALT_LEN); memcpy(wire  + FFC_SALT_LEN, orig, sizeof(orig));
        memcpy(wire2, salt2, FFC_SALT_LEN); memcpy(wire2 + FFC_SALT_LEN, orig, sizeof(orig));
        ffc_set_salt(cli, 2, FFC_TX, salt);
        ffc_set_salt(cli, 3, FFC_TX, salt2);
        ffc_crypt(cli, 2, FFC_TX, wire,  (int)sizeof(wire));
        ffc_crypt(cli, 3, FFC_TX, wire2, (int)sizeof(wire2));
        if (!memcmp(wire + FFC_SALT_LEN, wire2 + FFC_SALT_LEN, sizeof(orig))) bad |= 16;
    }

    // ⑤ 两条出口等价：把盐记录拆成 5+3 字节两次喂 == 一次喂（跨调用连续 + 部分记录）
    {
        memcpy(wire , salt, FFC_SALT_LEN); memcpy(wire  + FFC_SALT_LEN, orig, sizeof(orig));
        memcpy(wire2, salt, FFC_SALT_LEN); memcpy(wire2 + FFC_SALT_LEN, orig, sizeof(orig));
        ffc_set_salt(cli, 4, FFC_TX, salt);
        ffc_crypt(cli, 4, FFC_TX, wire, (int)sizeof(wire));
        ffc_set_salt(cli, 5, FFC_TX, salt);
        ffc_crypt(cli, 5, FFC_TX, wire2, 5);                       // 盐只给了一半
        ffc_crypt(cli, 5, FFC_TX, wire2 + 5, (int)sizeof(wire2) - 5);
        if (memcmp(wire, wire2, sizeof(wire))) bad |= 32;
    }

    // ⑥ ffc_drop + 重新设盐 ⇒ 密钥流从字节 0 重来（重连复位靠这个）
    {
        memcpy(wire , salt, FFC_SALT_LEN); memcpy(wire  + FFC_SALT_LEN, orig, sizeof(orig));
        memcpy(wire2, salt, FFC_SALT_LEN); memcpy(wire2 + FFC_SALT_LEN, orig, sizeof(orig));
        ffc_set_salt(cli, 6, FFC_TX, salt);
        ffc_crypt(cli, 6, FFC_TX, wire, (int)sizeof(wire));
        ffc_set_salt(cli, 6, FFC_TX, salt);
        ffc_crypt(cli, 6, FFC_TX, wire2, (int)sizeof(wire2) / 2);   // 先跑一半（模拟断线前）
        ffc_drop(cli, 6);
        ffc_set_salt(cli, 6, FFC_TX, salt);
        memcpy(wire2, salt, FFC_SALT_LEN); memcpy(wire2 + FFC_SALT_LEN, orig, sizeof(orig));
        ffc_crypt(cli, 6, FFC_TX, wire2, (int)sizeof(wire2));
        if (memcmp(wire, wire2, sizeof(wire))) bad |= 64;
    }

    // ⑦ 逐字节喂（最刁的口径：每次 1 字节，盐记录也被拆成 8 次）
    {
        memcpy(wire , salt, FFC_SALT_LEN); memcpy(wire  + FFC_SALT_LEN, orig, sizeof(orig));
        memcpy(wire2, salt, FFC_SALT_LEN); memcpy(wire2 + FFC_SALT_LEN, orig, sizeof(orig));
        ffc_set_salt(cli, 7, FFC_TX, salt);
        ffc_crypt(cli, 7, FFC_TX, wire, (int)sizeof(wire));
        ffc_set_salt(cli, 8, FFC_TX, salt);
        for (i = 0; i < (int)sizeof(wire2); i++) ffc_crypt(cli, 8, FFC_TX, wire2 + i, 1);
        if (memcmp(wire, wire2, sizeof(wire))) bad |= 128;
    }

    // ⑧ 两端都不调 set_salt：nonce 取线上那 8 字节记录（全零）⇒ 仍然解得开，且只跳记录本身
    {
        uint8_t pre[FFC_SALT_LEN] = {};
        memcpy(wire,  pre, FFC_SALT_LEN); memcpy(wire + FFC_SALT_LEN, orig, sizeof(orig));
        ffc_crypt(cli, 9, FFC_TX, wire, (int)(FFC_SALT_LEN + sizeof(orig)));   // 双方都没 set_salt
        memcpy(wire2, wire, FFC_SALT_LEN + sizeof(orig));
        ffc_crypt(srv, 9, FFC_RX, wire2, (int)(FFC_SALT_LEN + sizeof(orig)));
        if (memcmp(wire + FFC_SALT_LEN, orig, sizeof(orig)) == 0)  bad |= 256;  // 得真加密过
        if (memcmp(wire2 + FFC_SALT_LEN, orig, sizeof(orig)))      bad |= 512;  // 且必须还原
        if (memcmp(wire2, pre, FFC_SALT_LEN))                      bad |= 1024; // 记录本身不许被动
    }

    ffc_free(cli);
    ffc_free(srv);
    return bad;
}

int ffc_selftest(void)
{
    int bad = 0;
    if (selftest_block()) bad |= 1;
    if (selftest_ctx())   bad |= 2;
    return bad;
}

/* ── 测速 ───────────────────────────────────────────────────────────────────── */
static volatile uint8_t g_sink;   // 防止编译器把测速循环整个优化掉

double ffc_bench(size_t bytes)
{
    static uint8_t key[FFC_KEY_LEN], salt[FFC_SALT_LEN];
    uint8_t  buf[1400] = {};   // 与 ffrdp 的帧大小同量级（小包是真实场景）
    FFC_CTX *c;
    size_t   done = 0;
    clock_t  t0, t1;

    memset(key,  0x33, sizeof(key ));
    memset(salt, 0x44, sizeof(salt));
    c = ffc_new(key, 0);
    if (!c) return 0.0;
    ffc_set_salt(c, 0, FFC_TX, salt);
    ffc_crypt(c, 0, FFC_TX, buf, (int)sizeof(buf));   // 先过掉盐记录

    t0 = clock();
    while (done < bytes) { ffc_crypt(c, 0, FFC_TX, buf, (int)sizeof(buf)); done += sizeof(buf); }
    t1 = clock();
    g_sink ^= buf[0];
    ffc_free(c);
    if (t1 == t0) return 0.0;
    return ((double)done / (1024.0 * 1024.0)) / ((double)(t1 - t0) / (double)CLOCKS_PER_SEC);
}

const char *ffc_backend(void) { return "builtin-chacha20"; }
