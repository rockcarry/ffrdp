#ifndef __FFRDP_H__
#define __FFRDP_H__

#ifdef __cplusplus
extern "C" {
#endif

void* ffrdp_init     (const char *ip, int port, const char *txkey, const char *rxkey, int server, int smss, int sfec);
int   ffrdp_getport  (void *ctxt);
void  ffrdp_free     (void *ctxt);
int   ffrdp_send     (void *ctxt, const char *buf, int len);
int   ffrdp_recv     (void *ctxt, char *buf, int len);
int   ffrdp_peeksize (void *ctxt);
int   ffrdp_isdead   (void *ctxt);
void  ffrdp_update   (void *ctxt);
void  ffrdp_flush    (void *ctxt);
void  ffrdp_dump     (void *ctxt, int clearhistory);

#ifdef __cplusplus
}
#endif

#endif

