#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include "socket.h"

#define log_printf printf

int ffrdp_socket_init(void)
{
#ifdef WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        log_printf("<2> WSAStartup failed !\n");
        return -1;
    }
#endif
    return 0;
}

void ffrdp_socket_exit(void)
{
#ifdef WIN32
    WSACleanup();
#endif
}

void ffrdp_socket_setopt(SOCKET sock, char *opt, int val)
{
#ifdef LINUX
    struct timeval tv;
#endif
    int    ret = 0;
    // 无效句柄要分平台判 ✓：Windows 的 SOCKET 是 winsock2 的 UINT_PTR（**无符号** ✓）⇒ 只能比 INVALID_SOCKET
    //（原来写 `sock < 0` 在 mingw 下**恒假** ✗ —— 这个"守门"从来没生效过 ✗，-Wextra 的 -Wtype-limits 抓住了它 ✓）。
#ifdef WIN32
    if (sock == INVALID_SOCKET) { log_printf("<4> socket_setopt, invalid socket %d\n", (int)sock); return; }
#else
    if (sock < 0) { log_printf("<4> socket_setopt, invalid socket %d\n", sock); return; }   // 0 是合法 fd ✓
#endif
    if (strcmp(opt, "recv_timeout") == 0) {
#ifdef WIN32
        ret = setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (void*)&val, sizeof(val));
#else
        tv.tv_sec = val / 1000, tv.tv_usec = val % 1000 * 1000;
        ret = setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv , sizeof(tv));
#endif
    } else if (strcmp(opt, "send_timeout") == 0) {
#ifdef WIN32
        ret = setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (void*)&val, sizeof(val));
#else
        tv.tv_sec = val / 1000, tv.tv_usec = val % 1000 * 1000;
        ret = setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (void*)&tv , sizeof(tv));
#endif
    } else if (strcmp(opt, "nonblock_io") == 0) {
#ifdef WIN32
        u_long flag = val;
        ret = ioctlsocket(sock, FIONBIO, &flag);
#else
        u_long flag = fcntl(sock, F_GETFL, 0);
        if (val) flag |=  O_NONBLOCK;
        else     flag &= ~O_NONBLOCK;
        ret = fcntl(sock, F_SETFL, flag);
#endif
    } else if (strcmp(opt, "reuse_addr") == 0) {
        ret = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (void*)&val, sizeof(val));
#ifdef LINUX
    } else if (strcmp(opt, "reuse_port") == 0) {
        ret = setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, (void*)&val, sizeof(val));
#endif
    // Buffer sizes in bytes, the same thing SO_SNDBUF / SO_RCVBUF take; a request is only a floor, both
    // ends of the kernel are free to give more (Linux doubles it, which is what getsockopt then reports).
    } else if (strcmp(opt, "send_buffer") == 0) {
        ret = setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (void*)&val, sizeof(val));
    } else if (strcmp(opt, "recv_buffer") == 0) {
        ret = setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (void*)&val, sizeof(val));
    } else if (strcmp(opt, "nodelay") == 0) {
        ret = setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (void*)&val, sizeof(val));
    } else if (strcmp(opt, "broadcast") == 0) {
        ret = setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (void*)&val, sizeof(val));
    }
    if (ret != 0) log_printf("<4> set socket option %s failed, ret: %d\n", opt, ret);
}
