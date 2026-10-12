#ifndef __FFP2P_SOCKET_H__
#define __FFP2P_SOCKET_H__

#ifdef WIN32
#include <winsock2.h>
#define socklen_t int32_t
#undef  EWOULDBLOCK
#define EWOULDBLOCK WSAEWOULDBLOCK
#undef  EINPROGRESS
#define EINPROGRESS WSAEINPROGRESS
#define socket_errno() WSAGetLastError()
#else
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <net/if_arp.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#define SOCKET int
#define closesocket close
#define socket_errno() errno
#endif

#define socket_init   ffrdp_socket_init
#define socket_exit   ffrdp_socket_exit
#define socket_setopt ffrdp_socket_setopt

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

int  ffrdp_socket_init(void);
void ffrdp_socket_exit(void);
// 可选键（未列出的键会被忽略，且只在设置失败时打印一行）：
//   recv_timeout / send_timeout（毫秒）、nonblock_io、reuse_addr、reuse_port（仅 LINUX）、
//   send_buffer / recv_buffer（字节，SO_SNDBUF / SO_RCVBUF）、nodelay、broadcast
void ffrdp_socket_setopt(SOCKET sock, char *opt, int val); // set socket option

#if defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif
