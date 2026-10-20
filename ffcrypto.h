/* ffcrypto.h —— 给 ffrdp 的 ENCRYPT / DECRYPT 回调用的对称加密模块（零依赖、单实例一个上下文）
 *
 * 它解决什么：ffrdp 只把**帧载荷**原样交给回调（原地改、长度不变，见 ffrdp.h 的加密约定），
 * 所以算法/密钥全在应用侧。本模块就是那份"应用侧"：一条连接、一个方向一条连续的密钥流
 * （ChaCha20），跨帧连续 ⇒ 与 ffrdp 的分帧、重传、乱序、FEC 全都无关。
 *
 * 安全边界（**必须照抄进产品文档**）：
 *   · 只做**机密性**：没有 MAC/tag ⇒ 不防篡改、不防注入、不防重放 ✗
 *   · 密钥是**双方约定的预共享密钥**（PSK）：没有协商、没有前向保密 ✗
 *     ⇒ 适合"同一 WiFi 局域网内防止随手抓包"，不适合对抗有动机的攻击者。要后者请用 TLS。
 *   · 多条连接**必须**各给一个不同的 salt（ffc_set_salt）：同一把 key + 同一段密钥流跑
 *     两条连接 = two-time pad（媒体流的头是可预测明文 ⇒ 可被已知明文攻击）。
 *
 * 用法：一个 ffrdp 上下文配一个 FFC_CTX（本模块**没有全局状态**，多实例互不干扰）。
 *     FFC_CTX *fc = ffc_new(key32, is_server);        // 每端给相反的角色
 *     ffc_crypt(fc, ev->conn, FFC_TX, ev->buf, ev->len);   // ENCRYPT 回调
 *     ffc_crypt(fc, ev->conn, FFC_RX, ev->buf, ev->len);   // DECRYPT 回调
 *     ffc_set_salt(fc, ev->conn, FFC_RX, salt8);           // 收到对端的盐（首帧，见下）
 *     ffc_drop(fc, ev->conn);                              // DISCONN 回调
 *     ffc_free(fc);                                        // 收尾
 *
 * 盐怎么传（应用层约定，8 字节**明文**，盐不需要保密、只需唯一）：
 *     发送方每个方向只发一次：`ffc_set_salt(fc, conn, FFC_TX, salt)` 然后把这 8 字节当作该方向
 *     的**第一批数据**发出去（模块会原样放过它们，不解密），之后的数据照常走 ffc_crypt(TX)。
 *     接收方**什么都不用做**：模块在第一次 ffc_crypt(RX) 时自己把这 8 字节读成盐，对**之后的
 *     字节**解密；应用只需在读流时**丢掉这 8 字节**（模块不能改长度）。
 *     没调 ffc_set_salt ⇒ 零盐、且**没有**这 8 字节记录（两端都不调才一致）。
 *     ⚠️ 单连接可以省掉这一步；一个服务端带多个客户端必须做，否则是 two-time pad。
 *
 * 关键约定（**别改 ffrdp.c 的这三处调用点**，否则页码会错位）：
 *   ffrdp.c 在"帧定稿时"对每帧恰好调用一次 ENCRYPT（顺序 = 发送流顺序），
 *   在"按序交付时"对每帧恰好调用一次 DECRYPT。本模块的密钥流就靠这两条对齐。
 *
 * 线程：回调在 ffrdp 的协议锁里被调用 ⇒ 本模块不需要自己加锁，也不能在回调里
 *       回调任何 ffrdp_* 接口（会自锁）。一个实例只给一个 ffrdp 上下文用。
 */
#ifndef FFCRYPTO_H
#define FFCRYPTO_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FFC_KEY_LEN   32          /* 预共享密钥长度（ChaCha20 / AES-256）*/
#define FFC_SALT_LEN  8           /* 每条连接每个方向的盐 */

/* 每条连接 × 2 方向的状态数（约 4 KB/实例）。与 ffrdp 的 FFRDP_MAXCONN 对齐即可。*/
#ifndef FFC_MAXCONN
#define FFC_MAXCONN   16
#endif

enum { FFC_TX = 0, FFC_RX = 1 };

typedef struct ffc_ctx FFC_CTX;   /* 不透明：内部布局只在 ffcrypto.c 里 */

/* ── 生命周期：一个 ffrdp 上下文一个实例 ──────────────────────────────────── */
/* is_server 只决定"哪个方向的密钥是哪一个"：客户端 TX=key_c2s，服务端 TX=key_s2c。
 * 两端必须给**相反**的角色；给反了不会崩，但解出来全是乱码（没有任何报错）。
 * 失败返回 NULL（内存不足 / key 为 NULL）。*/
FFC_CTX *ffc_new (const uint8_t key[FFC_KEY_LEN], int is_server);
void     ffc_free(FFC_CTX *c);

/* ── 每连接 ───────────────────────────────────────────────────────────────── */
/* 盐：调用即表示"这条流最前面有 8 字节明文盐记录"（见文件头）。发送侧在第一次 ffc_crypt()
 * 之前调，接收侧不用调（模块自己从流里读）。不设 = 全零盐且没有记录（单连接可以；
 * 多连接必须设，否则 two-time pad）。*/
void ffc_set_salt(FFC_CTX *c, int conn, int dir, const uint8_t salt[FFC_SALT_LEN]);

/* 加解密同一件事：原地、等长、跨调用连续。c 为 NULL / conn 越界 / len<=0 都是空转。*/
void ffc_crypt(FFC_CTX *c, int conn, int dir, void *buf, int len);

/* 连接断开：清掉这条连接两个方向的流状态（同一句柄重连也会从零开始）*/
void ffc_drop(FFC_CTX *c, int conn);

/* ── 测试 / 调试（产品路径不需要，但建议启动时跑一次 selftest）──────────────── */
/* 自检：RFC 8439 官方向量 + 端到端是否解得开 + 上下行/多连接隔离 + 两条出口等价。
 * 0 = 全部通过。不依赖任何已创建的实例（成本 <1 ms）。*/
int  ffc_selftest(void);

/* 吞吐：对 bytes 字节做一次加密，返回 MB/s（确认本机性能用）*/
double ffc_bench(size_t bytes);

/* 当前编译进来的后端名（"builtin-chacha20" / "openssl" / "mbedtls"）*/
const char *ffc_backend(void);

#ifdef __cplusplus
}
#endif
#endif /* FFCRYPTO_H */
