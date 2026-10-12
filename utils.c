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

uint64_t ffrdp_addr_make(uint32_t ip, uint16_t port)
{
    return ((uint64_t)ip << 16) | (uint64_t)port;
}

void ffrdp_addr_split(uint64_t addr, uint32_t *ip, uint16_t *port)
{
    if (ip)   *ip   = (uint32_t)(addr >> 16);
    if (port) *port = (uint16_t)(addr & 0xFFFF);
}

// 严格的四段点分十进制。手写而不走 inet_addr：协议层不该为了这一步去引 socket 头。
static int ffrdp_addr_ipv4_parse(const char *text, int len, uint32_t *ip)
{
    uint32_t v = 0;
    int      i, part = 0, val = 0, digits = 0;

    for (i = 0; i < len; i++) {
        char c = text[i];
        if (c >= '0' && c <= '9') {
            if (++digits > 3) return -1;
            val = val * 10 + (c - '0');
            if (val > 255) return -1;
        } else if (c == '.') {
            if (digits == 0 || part >= 3) return -1;
            v = (v << 8) | (uint32_t)val;
            part++; val = 0; digits = 0;
        } else {
            return -1;
        }
    }
    if (digits == 0 || part != 3) return -1;
    *ip = (v << 8) | (uint32_t)val;
    return 0;
}

int ffrdp_addr_parse_text(char *text, uint32_t *ip, int *port, int default_port)
{
    char    *colon;
    uint32_t vip = 0;
    int      i, len, p = 0, vport = 0;

    if (!text || !ip || !port) return -1;
    len = (int)strlen(text);
    if (len <= 0 || len >= 64) return -1;

    colon = strrchr(text, ':');
    if (colon) {
        if ((int)(colon - text) <= 0) return -1;                       // ":port"：没有地址
        if (ffrdp_addr_ipv4_parse(text, (int)(colon - text), &vip) != 0) return -1;
        for (i = 1; colon[i]; i++) {
            if (colon[i] < '0' || colon[i] > '9') return -1;
            p++;
        }
        if (p == 0 || p > 5) return -1;                                // 空端口 / 超过 5 位
        vport = atoi(colon + 1);
    } else {
        if (default_port < 0) return -1;                               // 端口必须显式给
        if (ffrdp_addr_ipv4_parse(text, len, &vip) != 0) return -1;
        vport = default_port;
    }
    if (vport < 0 || vport > 65535) return -1;
    *ip   = vip;          // 只有全部通过才动输出：调用方不会把旧值误当新值
    *port = vport;
    return 0;
}
