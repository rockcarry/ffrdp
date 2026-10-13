#ifndef __FFRDP_H__
#define __FFRDP_H__


// 协议版本号：与分支一一对应（v1.0.x → "v1.0.0"，v1.1.x → "v1.1.0"，v1.2.x → "v1.2.0"）。
// dump 的第一行会打印它 —— 用来一眼确认"手上跑的是哪一版"（避免把旧二进制当新的跑）。
#define FFRDP_VERSION "v1.2.0"

// addr：一个地址字符串，取代原来的 (ip, port) 两个参数。
//   服务端（server=1）：本端 bind 的地址 —— "1.2.3.4:9000" / "1.2.3.4"（端口取默认 8000）/ 也可以
//                       自带 "bind:" 前缀（那是传输层的原样写法）；NULL ⇒ bind 0.0.0.0 的默认端口。
//   客户端（server=0）：对端地址 —— 同一套写法；不带端口取默认 8000；端口必须非 0（0 会被拒绝）。
// 解析走 utils.h 的共享约定（两条层同一份代码），addr 的布局对协议是黑盒。
void* ffrdp_init  (char *addr, char *txkey, char *rxkey, int server, int smss, int sfec);
void  ffrdp_free  (void *ctxt);

// 发送是**原子**的：返回 len = 整条被接受；返回 -1 = 一个字节都没接受（队列满、参数错、对端还没连上）。
// timeout_ms： 0 = 不等待，立即返回（旧版行为）
//             >0 = 最多等这么多毫秒，期间协议照常跑，ack 回来腾出空间就接受
//             <0 = 一直等到能被接受为止
int   ffrdp_send  (void *ctxt, char *buf, int len, int timeout_ms);

// 接收的是**有序可靠字节流**（没有消息边界）。**有数据就立刻返回**，不会为了凑满 len 而等。
// timeout_ms： 0 = 不等待（旧版行为）
//             >0 = 最多等这么多毫秒，期间协议照常跑
//             <0 = 等到有数据为止（对端失联会提前返回 0）
// 返回： >0 = 取到的字节数；0 = 没取到（超时 / 本来就没有 / 对端已失联）；-1 = 参数错
int   ffrdp_recv  (void *ctxt, char *buf, int len, int timeout_ms);

int   ffrdp_isdead(void *ctxt);
void  ffrdp_flush (void *ctxt);                       // 把还没攒满的那一帧立刻发出去
void  ffrdp_dump  (void *ctxt, int clearhistory);     // 打印诊断

// 应用不再需要周期性驱动协议：协议跑在 lowlevel 的工作线程上（收包走 RECV 回调、到期走 TIMER 回调），
// ffrdp_init 里传进去的传输层自带那个线程。应用侧唯一的等待点是 ffrdp_recv 的 timeout_ms。
// （这里曾经有一个 ffrdp_update 让应用轮询，它已经删掉了 —— 见 notes 16。）

#endif
