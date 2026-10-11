#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include "utils.h"

#ifdef LINUX
#include <sys/prctl.h>
#endif
#ifdef WIN32
extern int pthread_setname_np(pthread_t thread, const char *name);
extern int pthread_getname_np(pthread_t thread, char *name, size_t len);
#endif

uint32_t ffrdp_get_timestamp32_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint32_t ms = ((uint32_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
    return (ms += !ms);
}

int ffrdp_module_thread_get_state(MOD_THREAD *mt)
{
    if (!mt->hthread) return 0;
    else if ((mt->flags & (MOD_THREAD_FLAG_PAUSE_REQ|MOD_THREAD_FLAG_PAUSE_OK)) == (MOD_THREAD_FLAG_PAUSE_REQ|MOD_THREAD_FLAG_PAUSE_OK)) return 2;
    else return 1;
}

void ffrdp_module_thread_set_state(MOD_THREAD *mt, int state)
{
    switch (state) {
    case 0: // stop
        if (mt->hthread) {
            mt->flags |= MOD_THREAD_FLAG_STOP;
            pthread_join((pthread_t)mt->hthread, NULL);
            mt->hthread = NULL;
        }
        break;
    case 1: // start
        if (!mt->hthread) {
            mt->flags &= ~(MOD_THREAD_FLAG_STOP|MOD_THREAD_FLAG_PAUSE_REQ|MOD_THREAD_FLAG_PAUSE_OK);
            pthread_create((pthread_t*)&mt->hthread, NULL, mt->proc, mt->arg);
        } else {
            mt->flags &= ~(MOD_THREAD_FLAG_PAUSE_REQ|MOD_THREAD_FLAG_PAUSE_OK);
        }
        break;
    case 2: // pause async
    case 3: // pause sync
        if (!(mt->flags & MOD_THREAD_FLAG_PAUSE_REQ)) {
            mt->flags = (mt->flags & ~MOD_THREAD_FLAG_PAUSE_OK) | MOD_THREAD_FLAG_PAUSE_REQ;
            if (state == 3) { while (!(mt->flags & MOD_THREAD_FLAG_PAUSE_OK)) usleep(100 * 1000); }
        }
        break;
    case 4: // restart
        module_thread_set_state(mt, 0);
        module_thread_set_state(mt, 1);
        break;
    }
}

void ffrdp_module_thread_dump(MOD_THREAD *mt, char *str, int len)
{
    snprintf(str, len, "state: %d, flags: 0x%X, hthread: %p, proc: %p, arg: %p", module_thread_get_state(mt), mt->flags, mt->hthread, mt->proc, mt->arg);
}

int ffrdp_thread_setname(char *name)
{
#ifdef LINUX
    return prctl(PR_SET_NAME, (unsigned long)name);
#endif
#ifdef WIN32
    return pthread_setname_np(pthread_self(), name);
#endif
}

int ffrdp_thread_getname(char *name, int len)
{
#ifdef LINUX
    char buf[16];
    prctl(PR_GET_NAME, (unsigned long)buf);
    snprintf(name, len, "%s", buf);
    return 0;
#endif
#ifdef WIN32
    return pthread_getname_np(pthread_self(), name, len);
#endif
}
