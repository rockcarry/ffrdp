#ifndef __FFRDP_H__
#define __FFRDP_H__

#include <stdint.h>   // uint64_t：对端地址是不透明的 64 位值（v2 起事件里要带它）

// 协议版本号：与分支一一对应（v1.0.x → "v1.0.0"，v1.1.x → "v1.1.0"，v1.2.x → "v1.2.0"，v2 → "v2.0.0"）。
// dump 的第一行会打印它 —— 用来一眼确认"手上跑的是哪一版"（避免把旧二进制当新的跑）。
#define FFRDP_VERSION "v2.0.0"

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
//   "ffrdps://0.0.0.0:8000,handshake:10,idle:5"   握手超时 10s / 空闲(keepalive)超时 5s
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

int   ffrdp_isdead(void *ctxt);       // 诊断用：对端是否已被判定失联（v2 保留；不在设计稿的接口清单里）

#endif
