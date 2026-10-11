#ifndef __FFRDP_H__
#define __FFRDP_H__

void* ffrdp_init  (char *ip, int port, char *txkey, char *rxkey, int server, int smss, int sfec);
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
