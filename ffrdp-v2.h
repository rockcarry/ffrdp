#ifndef __FFRDPV2_H__
#define __FFRDPV2_H__

#include <stdint.h>   // uint64_t（对端地址是不透明的 64 位值 ✓）；v1 的 ffrdp.h 不用 64 位类型，所以以前没有这一行 ✓

/* ============================================================================
 * ffrdp v2 设计稿（临时文件 ✓）
 *
 * ⚠️ v2.0.0 完成后，对外**只有一个** `ffrdp.h` ✓ —— 本文件的内容会并进去，
 *    然后删除 ✓。它现在的作用是"把设计定下来"，不是发布接口 ✓。
 *
 * 相对 v1.2.0 的改动：
 *   1. 服务器支持 accept 多客户端接入 ✓
 *   2. 增加连接的三次握手与四次挥手 ✓
 *   3. 加密通过回调在外部实现 ✓
 *   4. ffrdp_send/ffrdp_recv 的返回值可判断：连接断开 / 收发字节数 / EAGAIN ✓
 *   5. ffrdp_send 保证原子性 ✓；ffrdp_recv 不保证（有序字节流 ✓）
 *
 * 实现方法：
 *   - ffrdp 内部维护一张 conn_list ✓
 *   - conn_list 存放对端 addr 和 conn_ctx（conn_ctx 对应 v1.x.x 单连接的全部功能 ✓）
 *   - conn_list 的 **index 就当作连接句柄** 使用 ✓
 *   - lowlevel 收到数据后，按对端 addr 查表决定流向哪个 conn_ctx ✓
 *   - ffrdp_send 按 conn 索引 conn_list，决定数据流去哪 ✓
 *
 * 已定的边界条件（实现必须照此 ✓）：
 *   - **未握手的 addr 送来的帧一律丢弃** ✓；服务端连接数达 maxconn ⇒ 不再 accept，
 *     所有未连接对端的包同样丢弃（并计入计数器 ✓，不静默 ✗）
 *   - 连接句柄**永不复用索引** ✓ ⇒ 陈旧句柄只会指向"已关闭"的连接 ⇒ 查表失败，
 *     返回无效句柄 ✓（代价：句柄数用满就不再接新连接 —— 第一阶段接受这个取舍 ✓）
 *   - **每连接内存第一阶段不优化** ✓（仍用固定大小缓冲 ✓，不做缩放/池化 ✗）
 *   - 加密回调**原地修改、长度不变** ✓
 *   - 加密作用在**帧**上 ⇒ 对 ffrdp_send/ffrdp_recv **完全透明** ✓（应用只看见字节流 ✓）
 *   - 协议层**不解 `addr` 的内部布局** ✓（它由传输层定义，对协议是不透明的 64 位值 ✓）
 *   - 配置字符串解析失败 ⇒ 返回 NULL ✓，**原因打在 log 里** ✓（用户从 log 判断 ✓）
 * ==========================================================================*/

#define FFRDP_VERSION "v2.0.0"

/* ── 事件类型 ─────────────────────────────────────────────────────────────
 * 每个事件都通过 FFRDP_EVENT 传给回调 ✓ —— 类型是普通枚举 ✓，连接句柄有自己的字段 ✓
 * （不再把 conn 编进 type 的高位 ✗：`0x80000000` 在 int 里是负数，判断容易踩坑 ✗）
 */
enum {
    FFRDP_ACCEPT = 0, // 新连接已建立：ev->conn = **新建**的句柄 ✓，ev->addr = 对端地址
    FFRDP_CONNECT,    // 客户端 connect 完成（握手成功 ✓）
    FFRDP_DISCONN,    // 连接断开（也可以由 send/recv 的返回值得知 ✓）
    FFRDP_ENCRYPT,    // 发送前加密这一帧的载荷：**原地修改，len 不变** ✓
    FFRDP_DECRYPT,    // 收到后解密这一帧的载荷：**原地修改，len 不变** ✓
    FFRDP_RECVDATA,   // 收到应用数据（异步通知 ✓；应用也可以不处理，留给 ffrdp_recv ✓）
};

/* 回调返回值 —— 只对 FFRDP_RECVDATA 有意义 ✓（其它事件忽略返回值 ✓）：
 *   FFRDP_CONSUMED     = 已在回调里把数据取走 ⇒ 协议不再留着 ✓
 *   FFRDP_NOT_CONSUMED = 没取 ⇒ 数据仍留在缓存里，等应用 ffrdp_recv 取 ✓
 */
#define FFRDP_CONSUMED      1
#define FFRDP_NOT_CONSUMED  0

typedef struct {
    int      type;   // 上面的 FFRDP_xxx
    int      conn;   // 连接句柄；无效时为 FFRDP_CONN_INVALID
    uint64_t addr;   // 对端地址（FFRDP_ACCEPT / FFRDP_RECVDATA 有效 ✓）—— 布局对协议不透明 ✓
    void    *buf;    // FFRDP_RECVDATA：数据；FFRDP_ENCRYPT / FFRDP_DECRYPT：待处理的载荷
    int      len;    // 数据长度
} FFRDP_EVENT;
typedef long (*FFRDP_CALLBACK)(void *cbctx, const FFRDP_EVENT *ev);

/* ── 返回码 ───────────────────────────────────────────────────────────────
 * ⚠️ 这是 **ffrdp 自己的约定，不是 socket 语义** ✓：
 *    socket 里 EAGAIN 是 "-1 + errno"、send 遇到断连是 "-1 + EPIPE"；
 *    这里把它们**编码进返回值** ✓，省掉 errno 的线程局部性，跨平台也简单 ✓。
 */
#define FFRDP_CLOSED        0    // 对端已断开（send / recv 通用 ✓）
#define FFRDP_EINVAL       (-1)  // 参数错（含**无效连接句柄** ✓）
#define FFRDP_EAGAIN       (-2)  // 暂时无数据 / 队列满且等不到空间 ✓
#define FFRDP_CONN_INVALID (-1)  // 无效连接句柄（与 FFRDP_EINVAL 同值 ✓）

/* ── 接口 ─────────────────────────────────────────────────────────────────
 * params（字符串配置 ✓）：
 *   "ffrdps://0.0.0.0:8000"                      服务器
 *   "ffrdpc://192.168.0.213:8000"                客户端
 *   "ffrdps://0.0.0.0:8000,maxconn:10"           最大连接数（默认 16）
 *   "ffrdps://0.0.0.0:8000,handshake_ms:10000,idle_ms:5000"  握手超时 / 空闲(keepalive)超时
 *     （毫秒 ✓：单位写进键名，与 ffrdp_send 的 timeout_ms 同一套风格）
 *   ⇒ 解析失败返回 NULL ✓，原因打 log ✓；成功后建议 dump 一行解析结果（便于 debug ✓）
 *
 * 两个超时是**两件事** ✓：
 *   handshake —— 等 SYN-ACK 的时限 ✓（v2 新增）
 *   idle      —— keep-alive 式判死：空闲期发 QUERY、超时即判失联 ✓
 *                （沿用 v1 的 FFRDP_QUERY_CYCLE 500ms / FFRDP_DEAD_TIMEOUT 5000ms ✓）
 */
void* ffrdp_init (void *params, FFRDP_CALLBACK callback, void *cbctx);
void  ffrdp_exit (void *ctxt);              // 关掉全部连接并释放（内部会 join 传输层线程 ✓）

/* ffrdp_send：原子 ✓ —— 返回 len = 整条被接受；0 = 连接已断（**立刻返回，不等待** ✓）；
 *             FFRDP_EAGAIN = 队列满且超时（timeout_ms=0 时立即返回 ✓）；FFRDP_EINVAL = 参数错/句柄无效 ✓
 * timeout_ms：0 = 不等待；>0 = 最多等这么多毫秒；<0 = 一直等到能被接受为止 ✓ */
int   ffrdp_send (void *ctxt, int conn, char *buf, int len, int timeout_ms);

/* ffrdp_recv：有序可靠**字节流**（无消息边界 ✓），**有数据就立刻返回** ✓，不为凑满 len 而等 ✓
 * 返回：>0 = 取到的字节数；FFRDP_CLOSED(0) = 对端已断；FFRDP_EAGAIN = 暂时没数据 ✓ */
int   ffrdp_recv (void *ctxt, int conn, char *buf, int len, int timeout_ms);

/* conn < 0 ⇒ 对**全部连接**生效 ✓（服务端做全局动作/看全局状态时用 ✓） */
void  ffrdp_flush(void *ctxt, int conn);                    // 把还没攒满的那一帧立刻发出去
void  ffrdp_dump (void *ctxt, int conn, int clearhistory);  // conn < 0 = 总状态（含连接数 + 每连接一行汇总 ✓）
                                                            // conn >= 0 = 该连接的详细状态 ✓

/* 没有单独的 ffrdp_conn_count ✓ —— 需要的连接统计（已用/上限/每连接摘要）放进 ffrdp_dump(ctxt, -1) ✓
 *
 * 服务端骨架：
 *   ffrdp_init("ffrdps://0.0.0.0:8000,maxconn:10", cb, NULL);
 *   cb: FFRDP_ACCEPT   ⇒ 记住 ev->conn，挂上应用自己的连接状态
 *       FFRDP_RECVDATA ⇒ 返回 FFRDP_NOT_CONSUMED（留给应用 ffrdp_recv ✓）或 FFRDP_CONSUMED ✓
 *       FFRDP_DISCONN  ⇒ 清掉该 conn 的应用状态
 *   ffrdp_recv(ctxt, conn, buf, sizeof buf, 1000);
 *   ffrdp_send(ctxt, conn, buf, len, 0);
 */

/* ── 握手 / 挥手（v2 新增的唯一线上协议 ✓）────────────────────────────────
 *   - 只占**一个**帧类型号：`FFRDP_FRAME_TYPE_HANDSHAKE = 35` ✓
 *     载荷第 1 字节是子类型：SYN / SYN-ACK / ACK / FIN ✓（将来要加字段只扩载荷 ✓，
 *     不用再吃类型号 ✓）
 *   - 构造方式**照抄 QUERY** ✓：一个小栈缓冲 + lowlevel->send() 直接发 ✓
 *     （不走 frame_node_new ✗ ⇒ 绕开"type > SHORT 就多 2 字节 FEC 尾巴"的规则 ✓）
 *   - **已核实现有 RX 派发**（`ffrdp.c` 的 `ffrdp_on_frame`：type ≤ 32 ⇒ 数据帧、
 *     33 ⇒ ACK、34 ⇒ QUERY、**其它一律静默忽略** ✓）⇒ 35 是安全的 ✓，
 *     而且 v1 收到 v2 的握手帧会**静默丢弃** ✓ —— 正好符合"不做互通、但能识别并丢弃" ✓
 *     （v1 不会崩 ✓，v2 客户端会握手超时 ✓）
 *   - 握手帧**不进数据 ARQ** ✓：它有自己的重传计时器（就是 handshake 超时 ✓），
 *     不需要 send_list / ack / 拥塞控制 ✓
 *   - **半开连接必须有上限** ✓：收到 SYN 就要建 conn（否则后续数据无处可去 ✗），
 *     但半开状态必须计时（handshake ✓）+ 计数封顶 ✓ ⇒ 否则伪造源地址的 SYN 能一路吃内存 ✗
 *
 * ── 已定（实现按此 ✓）────────────────────────────────────────────────────
 *   1. maxconn 默认 **16** ✓
 *   2. 加密回调放在**协议层** ✓ —— lowlevel 更简单、移植与实现量更小 ✓
 *      （密钥状态本来就是每连接的 ✓，而 lowlevel 不认识 conn ✗）
 *   3. 第一阶段加密只做**机密性** ✓（无完整性/防篡改 ✗）⇒ 文档里写明 ✓，
 *      需要完整性请在上层加 MAC ✓
 * ==========================================================================*/

#endif
