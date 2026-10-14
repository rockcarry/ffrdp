#ifndef __FFRDP_H__
#define __FFRDP_H__

#include <stdint.h>   // uint64_t：对端地址是不透明的 64 位值（v2 起事件里要带它）

// 协议版本号：与分支一一对应（v1.0.x → "v1.0.0"，v1.1.x → "v1.1.0"，v1.2.x → "v1.2.0"，v2 → "v2.0.0"）。
// dump 里有一行会打印它（v2 起是 `version & build` ✓）—— 用来一眼确认"手上跑的是哪一版"
// （避免把旧二进制当新的跑 ✓）。注意它不在第一行：dump 的块头与 total_send 在更前面 ✓。
#define FFRDP_VERSION "v2.0.0"

/* ── 设计要点与已定的边界（v2.0.0）────────────────────────────────────────────
 * 原设计稿 `ffrdp-v2.h` 已并入本文件并删除 ✓ —— 这里就是它的去处 ✓。
 *
 * 相对 v1.2.0 的改动：
 *   1. 服务器支持 accept 多客户端接入 ✓
 *   2. 增加连接的三次握手（收到 FIN ⇒ 断开并报 DISCONN ✓）
 *   3. 加密通过回调在外部实现 ✓（协议只把**载荷原样**交给回调 ✓）
 *   4. ffrdp_send/ffrdp_recv 的返回值可判断：连接断开 / 收发字节数 / EAGAIN ✓
 *   5. ffrdp_send 保证原子性 ✓；ffrdp_recv 不保证（有序字节流 ✓）
 *
 * 实现方法：
 *   - 内部维护一张**连接表** ✓；表项 = 对端 addr + 该连接的全部协议状态 ✓
 *   - 表的**下标就是连接句柄** ✓；lowlevel 收到数据后按对端 addr 查表分发 ✓
 *   - ffrdp_send / ffrdp_recv 用句柄索引连接表 ✓
 *
 * 已定的边界条件（实现照此 ✓）：
 *   - **未握手的 addr 送来的帧一律丢弃** ✓；服务端连接数达 maxconn ⇒ 不再 accept，
 *     未连接对端的包同样丢弃（并计入计数器 ✓，不静默 ✗）
 *   - 连接句柄**永不复用** ✓ ⇒ 陈旧句柄只会指向"已关闭"的连接 ⇒ 查表失败 ⇒ 返回无效句柄 ✓
 *     （代价：句柄用满就不再接新连接 —— 第一阶段接受这个取舍 ✓）
 *   - 每连接内存第一阶段不优化 ✓（固定大小缓冲 ✓，不做缩放 / 池化 ✗）
 *   - 加密回调：协议**原样**交付载荷 ✓、不改长度 ✓；块要求由**应用**自己覆盖块整数倍的部分 ✓，
 *     剩余字节留明文 ✓；**对称（先加密后解密能还原）是唯一原则** ✓
 *   - 协议层**不解 `addr` 的内部布局** ✓（它由传输层定义，对协议是不透明的 64 位值 ✓）
 *   - 配置字符串解析失败 ⇒ 返回 NULL ✓，**原因打进 log** ✓
 *   - 握手只占**一个**帧类型号（`FFRDP_FRAME_TYPE_HANDSHAKE = 35` ✓）；握手帧**不进数据 ARQ** ✓，
 *     它有自己的重传计时器（= handshake_ms ✓）
 *   - 半开连接有上限 ✓（handshake_ms 计时 + 计数封顶 ✓）⇒ 伪造源地址的 SYN 不能一路吃内存 ✓
 *   - 加密回调放在**协议层** ✓（密钥状态本来就是每连接的 ✓，lowlevel 不认识 conn ✗）；
 *     第一阶段只做**机密性** ✓（无完整性 / 防篡改 ✗ ⇒ 需要就自己在上层加 MAC ✓）
 * ──────────────────────────────────────────────────────────────────────────*/

// ── 事件（v2）────────────────────────────────────────────────────────────────
// 每个事件都通过 FFRDP_CALLBACK 传给应用；类型是普通枚举，连接句柄有自己的字段
// （不再把 conn 编进 type 的高位：0x80000000 在 int 里是负数，判断容易踩坑）。
enum {
    FFRDP_ACCEPT = 0, // 新连接已建立：ev->conn = 新建的句柄，ev->addr = 对端地址
    FFRDP_CONNECT,    // 客户端 connect 完成（握手成功）
    FFRDP_DISCONN,    // 连接断开（也可以由 send/recv 的返回值得知）
    FFRDP_ENCRYPT,    // 发送前加密这一帧的载荷：原地修改，len 不变
    FFRDP_DECRYPT,    // 收到后解密这一帧的载荷：原地修改，len 不变
    FFRDP_RECVDATA,   // 收到应用数据（异步通知；应用也可以不处理，留给 ffrdp_recv）
};

// 回调返回值 —— 只对 FFRDP_RECVDATA 有意义（其它事件忽略返回值）：
//   FFRDP_CONSUMED     = 已在回调里把数据取走 ⇒ 协议不再留着
//   FFRDP_NOT_CONSUMED = 没取 ⇒ 数据仍留在缓存里，等应用 ffrdp_recv 取
#define FFRDP_CONSUMED      1
#define FFRDP_NOT_CONSUMED  0

typedef struct {
    int      type;   // 上面的 FFRDP_xxx
    int      conn;   // 连接句柄；无效时为 FFRDP_CONN_INVALID
    uint64_t addr;   // 对端地址（FFRDP_ACCEPT / FFRDP_RECVDATA 有效）—— 布局对协议不透明
    void    *buf;    // FFRDP_RECVDATA：数据；FFRDP_ENCRYPT / FFRDP_DECRYPT：待处理的载荷
    int      len;    // 数据长度
} FFRDP_EVENT;
typedef long (*FFRDP_CALLBACK)(void *cbctx, const FFRDP_EVENT *ev);

// ── 返回码 ───────────────────────────────────────────────────────────────────
// 这是 ffrdp 自己的约定，不是 socket 语义：socket 里 EAGAIN 是 "-1 + errno"，
// 这里把两件事编码进返回值，省掉 errno 的线程局部性，跨平台也简单。
#define FFRDP_CLOSED        0    // 对端已断开（send / recv 通用）
#define FFRDP_EINVAL       (-1)  // 参数错（含无效连接句柄）
#define FFRDP_EAGAIN       (-2)  // 暂时无数据 / 队列满且等不到空间
#define FFRDP_CONN_INVALID (-1)  // 无效连接句柄（与 FFRDP_EINVAL 同值）

// ── 接口 ─────────────────────────────────────────────────────────────────────
// params（字符串配置）：
//   "ffrdps://0.0.0.0:8000"                       服务器
//   "ffrdpc://192.168.0.213:8000"                 客户端
//   "ffrdps://0.0.0.0:8000,maxconn:10"            最大连接数（默认 16，上限 FFRDP_MAXCONN）
//   "ffrdps://0.0.0.0:8000,handshake_ms:10000,idle_ms:5000"  握手超时 / 空闲(keepalive)超时
//        ⚠️ 时间的单位**写进键名**（毫秒 ✓）—— 与 ffrdp_send 的 timeout_ms 同一套风格，
//           不留"配置是秒、内部是毫秒"的换算点 ✗。
//   ",mss:1280,fec:0"                             本端 MSS 与 FEC 冗余（v1 的 smss / sfec 两个参数；
//                                                 v2 的 init 没有位置参数，所以进了配置串）
// 解析失败 ⇒ 返回 NULL，原因打印出来（用户从输出判断）。
void* ffrdp_init (void *params, FFRDP_CALLBACK callback, void *cbctx);
void  ffrdp_exit (void *ctxt);              // 关掉全部连接并释放（内部会 join 传输层线程）

// ffrdp_send：原子 —— 返回 len = 整条被接受；0 = 连接已断（立刻返回，不等待）；
//             FFRDP_EAGAIN = 队列满且超时（timeout_ms=0 时立即返回）；FFRDP_EINVAL = 参数错/句柄无效
// timeout_ms：0 = 不等待；>0 = 最多等这么多毫秒；<0 = 一直等到能被接受为止
int   ffrdp_send (void *ctxt, int conn, char *buf, int len, int timeout_ms);

// ffrdp_recv：有序可靠字节流（无消息边界），有数据就立刻返回，不为凑满 len 而等
// 返回：>0 = 取到的字节数；FFRDP_CLOSED(0) = 对端已断；FFRDP_EAGAIN = 暂时没数据
int   ffrdp_recv (void *ctxt, int conn, char *buf, int len, int timeout_ms);

// conn < 0 ⇒ 对全部连接生效（服务端做全局动作/看全局状态时用）
void  ffrdp_flush(void *ctxt, int conn);                    // 把还没攒满的那一帧立刻发出去
void  ffrdp_dump (void *ctxt, int conn, int clearhistory);  // conn < 0 = 总状态；conn >= 0 = 该连接的详细状态

int   ffrdp_isdead(void *ctxt, int conn);  // 诊断用：**这条连接**是否已被判定失联 ✓

/* ffrdp_close：**主动**关一条连接（v2 新增 ✓）。
 *   返回：0 = 已开始关闭 ✓；-1 = 参数错 / 句柄无效（含已经关掉的 ✓）。
 *   走 FIN 挥手：我方发 FIN ⇒ 对端回 FIN ⇒ 双方才认为断开（期间对方先发 FIN 也认 ✓）。
 *   ⚠️ `hs:0`（老路径）没有 FIN 这东西 ⇒ 只做**本地**拆除 + 报 FFRDP_DISCONN ✓
 *      （对端不会收到任何通知 ✗ —— 这是老路径的固有限制 ✓）。
 *   ⚠️ 不做半关 ✓：收到 FIN 即整条结束（与 v1 一致 ✓）。
 */
int   ffrdp_close(void *ctxt, int conn);
                                           //   1 = 失联（句柄指向已关闭的连接也算 ✓）/ 0 = 活着 / -1 = 参数错

#endif
