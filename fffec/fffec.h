#ifndef __FFFEC_H__
#define __FFFEC_H__

void* fffec_init(int fd);
void  fffec_free(void *ctxt);
int   fffec_send(void *ctxt, char *buf, int len, void *dstaddr);
int   fffec_recv(void *ctxt, char *buf, int len, void *srcaddr);
void  fffec_dump(void *ctxt);

#endif

