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

// ⚠️ 过渡态：协议**仍然需要被周期性驱动**，所以这一步还留着它。
// 试过"删掉它、让收发顺带驱动协议"，实测不成立：协议只在应用接收环空着时才转一圈，应用一积压就把
// 协议饿住 —— 本机回环吞吐从 55.5MB/s 掉到 11.6MB/s。删掉它的前提是"协议由别人按时驱动"，
// 也就是下一步把它交给 lowlevel 的工作线程（见 lowlevel.h 的 timer / 回调）；那一步之后这个函数删除。
void  ffrdp_update(void *ctxt);

#endif
