#ifndef __FFRDP_H__
#define __FFRDP_H__


// 协议版本号：与分支一一对应（v1.0.x → "v1.0.0"，v1.1.x → "v1.1.0"，v1.2.x → "v1.2.0"）。
// dump 的第一行会打印它 —— 用来一眼确认"手上跑的是哪一版"（避免把旧二进制当新的跑）。
#define FFRDP_VERSION "v1.1.0"

void* ffrdp_init  (char *ip, int port, char *txkey, char *rxkey, int server, int smss, int sfec);
void  ffrdp_free  (void *ctxt);
int   ffrdp_send  (void *ctxt, char *buf, int len);   // 原子：返回 len = 整条已接受；返回 -1 = 什么都没接受（队列满）
int   ffrdp_recv  (void *ctxt, char *buf, int len);
int   ffrdp_isdead(void *ctxt);
void  ffrdp_update(void *ctxt);
void  ffrdp_flush (void *ctxt);
void  ffrdp_dump  (void *ctxt, int clearhistory);

#endif

