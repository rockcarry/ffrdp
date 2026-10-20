# ffrdp 加密接入指南

本文只讲**加密怎么接**：ffrdp 提供了什么钩子、由这些钩子能推出哪些方案、内置方案（`ffcrypto`）怎么用、
TLS 怎么架上去、以及 `test.c` 里那套加密功能怎么编译和跑。

> 协议本身的细节（握手/挥手、拥塞控制、dump 字段）见同目录的 [`ffrdp-v2.md`](ffrdp-v2.md)。
> 本文档对应的代码：`ffrdp.h`（回调契约）、`ffcrypto.h` / `ffcrypto.c`（内置方案）、`test.c`（参考实现）；
> 文中出现的代码路径都相对**仓库根目录**（本文自己放在 `docs/`）。

---

## 0. 结论速查

| 你想干什么 | 该怎么做 | 代价 |
|---|---|---|
| 局域网里"防止随手抓包/防运营商钓鱼 WiFi" | 挂 `ENCRYPT`/`DECRYPT` 回调 + `ffcrypto`（内置 ChaCha20，零依赖） | **只有机密性**：不防篡改、不防注入、无前向保密 |
| 需要真正的安全（认证、防篡改、PFS） | **TLS 架在 ffrdp 之上**（`SSL_*` + memory BIO，走 `ffrdp_send/recv`） | 多一层拷贝、record 开销、需要 OpenSSL |
| 不想加密 | 不挂回调。协议照常工作，线上就是明文 | — |
| 想在回调里塞 TLS/DTLS | **做不到**，见 §6.1（回调等长原地 + 不能自己发帧） | — |

能力边界一句话：**ffrdp 只提供一个"等长、原地、按帧、有状态"的载荷变换钩子**，它不做任何密码学决策。
机密性/完整性/认证/密钥协商全在应用侧。

---

## 1. ffrdp 给了什么

### 1.1 两个事件

```c
enum {
    ...
    FFRDP_ENCRYPT,   // 发帧"定稿"时：原地修改这一帧载荷，len 不变
    FFRDP_DECRYPT,   // 收到帧、即将交付给应用时：原地修改，len 不变
};

typedef struct {
    int      type;   // FFRDP_xxx
    int      conn;   // 连接句柄（不透明 int！见 §4.7 陷阱 1）
    int      code;
    uint64_t addr;
    void    *buf;    // 载荷起始地址
    int      len;    // 载荷长度（任意值，1 字节也可能）
} FFRDP_EVENT;
```

`ffrdp.h` 里写死的边界（`ffrdp.h:36-37`）：

```c
//   - 加密回调：协议**原样**交付载荷 ✓、不改长度 ✓；块要求由**应用**自己覆盖块整数倍的部分 ✓，
//     剩余字节留明文 ✓；**对称（先加密后解密能还原）是唯一原则** ✓
```

### 1.2 五条硬约束（整个设计都从这里推出来）

| # | 事实 | 后果 |
|---|---|---|
| C1 | 回调是**原地、等长** | 不能加 IV、不能加 tag、不能填充、不能压缩 |
| C2 | 每次交付的是**帧载荷**（≤ mss，默认 1280 减头），长度任意 | 大消息会被拆成多帧 ⇒ 每帧一次回调 |
| C3 | `ENCRYPT` 在**帧定稿时只调一次**，之后帧节点被**原样重传**（不再回调） | 密钥流每帧只推进一次；反过来，若你把加密写在"每次 `sendto` 之前"，重传就会二次加密 ⇒ 对端必然解不开 |
| C4 | 一次性挂**两条出口**：①攒满 mss 的整帧 ②超时/`ffrdp_flush` 的短帧（协议已统一处理，应用无需区分） | 若只有一条被加密，另一端会把明文当密文解 ⇒ 全坏 |
| C5 | 回调在 `ffrdp->lock` 内被调用（`ffrdp.c:1407`、`ffrdp.c:1663`） | 回调里**绝不能**再调 `ffrdp_*`（自锁）；也别在里面做耗时工作（会拖住整条协议） |

另一个关键事实：**`ENCRYPT` 的调用顺序 == 发送流顺序**，`DECRYPT` 的调用顺序 == 交付顺序
（乱序帧在交付前就被丢了，重传帧不会二次解密）。这一条是所有"流密码式"方案能成立的全部依据。

---

## 2. 由约束推出的方案空间

### 2.1 为什么"每帧随机 IV"不行

最直觉的方案是 AES-CBC/GCM 每帧随机 IV。但 C1 不给空间：一帧 1280 字节的载荷里，加上 16 字节 IV 就
必须挤掉 16 字节应用数据 —— 协议不接受长度变化，应用也没有"往帧里塞头"的接口。

**唯一能做的是让"位置"代替"IV"**：所有帧按顺序拼起来当一条字节流，第 N 个字节用密钥流第 N 个字节。
这就是流密码 / CTR 模式。副作用是：任何一处错位（丢一帧、跳一字节、少解一次）都会让**后面全部**变成乱码。

### 2.2 为什么不能"只加密块整数倍、余下留明文"

`ffrdp.h:36` 允许这么干（那是为 AES-CBC 这类块密码留的口子），但对**流密码是致命的**：

```
帧 A = 1280 字节，块大小 16 ⇒ 全加密（1280 % 16 == 0）
帧 B = 1300 字节 ⇒ 前 1296 加密、末尾 4 字节明文
接收端如果按"整段都加密"来解，密钥流就对不齐，后面每一个字节都错位
```

而且"末尾留明文"本身会泄露长度模式。**要么全加密，要么别用流密码。**

### 2.3 因此只有三条路

1. **回调 + 等长流密码**（本文 §3~§5）：零依赖、零长度开销、最省 CPU，但只有机密性。
2. **回调什么都不做 + TLS 架在 ffrdp 之上**（§6）：完整安全，代价是 OpenSSL 与一层拷贝。
3. **不加密**：调试、可信内网。

> 想"在回调里既机密又完整"是**不可能**的：AEAD 的 tag 至少要 16 字节，而 C1 规定长度不变。
> 除非你在应用协议里为每帧预留 tag 空间（那你其实是在应用层做 AEAD，不是在用回调做）。

---

## 3. 内置方案 `ffcrypto`

`ffcrypto.c` 是"应用侧那份密码学"，刻意**不进 `libffrdp.a`**：库保持零依赖、且不认识密码学。

### 3.1 算法

- **ChaCha20**（20 轮），内置实现，无外部依赖；不用 AES 是因为它不挑 CPU（无 AES-NI 的 ARM/老 x86 也能跑快）。
- 后端可切：`-DFFC_BACKEND=0` 内置（默认）/ `1` OpenSSL / `2` mbedTLS（后两个目前是 `#error` 占位，接口只留了
  `stream_arm` / `stream_xor` 两个函数要换）。

### 3.2 方向密钥由 PSK 派生

```c
// ffcrypto.c：用 ChaCha20 自己当 KDF，不引入 SHA-256
c2s = block(psk, nonce=0, counter=1) // 32 字节
s2c = block(psk, nonce=0, counter=2) // 32 字节
客户端 TX = c2s, RX = s2c
服务端 TX = s2c, RX = c2s            // 同一方向两端落到同一把密钥上，上下行不同密钥
```

角色给反了不会报错，只会解出乱码 —— 这也是"两端表现为 crc 全错"的最常见原因之一。

### 3.3 连续密钥流 + 64 位计数器

状态布局用 **djb 原始版**（`state[12..13]` 是 64 位计数器，`state[14..15]` 是 8 字节 nonce），
不用 IETF 的 32 位计数器：32 位只有 256 GiB/密钥流，8 Mbps 连续推大约 3 天就回绕并开始复用密钥流。

`FFC_STREAM` 里保存 `ks[64]`（当前块）+ `pos`（用到第几字节），所以 `ffc_crypt` 可以被任意切分调用
（1 字节、5+3 字节、整帧都等价）—— 这是它能在 C2/C3 下正确工作的前提。

### 3.4 盐记录：8 字节明文，放在每个方向字节流的最前面

多连接必须隔离（同一把 PSK + 同一段密钥流 = two-time pad），所以每条连接每个方向给一个 8 字节盐（nonce）。
但回调不能带外传参 ⇒ 盐只能放在**流里**：

```
线上（客户端 → 服务端，某条连接）：
┌────────────┬──────────────────────────────────────────┐
│ 8 字节盐   │ 密文（密钥流第 0 字节开始）              │
│ 明文原样   │                                          │
└────────────┴──────────────────────────────────────────┘
```

- 发送侧：`ffc_set_salt(TX, salt)` 之后，把这 8 字节当作该方向的**第一批数据**发出去
  （`ffrdp_send` 会触发 `ENCRYPT`，模块在 `FFC_PH_SALT` 阶段把它们**原样放过、不解密**）。
- 接收侧：**什么都不用做**。模块在第一次 `ffc_crypt(RX)` 时把这 8 字节收成盐，之后的字节才解密。
- 应用只负责一件事：**读流时把这 8 字节丢掉**（模块改不了长度，它没法替你把字节吃掉）。
- 没调 `ffc_set_salt` ⇒ 零盐，且按约定线上仍然要先有 8 字节（两端一致即可）。

⚠️ 盐记录必须**严格**是这条流的最前面 8 字节。先发了业务数据再设盐 ⇒ 对端把业务数据当盐 ⇒ 从第一帧起全错，
而且没有任何报错。

### 3.5 多连接 / 断线重连

- 状态是 `st[conn][dir]` 的数组（`FFC_MAXCONN`，默认 16），每个 (连接,方向) 一条独立密钥流。
- `ffc_drop(fc, conn)` 清零该连接两个方向 ⇒ 同一句柄重连后从字节 0 重来。
  **不 drop 就是带着上一条连接的偏移继续跑**，表现为"重连后第一条数据起就是乱码"。
- 盐要求**唯一**、不要求不可预测（它是 nonce 不是密钥）：时间 + pid + 计数器足矣。

### 3.6 自检与测速

```c
int    ffc_selftest(void);      // 0 = 全过：RFC 8439 块向量 + 8 项上下文自检
double ffc_bench(size_t bytes); // 返回 MB/s
const char *ffc_backend(void);  // "builtin-chacha20"
```

`ffc_selftest()` 覆盖：RFC 8439 §2.3.2 块向量、端到端、反方向、上下行密钥流不同、多连接不同盐不同密文、
盐记录分片（5+3）、逐字节喂入、`ffc_drop` 复位、零盐通路。
实测（本机 x86_64，`-O2`，内置后端）：**约 420 MB/s 单核**，自检 < 1 ms。

### 3.7 API 速查

```c
FFC_CTX *ffc_new(const uint8_t key[32], int is_server); // 一个 ffrdp 上下文一个实例
void     ffc_free(FFC_CTX *c);
void     ffc_set_salt(FFC_CTX *c, int slot, int dir, const uint8_t salt[8]); // dir = FFC_TX / FFC_RX
void     ffc_crypt(FFC_CTX *c, int slot, int dir, void *buf, int len); // 加解密同一件事
void     ffc_drop (FFC_CTX *c, int slot);                              // DISCONN 时调
```

`slot` 是**你自己分配的稠密下标**（0..FFC_MAXCONN-1），**不是** `ev->conn`（见 §4.7 陷阱 1）。

---

## 4. 上层怎么写（伪代码）

下面是一个"能直接用"的骨架，逻辑与 `test.c` 一致（省略错误处理与统计）。

### 4.1 数据结构

```c
#define APP_MAXLINK  16  /* = FFC_MAXCONN，也是 ffrdp 的 FFRDP_MAXCONN */

typedef struct {
    FFC_CTX *fc;                          /* NULL = 不加密（线上明文） */
    int      used        [APP_MAXLINK];   /* 1 = 该槽在用 */
    int      hnd         [APP_MAXLINK];   /* 该槽对应的**连接句柄**（不是下标！见 §4.7） */
    uint8_t  rx_need_salt[APP_MAXLINK];   /* 该方向还没通知读流侧"要丢 8 字节" */
    uint8_t  rx_skip     [APP_MAXLINK];   /* 读流时要丢弃的字节数（= 8） */
    uint8_t  tx_salt     [APP_MAXLINK][FFC_SALT_LEN];
    uint8_t  tx_salt_left[APP_MAXLINK];   /* 盐还有几字节没发出去（>0 时禁止发业务数据） */
} APP_CRYPTO;

static APP_CRYPTO g_cr;  /* 随 cbctx 交给回调；**一个 ffrdp 上下文一份** */

/* 句柄 → 密码槽：**线性扫**（N ≤ 16，够快）。绝不能拿句柄直接当下标，见 §4.7 */
static int slot_of(const APP_CRYPTO *cr, int handle)
{
    for (int i = 0; i < APP_MAXLINK; i++)
        if (cr->used[i] && cr->hnd[i] == handle) return i;
    return -1;
}

static int slot_alloc(APP_CRYPTO *cr, int handle)
{
    for (int i = 0; i < APP_MAXLINK; i++) {
        if (!cr->used[i]) {
            cr->used[i] = 1;  cr->hnd[i] = handle;
            cr->rx_need_salt[i] = 1;  cr->rx_skip[i] = 0;  cr->tx_salt_left[i] = 0;
            return i;
        }
    }
    return -1;  /* 槽满：要么 close 这条连接，要么不加密地跑（自己决定） */
}

static void slot_free(APP_CRYPTO *cr, int s)
{
    if (s < 0) return;
    if (cr->fc) ffc_drop(cr->fc, s);  /* 清零这条连接两个方向的密钥流 */
    cr->used[s] = 0;  cr->hnd[s] = -1;
    cr->rx_need_salt[s] = 1;  cr->rx_skip[s] = 0;  cr->tx_salt_left[s] = 0;
}

static void app_crypto_init(int is_server, const uint8_t key[32])
{
    memset(&g_cr, 0, sizeof(g_cr));
    for (int i = 0; i < APP_MAXLINK; i++) { g_cr.hnd[i] = -1; g_cr.rx_need_salt[i] = 1; }
    g_cr.fc = ffc_new(key, is_server);  /* 两端角色必须相反 */
}
```

### 4.2 回调（只做变换，不做别的）

```c
static long on_event(void *cbctx, const FFRDP_EVENT *ev)
{
    APP_CRYPTO *cr = (APP_CRYPTO*)cbctx;

    if (ev->type == FFRDP_ENCRYPT) {
        if (!cr || !cr->fc) return FFRDP_NOT_CONSUMED;  /* 不加密 */
        int s = slot_of(cr, ev->conn);                  /* 句柄 → 槽，见 §4.7 */
        if (s < 0) return FFRDP_NOT_CONSUMED;
        ffc_crypt(cr->fc, s, FFC_TX, ev->buf, ev->len);
        return FFRDP_NOT_CONSUMED;  /* 返回值只对 RECVDATA 有意义 */
    }
    if (ev->type == FFRDP_DECRYPT) {
        int s = slot_of(cr, ev->conn);
        if (!cr || !cr->fc || s < 0) return FFRDP_NOT_CONSUMED;
        /* 盐记录由模块自己从流里读（原样放过）—— 这里绝不能 ffc_set_salt(RX)，那会多跳 8 字节 */
        if (cr->rx_need_salt[s]) {
            cr->rx_need_salt[s] = 0;
            cr->rx_skip[s]      = FFC_SALT_LEN;  /* 只通知读流侧：丢掉开头 8 字节 */
        }
        ffc_crypt(cr->fc, s, FFC_RX, ev->buf, ev->len);
        return FFRDP_NOT_CONSUMED;
    }
    if (ev->type == FFRDP_DISCONN) {
        slot_free(cr, slot_of(cr, ev->conn));    /* ffc_drop + 释放槽：重连必须从零开始 */
        return FFRDP_NOT_CONSUMED;
    }
    return FFRDP_NOT_CONSUMED;
}
```

### 4.3 连接建立：分配槽 + 发盐

```c
/* 服务端在 FFRDP_ACCEPT、客户端在 FFRDP_CONNECT 时调（回调里只登记 handle，槽可以在
 * 回调里分配，因为 slot_alloc 不碰 ffrdp。别在这里 send —— 回调里不能调 ffrdp_*） */
static void on_conn_up(int handle)
{
    if (!g_cr.fc) return;                /* 不加密 ⇒ 什么都不用做 */

    int s = slot_alloc(&g_cr, handle);
    if (s < 0) return;                   /* 槽满：见 slot_alloc 的注释 */

    salt_make(g_cr.tx_salt[s]);          /* 时间 ^ pid ^ 计数器，唯一即可（不需要随机） */
    ffc_set_salt(g_cr.fc, s, FFC_TX, g_cr.tx_salt[s]);
    g_cr.tx_salt_left[s] = FFC_SALT_LEN; /* 先发它，发完才允许发业务数据 */
}

/* 发送循环里，业务数据之前 */
int s = slot_of(&g_cr, handle);
if (s >= 0 && g_cr.tx_salt_left[s] > 0) {
    int off = FFC_SALT_LEN - g_cr.tx_salt_left[s]; /* 上次发到哪了（可能被 EAGAIN 截断） */
    int n   = ffrdp_send(rdp, handle, (char*)g_cr.tx_salt[s] + off, g_cr.tx_salt_left[s], 0);
    if (n > 0) g_cr.tx_salt_left[s] -= (uint8_t)n;
}
if (s < 0 || g_cr.tx_salt_left[s] == 0)            /* 盐发完了（或没加密）才发业务数据 */
    ffrdp_send(rdp, handle, data, len, 0);
```

⚠️ **盐没被接受之前一条业务数据都不许发**：否则盐就不是"流的最前面 8 字节"了。

### 4.4 接收：读流时丢掉 8 字节

```c
int n = ffrdp_recv(rdp, handle, buf, sizeof(buf), timeout_ms);
if (n > 0) {
    int s = slot_of(&g_cr, handle);
    if (g_cr.fc && s >= 0 && g_cr.rx_skip[s] > 0) {
        int d = MIN((int)g_cr.rx_skip[s], n);
        memmove(buf, buf + d, (size_t)(n - d));  /* 盐已经在 DECRYPT 里被模块用掉了 */
        g_cr.rx_skip[s] -= (uint8_t)d;
        n -= d;
    }
    if (n > 0) handle_plaintext(buf, n);
}
```

### 4.5 收尾

```c
ffrdp_exit(rdp);   /* 先停协议线程 ⇒ 之后不会再有回调 */
ffc_free(g_cr.fc); /* 再释放密码状态 */
```

### 4.6 落地检查表

1. **两端角色相反**（`ffc_new(key, is_server)`），密钥逐字节相同。
2. **`ENCRYPT`/`DECRYPT` 两个事件都处理**，且都对**整段**载荷做（不要只做块整数倍）。
3. **每条连接每个方向一个唯一盐**；服务端带 N 个客户端时这是硬性要求。
4. **盐要在业务数据之前发出**；接收侧要**丢掉**这 8 字节。
5. **`DISCONN` 里 `ffc_drop`**，否则重连即乱码。
6. **不要**在回调里调 `ffrdp_*`、不要 `printf`、不要 malloc。
7. 一个 `FFC_CTX` 只配一个 ffrdp 上下文（不要做成全局单例）。

### 4.7 陷阱：**句柄不是数组下标**

`ev->conn` 是句柄，编码是 `代号 × FFRDP_MAXCONN + 槽号`，**同一个槽被复用时代号 +1**（`ffrdp.h:23`、
`ffrdp.c:1028`）。所以：

- 第一次连接：句柄 `1`（客户端主连接恒为 `0`）
- 该槽回收后再来一个连接：句柄 `17`、`33`……

于是 `ffc_crypt(fc, ev->conn, ...)` 在句柄 ≥ `FFC_MAXCONN` 时会命中 `conn >= FFC_MAXCONN` 的越界保护，
**静默什么都不做** ⇒ 这条连接线上是明文，对端按密文解 ⇒ 从第一条数据起全乱、且没有任何报错。

正确做法：应用自己维护 `handle -> 0..N-1` 的映射（`ACCEPT`/`CONNECT` 时分配、`DISCONN` 时释放），
密码槽用映射后的下标，不要用句柄。

> `test.c` 已经按本节写成**映射**（`crypto_slot_get` / `crypto_slot_drop`，`test.c:230-265`）：
> 口令槽用 `0..15`，句柄只当键。验证方式：服务端长跑，客户端连续重连 12 次
> （句柄 `1 → 17 → 33 … → 177`），12 条连接全部 `crc_err 0 / len_err 0`。
>
> 这个坑的**实际症状**值得记住：明文跑、加密跑第一条连接都没问题，**第二条连接开始才全乱**，
> 而且服务端 `verify` 全绿（它收得没问题，只是回出去的是明文）—— 只怪在客户端一侧。
> 如果哪天看到"第一条连接好、重连后必坏"，先查这一条。
>
> 定位手法：`TEST_CRYPTO_DBG=1` 跑两端，看 `[crypto] alloc handle=N -> slot=M`。如果某个句柄
> **一次 alloc 都没有**（却在该连接上收发数据），就是这条路径漏掉了映射。

---

## 5. 取舍与风险

### 5.1 得到什么 / 失去什么

| 项目 | `ffcrypto` 方案 |
|---|---|
| 机密性 | ✅（ChaCha20 流密码） |
| 完整性 / 防篡改 | ❌ —— 协议本身也没有校验和（`ffrdp.c` 里没有 crc），要靠应用层自己（`test.c` 的消息里带 crc32） |
| 认证（对端是不是我以为的那个人） | ❌ —— PSK 相同即"是"，不做身份绑定 |
| 前向保密 | ❌ —— 长期 PSK，泄露即历史流量可解 |
| 重放防护 | ❌ —— 靠 ffrdp 的序号窗口挡住"重复帧"，但挡不住"重放整条会话" |
| 长度开销 | ✅ 0 字节 |
| CPU 开销 | ✅ 很低（~420 MB/s 单核，是 8 Mbps 摄像头的 400 倍余量） |
| 依赖 | ✅ 无（单文件，C99） |

### 5.2 two-time pad 是这里最容易犯的错

同一把 PSK + 同一段密钥流跑两条连接 ⇒ `C1 ⊕ C2 = P1 ⊕ P2`，媒体流的头是可预测明文（SPS/PPS、RTP 头），
等于白送。**所以"每条连接每个方向不同盐"不是可选优化，是正确性要求。**

### 5.3 密钥从哪来

- 命令行 `--key=<64 hex>` 会出现在 `ps`、shell history、`/proc/<pid>/cmdline` ⇒ 只适合测试。
- 环境变量 `FFRDP_KEY` 好一点（`ps` 看不到，`/proc/<pid>/environ` 只有同用户/root 能读）。
- 生产：读配置文件/密钥库/安全存储，且**不要**编译进二进制。

### 5.4 与 MTU / 分片的关系

载荷等长 ⇒ 帧大小不变 ⇒ MTU、分片、FEC 尾巴、窗口的计算全都不受影响。这是回调方案相对
"自己套一层 TLS"的最大优势（TLS 会让每 16 KB 多出 5 字节头 + 16 字节 tag，并被 ffrdp 重新切帧）。

### 5.5 出问题时的表现

流密码错一位就全错，所以**几乎所有配置错误的表现都一样**：`crc_err` 暴涨 / 花屏 / "bad length, stream lost sync"。
不会出现"加密失败"这种明确报错。排查顺序见 §7.7。

---

## 6. 方案 B：TLS 架在 ffrdp 之上（OpenSSL）

### 6.1 为什么不能塞进回调

| 原因 | 说明 |
|---|---|
| 长度会变 | TLS record 头 5 字节 + AEAD tag 16 字节，C1 不允许 |
| 握手是多轮消息 | 回调无法发帧（在里面调 `ffrdp_send` 直接自锁，C5） |
| 回调拿到的不是字节流 | 是"这一帧的载荷"；TLS 的 record 边界与 ffrdp 帧边界毫无关系 |

结论：**TLS 必须在回调之外、在应用层，包住 `ffrdp_send` / `ffrdp_recv`。** 此时
`ENCRYPT`/`DECRYPT` 两个事件**不要**处理（密码学交给 TLS）。

### 6.2 拓扑

```
应用数据
  │  SSL_write / SSL_read                ← 加解密、认证都在这里
  ▼
TLS 记录（memory BIO）
  │  BIO_read / BIO_write
  ▼
ffrdp_send / ffrdp_recv                 ← 可靠有序字节流（UDP + ARQ）
  │
  ▼
UDP
```

ffrdp 已经是**可靠有序字节流**，正好就是 TLS 需要的传输模型（TLS 不需要关心底层是 TCP 还是 ffrdp）。

### 6.3 代码骨架

```c
#include <openssl/ssl.h>
#include <openssl/err.h>

typedef struct {
    SSL *ssl;
    BIO *net; /* memory BIO 的"应用侧"：读出来 = SSL 要发的记录；写进去 = 收到的记录 */
    int  hs_done;
} TLS_CONN;

/* ── 上下文：进程里一份 ─────────────────────────────────────────── */
static SSL_CTX *tls_ctx_new(int is_server, const char *cert, const char *key, const char *ca)
{
    SSL_CTX *ctx = SSL_CTX_new(is_server ? TLS_server_method() : TLS_client_method());
    if (!ctx) return NULL;

    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);      /* 别留 SSLv3/TLS1.0 口子 */
    SSL_CTX_set_options(ctx, SSL_OP_NO_COMPRESSION);
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

    if (SSL_CTX_load_verify_locations(ctx, ca, NULL) != 1) return NULL;
    if (is_server) {
        if (SSL_CTX_use_certificate_chain_file(ctx, cert) != 1) return NULL;
        if (SSL_CTX_use_PrivateKey_file(ctx, key, SSL_FILETYPE_PEM) != 1) return NULL;
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
    } else {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);      /* 校验服务端证书 */
    }
    return ctx;
}

/* ── 每条连接一个 TLS 会话 ─────────────────────────────────────── */
static int tls_conn_new(TLS_CONN *t, SSL_CTX *ctx, int is_server, const char *sni)
{
    BIO *ssl_bio = NULL;
    if (BIO_new_bio_pair(&ssl_bio, 0, &t->net, 0) != 1) return -1; /* 0 = 用默认大小 */

    t->ssl = SSL_new(ctx);
    if (!t->ssl) { BIO_free(ssl_bio); BIO_free(t->net); return -1; }
    SSL_set_bio(t->ssl, ssl_bio, ssl_bio); /* 所有权转移 ⇒ 之后别再 BIO_free(ssl_bio) */
    if (is_server) SSL_set_accept_state(t->ssl);
    else {
        SSL_set_connect_state(t->ssl);
        SSL_set_tlsext_host_name(t->ssl, sni); /* SNI，服务端多域名时要用 */
    }
    t->hs_done = 0;
    return 0;
}

/* 出站：把 SSL 想发的记录搬进 ffrdp（**必须在应用线程、回调之外**） */
static int tls_flush_to_ffrdp(void *rdp, int handle, TLS_CONN *t)
{
    char rec[4096];
    for (;;) {
        int n = BIO_read(t->net, rec, (int)sizeof(rec));
        if (n <= 0) return 0;
        if (ffrdp_send(rdp, handle, rec, n, 0) != n) return -1;  /* 真实现里要自己缓冲剩下的 */
    }
}

/* 驱动状态机：入站字节已经 BIO_write 过；返回已解出的明文长度（0 = 还没有，-1 = 失败） */
static int tls_pump(void *rdp, int handle, TLS_CONN *t, char *out, int outlen)
{
    int got = 0;
    for (;;) {
        int n = t->hs_done ? SSL_read(t->ssl, out + got, outlen - got)
                           : SSL_do_handshake(t->ssl);
        if (n > 0 && t->hs_done) {
            got += n;
            if (got == outlen) break;
            continue;
        }
        if (!t->hs_done && n == 1) { t->hs_done = 1; continue; }

        int e = SSL_get_error(t->ssl, n);
        if (e == SSL_ERROR_WANT_READ)  { tls_flush_to_ffrdp(rdp, handle, t); return got; }
        if (e == SSL_ERROR_WANT_WRITE) { tls_flush_to_ffrdp(rdp, handle, t); continue; }
        if (e == SSL_ERROR_ZERO_RETURN) return got;         /* 对方发了 close_notify */
        ERR_print_errors_fp(stderr);
        return -1;
    }
    return got;
}
```

主循环（**收、喂、跑、发**，全部在应用线程里顺序做）：

```c
for (;;) {
    int n = ffrdp_recv(rdp, handle, buf, sizeof(buf), 20 /* ms */);
    if (n > 0 && BIO_write(t->net, buf, n) != n) { /* 失败 */ }

    int k = tls_pump(rdp, handle, t, plain, sizeof(plain));
    tls_flush_to_ffrdp(rdp, handle, t); /* 握手/读响应产生的记录 */
    if (k > 0)  handle_plaintext(plain, k);
    if (k < 0)  { ffrdp_close(rdp, handle); break; }

    /* 等 TLS 握手完成才允许写业务数据。注意 SSL_write 也可能返回 WANT_READ/WANT_WRITE
     * （key update、对端窗口满）⇒ 同样按 "flush + 下一轮重试" 处理，别当成失败 */
    if (t->hs_done && has_app_data()) {
        int w = SSL_write(t->ssl, out, outlen);
        tls_flush_to_ffrdp(rdp, handle, t);
        if (w > 0) consume_app_data(w);
    }
}
```

### 6.4 三条落地注意

1. **绝不在 ffrdp 回调里驱动 TLS**：`tls_pump` 需要 `ffrdp_send`，而回调持着 `ffrdp->lock`（C5）；
   回调里只能把字节记下来（`FFRDP_RECVDATA` 里最省事：`return FFRDP_NOT_CONSUMED` 让数据留在环里，
   出来以后再 `ffrdp_recv`）。
2. **握手完成前不要 `SSL_write` 业务数据**：`SSL_do_handshake` 返回 1 之后才算连上；
   期间产生的记录（ClientHello 等）必须靠 `tls_flush_to_ffrdp` 发出去。
3. **一个连接一个 `SSL*`，`SSL_read/SSL_write` 只在一个线程里调**（OpenSSL 对象不是线程安全的）。

### 6.5 免证书变体：TLS-PSK（与 `ffcrypto` 同样的"一把预共享密钥"体验）

```c
static unsigned int psk_client_cb(SSL *s, const char *hint, char *identity, unsigned int max_id,
                                  unsigned char *psk, unsigned int max_psk)
{
    strncpy(identity, "cam-01", max_id - 1); /* 身份串，双方约定 */
    memcpy(psk, g_psk, 32);
    return 32;
}
static unsigned int psk_server_cb(SSL *s, const char *identity,
                                  unsigned char *psk, unsigned int max_psk)
{
    if (!identity || strcmp(identity, "cam-01")) return 0; /* 0 = 拒绝 */
    memcpy(psk, g_psk, 32);
    return 32;
}

SSL_CTX_set_psk_client_callback(ctx, psk_client_cb);
SSL_CTX_set_psk_server_callback(ctx, psk_server_cb); /* TLS 1.2 */
SSL_CTX_set_cipher_list(ctx, "ECDHE-PSK-CHACHA20-POLY1305:ECDHE-PSK-AES128-GCM-SHA256");
```

- 必须是 **ECDHE-PSK**（带临时 ECDHE 才有前向保密）；纯 `PSK-AES...` 没有。
- TLS 1.3 的外部 PSK 走另一套回调（`SSL_CTX_set_psk_use_session_callback` 客户端 /
  `SSL_CTX_set_psk_find_session_callback` 服务端），要更多代码，一般直接用证书更省事。
- 有了 TLS-PSK，就可以把"PSK"这条约定从 `ffcrypto` 平移过来，代价是带宽和 CPU。

### 6.6 那 DTLS 呢

DTLS 是"跑在数据报上的 TLS"，看起来和 ffrdp（UDP）更配，但：

- ffrdp **自己已经有 ARQ/重传/乱序窗口**。DTLS 也有一套重传/防重放 ⇒ 两层重传互相打架，
  超时叠加后握手可能几十秒才成功。
- DTLS 的记录边界与 ffrdp 的帧边界无关，同样不能塞进 `ENCRYPT`/`DECRYPT`（§6.1 的三条原因都成立）。
- 想用 DTLS 的话，正确做法是**不用 ffrdp 的加密回调**，让 DTLS 直接跑在 UDP 上——那就是另一个传输方案了，
  不是"给 ffrdp 加加密"。

**所以：ffrdp 之上的推荐组合是 "ffrdp（可靠流）+ TLS（安全层）"，不是 DTLS。**

### 6.7 选哪个

| | `ffcrypto` 回调 | TLS over ffrdp |
|---|---|---|
| 安全强度 | 机密性（防随手抓包） | 机密性 + 完整性 + 认证 + PFS |
| 依赖 | 无 | OpenSSL（~3 MB） |
| 带宽开销 | 0 | ~0.2%（record 头 + tag） |
| CPU | ~420 MB/s 单核 | ~200-800 MB/s（AES-NI/ChaCha20-Poly1305，看 CPU） |
| 代码量 | ~60 行 | ~200 行 + 证书/PSK 管理 |
| 适合 | 摄像头/内网/嵌入式、只求"别被同网段看到" | 跨公网、需要防篡改/防劫持 |

---

## 7. `test.c` 的加密功能

`test.c` 是**参考实现 + 对测程序**，加密部分就是 §4 那套骨架的完整版。

### 7.1 编译

推荐用脚本（`ffcrypto.c` 与 `test.c` 一起编，库本身不含密码学）：

```bash
TOOLCHAIN_NAME=gcc ./build.sh # 产物：./test_ffrdp + out/gcc/{include,lib}
./build.sh clean              # 清掉 *.o / *.a / test_ffrdp / out/
```

手工编（等价，注意 `-DLINUX` 与 `ffcrypto.c`）：

```bash
gcc -Wall -Werror -Wextra -DLINUX -O2 -g -DFFRDP_DIAG=1 -DFFRDP_FEC_ENABLE=1 -c ffrdp.c        -o ffrdp.o
gcc -Wall -Werror -Wextra -DLINUX -O2 -g -DFFRDP_DIAG=1 -DFFRDP_FEC_ENABLE=1 -c lowlevel-udp.c -o lowlevel-udp.o
gcc -Wall -Werror -Wextra -DLINUX -O2 -g -DFFRDP_DIAG=1 -DFFRDP_FEC_ENABLE=1 -c socket.c       -o socket.o
gcc -Wall -Werror -Wextra -DLINUX -O2 -g -DFFRDP_DIAG=1 -DFFRDP_FEC_ENABLE=1 -c utils.c        -o utils.o
ar  rcs libffrdp.a ffrdp.o lowlevel-udp.o socket.o utils.o

gcc -Wall -Werror -Wextra -DLINUX -O2 -g -DFFRDP_DIAG=1 -DFFRDP_FEC_ENABLE=1 \
    test.c ffcrypto.c libffrdp.a -lpthread -rdynamic -o test_ffrdp
```

`-Werror` 是刻意的：警告当错。⚠️ 编 `test.c` 时**必须**带上 `ffcrypto.c` —— 它 `#include "ffcrypto.h"`
并调用 `ffc_new/ffc_crypt/ffc_selftest`，少一个文件就是链接期 undefined reference。
（自己写应用时当然可以不给：不挂回调就是明文。）

### 7.2 命令行与环境变量

| 参数 / 变量 | 含义 |
|---|---|
| `--key=<64 个十六进制字符>` | 32 字节 PSK；不设 ⇒ **不加密**（线上明文） |
| `FFRDP_KEY=<64 hex>` | 同上，环境变量形式；命令行给了就以命令行为准 |
| `--server[=ip:port]` | 起服务端（默认 `0.0.0.0:8000`） |
| `--client[=ip:port]` | 起客户端（默认 `127.0.0.1:8000`）；**两个可同时给**（同机自测） |
| `--recv_only` | 只收不发。⚠️ 这一侧**不发盐**（`test.c:659` 的 `!g_recv_only`） |
| `--send_only` | 只发不收 |
| `--server_max_send_size=N` / `--client_max_send_size=N` | 每条消息最大字节数（默认 16384） |
| `--send_speed=N` | 限速 KB/s（0 = 不限） |
| `--dump_ms=N` | 诊断 dump 间隔（默认 10000） |
| `TEST_CRYPTO_DBG=1` | 把加密的轨迹打到 **stderr**：句柄 → 槽的分配/释放、TX 盐、RX 第一次丢 8 字节。排查"哪一端先错位"用 |

密钥格式：64 个 hex，可带 `0x` 前缀，`:`/`-`/空格 会被跳过。

### 7.3 启动时先自检

```c
if (g_key_set) {
    int st = ffc_selftest();
    printf("encryption          : on, backend %s, selftest %s\n",
           ffc_backend(), st ? "FAILED (do not trust the numbers below)" : "ok");
    if (st) g_key_set = 0; // 自检不过就退回明文：宁可明文，也别发一堆对端解不开的字节
}
```

正常输出：

```
encryption          : on, backend builtin-chacha20, selftest ok
```

看到 `selftest FAILED` ⇒ `g_key_set` 被清 0，**程序会以明文继续跑**（不会退出）。这时候两端都"看起来正常"，
但线上是明文 —— 别拿这种一轮的结果当"加密可用"。

### 7.4 本机回环（最快验证）

```bash
# 同机自测：一个进程里同时当 server 和 client
(sleep 10; echo quit) | ./test_ffrdp \
    --server=127.0.0.1:8111 --client=127.0.0.1:8111 \
    --key=00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff
```

**必须**通过 `quit`（或 stdin EOF）退出才会打印 `verify:` 汇总；Ctrl+C 直接结束、看不到汇总。

期望输出（两端各一段）：

```
[client] verify: sent 25136 msgs / 208146659 B, recv 25043 msgs / 207418423 B | seq_gap 0 (missing 0), crc_err 0, len_err 0, partial_err 0
[client] verify: OK -- received 25043 messages, seq contiguous and crc all correct
...
[server] verify: OK -- received 25135 messages, seq contiguous and crc all correct
```

判据就一条：**`crc_err 0` + `len_err 0` + `seq_gap 0` + `verify: OK`**。
加密接错时最先炸的就是 `crc_err` / `len_err`（§7.7）。

**再加一条：重连（句柄复用）也要跑。** 单进程那一轮只会用到句柄 `0`，测不出 §4.7 那个坑。
最小的回归是"服务端长跑 + 客户端连着连断若干次"，看每个客户端的 `verify` 是否都 OK：

```bash
KEY=00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff
( sleep 40; echo quit ) | ./test_ffrdp --server=127.0.0.1:8200 --key=$KEY > /tmp/s.log 2>&1 &
sleep 1
for i in $(seq 1 12); do                      # 服务端会依次拿到句柄 1、17、33…（槽位复用 ⇒ 代号 +1）
    ( sleep 2; echo quit ) | ./test_ffrdp --client=127.0.0.1:8200 --key=$KEY > /tmp/c$i.log 2>&1
    grep -h 'client] verify:' /tmp/c$i.log | tail -1
done
wait
grep -h 'server] verify: sent' /tmp/s.log     # 每一行都该是 crc_err 0, len_err 0
```

12 条全部 `verify: OK` 才算过；任何一条 `CHECK FAILED` 都是句柄/槽的映射问题（用
`TEST_CRYPTO_DBG=1` 抓 `alloc handle=…`）。

### 7.5 真机对测（`test.sh`）

```bash
./test.sh ./test_ffrdp chenk@192.168.0.213 abc 120
```

⚠️ **`test.sh` 目前不透传密钥**：它只在 ssh 的 `env` 白名单里传 `FFRDP_HS` / `FFRDP_IDLE` /
`FFRDP_CLOSE` / `TEST_STAMP` 等（`test.sh:372`、`test.sh:403`），没有 `FFRDP_KEY`。
要么两端手工各跑一条 `test_ffrdp`，要么给这两行 `env` 列表各加 `FFRDP_KEY='${FFRDP_KEY:-}'`
（远端那一行在双引号命令串里，`${…}` 由本地展开 ⇒ 写法与现有变量一致）。

### 7.6 `test.c` 的实现要点（读代码时对着看）

| 位置 | 内容 |
|---|---|
| `test.c:180-187` | `TEST_CRYPTO`：`fc` + 句柄→槽映射表 + `rx_need_salt[]` / `rx_skip[]` |
| `test.c:193-201` | `salt_make()`：时间 ^ pid ^ 地址，xorshift 出 8 字节 |
| `test.c:203-212` | `crypto_init()`：`ffc_new(key, is_server)`，随 cbctx 传进回调 |
| `test.c:230-251` | `crypto_slot_get()`：**句柄 → 密码槽**（查不到就分配；这就是 §4.7 的映射） |
| `test.c:253-265` | `crypto_slot_drop()`：`ffc_drop` + 还槽（`DISCONN` 时调） |
| `test.c:280-289` | `crypto_rx_skip()`：取"这次要丢几字节盐"并扣减 |
| `test.c:362-378` | `FFRDP_ENCRYPT` / `FFRDP_DECRYPT` 两个分支（**都**走 `crypto_slot_get`） |
| `test.c:654-672` | 每条连接发送前 `salt_make` + `ffc_set_salt(TX)`（`TEST_CRYPTO_DBG=1` 会打盐） |
| `test.c:686-690` | 盐先发，`tx_salt_left` 归零前业务数据一条不发 |
| `test.c:776-786` | 读流时按 `rx_skip` 丢掉盐 |
| `test.c:392-396` | `DISCONN`：`crypto_slot_drop`（密钥流复位 + 还槽） |
| `test.c:1163-1168` | 启动自检，失败则退回明文 |
| `test.c:1066` | `ffrdp_exit` 之后才 `ffc_free`（此时回调不会再发生） |

### 7.7 出问题时的症状表

| 症状 | 最可能的原因 |
|---|---|
| 一端开 `--key`、另一端没开 | 对端把密文当明文 ⇒ `crc_err` / `len_err` 暴涨 |
| 两端 `--key` 不同 | 同上，且两端都报错 |
| 角色给反（两侧都传同一个 `is_server`） | 同上；上下行都解不开 |
| 盐没走"流的最前面"（先发了数据） | 从第一帧起全错 |
| 接收侧自己又 `ffc_set_salt(RX)` 并手动跳过 8 字节 | 多跳 8 字节 ⇒ 全错（`test.c` 早期版本就是这个坑） |
| 重连后立刻乱码 | `DISCONN` 里没 `ffc_drop`（密钥流偏移继续跑） |
| **第一条连接正常、第二条起全乱**，且服务端 verify 全绿 | §4.7 的"句柄当数组下标"：句柄 ≥ 16 后 `ffc_crypt` 越界、**静默不加密**（只有一侧漏了映射时就是这样） |
| 部分连接乱码、且 `TEST_CRYPTO_DBG=1` 里看不到该句柄的 `alloc` | 同上：某条路径（ENCRYPT/DECRYPT/读流）没走 `crypto_slot_get` |
| 自检 `FAILED` | 后端/编译器问题；此时程序按明文跑，别当成"加密正常" |
| 加密开了但吞吐掉一半以上 | 正常吗？先跑 `ffc_bench(1<<30)` 确认是加密慢还是链路/协议问题 |

### 7.8 想量一下加密本身的开销

`test.c` 不打印加密吞吐，自己写 10 行驱动即可：

```c
#include <stdio.h>
#include "ffcrypto.h"

int main(void)
{
    int st = ffc_selftest();
    printf("selftest : %d (0=ok)\n", st);
    printf("backend  : %s\n", ffc_backend());
    printf("bench    : %.1f MB/s\n", ffc_bench((size_t)1 << 30));   /* 1 GiB */
    return st;
}
```

```bash
gcc -O2 -Wall -Wextra -std=c99 -I. bench.c ffcrypto.c -o bench && ./bench
```

本机参考：`selftest 0` / `builtin-chacha20` / **约 420 MB/s**（x86_64，`-O2`）。
交叉编译到目标板后重跑同一个驱动，才是那块板上的真实数字。

---

## 8. FAQ

**Q：能不加密但保留"防篡改"吗？**
A：不行。回调等长 ⇒ 没地方放 MAC。要么 TLS（§6），要么放弃完整性。

**Q：为什么不用 AES-GCM？**
A：GCM 是 AEAD，带 16 字节 tag ⇒ 长度会变。若只要机密性可以用 AES-CTR（同样等长），
`FFC_BACKEND=1/2` 就是留给它的口子。

**Q：`ffc_crypt` 需要自己加锁吗？**
A：当前实现里两个回调都在 `ffrdp->lock` 内被调，天然串行；但**别依赖**这一点——
按"可能并发"设计：`TX` 只从发送线程动、`RX` 只从接收线程动，不同连接互不相干，
`ffcrypto` 的 `st[slot][dir]` 结构本来就是这么分的。

**Q：同一把 PSK 能给"所有设备"用吗？**
A：能跑，但不该：任意一台设备泄露 = 全网泄露，且不同设备之间可以互相冒充。
至少做到"一设备一密钥"，或改用 TLS + 证书。

**Q：加密了还需要应用层 crc 吗？**
A：**需要**。流密码不防篡改，而 ffrdp 协议本身没有校验和；`test.c` 每条消息尾部的 crc32
就是唯一的完整性检查（也是所有"加密接错"能被发现的原因）。

**Q：`ffc_free` 里那个 `memset` 有用吗？**
A：意图是"释放前擦掉密钥"，但在 `-O2` 下会被 dead-store elimination 删掉（实测 `ffc_free` 编成
`jmp free@PLT`）。要真擦得加编译器屏障（`__asm__ __volatile__("" : : "r"(p) : "memory")`）或
`explicit_bzero` / `SecureZeroMemory`；同时别忘了 `ffc_new` 里栈上的密钥副本。
