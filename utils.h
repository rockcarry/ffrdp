#ifndef _UTILS_H_
#define _UTILS_H_

#include <stdint.h>

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

#define get_timestamp32_ms      ffrdp_get_timestamp32_ms
#define module_thread_get_state ffrdp_module_thread_get_state
#define module_thread_set_state ffrdp_module_thread_set_state
#define module_thread_dump      ffrdp_module_thread_dump
#define module_thread_check_exit(mt)  ((mt)->flags & MOD_THREAD_FLAG_STOP)
#define module_thread_check_pause(mt) (((mt)->flags & MOD_THREAD_FLAG_PAUSE_REQ) ? ((mt)->flags |= MOD_THREAD_FLAG_PAUSE_OK, 1) : 0)
#define thread_setname ffrdp_thread_setname
#define thread_getname ffrdp_thread_getname

typedef struct {
    #define MOD_THREAD_FLAG_STOP      (1 << 0)
    #define MOD_THREAD_FLAG_PAUSE_REQ (1 << 1)
    #define MOD_THREAD_FLAG_PAUSE_OK  (1 << 2)
    uint32_t    flags;
    void     *hthread;
    void*      (*proc)(void *arg);
    void         *arg;
} MOD_THREAD;

uint32_t ffrdp_get_timestamp32_ms(void);

int  ffrdp_module_thread_get_state(MOD_THREAD *mt);
void ffrdp_module_thread_set_state(MOD_THREAD *mt, int state);
void ffrdp_module_thread_dump(MOD_THREAD *mt, char *str, int len);
int  ffrdp_thread_setname(char *name);
int  ffrdp_thread_getname(char *name, int len);

#if defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif
