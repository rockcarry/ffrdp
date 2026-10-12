#ifndef _UTILS_H_
#define _UTILS_H_

#include <stdint.h>

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

#ifdef WIN32
#include <windows.h>
#define get_timestamp32_ms GetTickCount64
#else
#define get_timestamp32_ms ffrdp_get_timestamp32_ms
#endif

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

// ---- 地址约定（协议层与传输层共用，就这一份）----
// 一个 uint64_t 表示一个对端：IPv4 地址在高 32 位、端口在低 16 位，**都是主机序**
// （所以 127.0.0.1 打印出来就是 0x7F000001，而不是小端机上 s_addr 那个字节序颠倒的数）。
// 这个布局以前只有 lowlevel-udp.c 知道；现在放在这里 —— 它不是某个后端的实现细节，
// 而是两条层共同的约定（协议层要能给还没开口的对端起名，传输层要能把它交给 send()）。
#define FFRDP_ADDR_DEFAULT_PORT 8000

uint64_t ffrdp_addr_make(uint32_t ip, uint16_t port);
void     ffrdp_addr_split(uint64_t addr, uint32_t *ip, uint16_t *port);
// 解析 "ip[:port]"：省略端口时用 default_port（**< 0 表示端口必须显式给**）。
// 成功时 *ip / *port 是主机序；端口只做 0..65535 的范围检查 —— "对端必须给真端口"这类
// 策略由调用方决定（协议层拒绝 0，bind 允许 0 = 由内核选）。
// 地址部分要求严格的四段点分十进制（不引 socket 头，所以协议层也能用）。
int ffrdp_addr_parse_text(char *text, uint32_t *ip, int *port, int default_port);

#if defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif
