#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include "fffec.h"

#ifdef WIN32
#include <winsock2.h>
#define usleep(t) Sleep((t) / 1000)
#define get_tick_count GetTickCount
typedef   signed char    int8_t;
typedef unsigned char   uint8_t;
typedef   signed short  int16_t;
typedef unsigned short uint16_t;
typedef unsigned int   uint32_t;
typedef   signed int    int32_t;
#pragma warning(disable:4996) // disable warnings
#else
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#define SOCKET int
#define closesocket close
#define stricmp strcasecmp
#define strtok_s strtok_r
static uint32_t get_tick_count()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
#endif

#define FFFEC_MTU_SIZE     (1024 + 4) // should align to 4 bytes
#define FFFEC_REDUNDANCY    8         // should be power of 2
#define FFFEC_UDPRBUF_SIZE (64 * FFFEC_MTU_SIZE)

typedef struct {
    uint8_t  send_buf[(FFFEC_MTU_SIZE + 2) * FFFEC_REDUNDANCY];
    uint8_t  recv_buf[(FFFEC_MTU_SIZE + 2) * FFFEC_REDUNDANCY];
    uint16_t send_seq;
    uint16_t recv_seq;
    uint32_t recv_mask;
    #define FLAG_GET_SRCADDR (1 << 0)
    uint32_t flags;
    SOCKET   socket;
    struct sockaddr_in srcaddr;
    uint32_t counter_tx_frame_short;
    uint32_t counter_tx_frame_full;
    uint32_t counter_rx_frame_short;
    uint32_t counter_rx_frame_full;
    uint32_t counter_rx_fec_ok;
    uint32_t counter_rx_fec_ng;
} FFFEC;

void* fffec_init(int fd)
{
    FFFEC *fffec = (FFFEC*)calloc(1, sizeof(FFFEC));
    if (!fffec) return NULL;
    fffec->socket = fd;
    return fffec;
}

void fffec_free(void *ctxt) { if (ctxt) free(ctxt); }

static int fffec_send_frame(FFFEC *fffec, char *buf, int len, void *dstaddr)
{
    if (len < FFFEC_MTU_SIZE) {
        sendto(fffec->socket, buf, len, 0, (struct sockaddr*)dstaddr, sizeof(struct sockaddr));
        fffec->counter_tx_frame_short++;
        return 0;
    } else {
        uint8_t *pbyte = fffec->send_buf + (fffec->send_seq & (FFFEC_REDUNDANCY - 1)) * (FFFEC_MTU_SIZE + 2);
        pbyte[FFFEC_MTU_SIZE + 0] = (uint8_t)(fffec->send_seq >> 0);
        pbyte[FFFEC_MTU_SIZE + 1] = (uint8_t)(fffec->send_seq >> 8);
        memcpy(pbyte, buf, len);
        sendto(fffec->socket, pbyte, FFFEC_MTU_SIZE + 2, 0, (struct sockaddr*)dstaddr, sizeof(struct sockaddr));
        fffec->send_seq++;

        if ((fffec->send_seq & (FFFEC_REDUNDANCY - 1)) == FFFEC_REDUNDANCY - 1) {
            uint32_t *pdst, *psrc, val, i, j;
            pdst = (uint32_t*)(fffec->send_buf + (FFFEC_REDUNDANCY - 1) * (FFFEC_MTU_SIZE + 2));
            for (i=0; i<FFFEC_MTU_SIZE/4; i++) {
                psrc = (uint32_t*)fffec->send_buf + i;
                for (val=0,j=0; j<FFFEC_REDUNDANCY-1; j++) {
                    val  ^= *psrc;
                    psrc += FFFEC_MTU_SIZE / 4;
                }
                *pdst++ = val;
            }
            pbyte = fffec->send_buf + (FFFEC_REDUNDANCY - 1) * (FFFEC_MTU_SIZE + 2);
            pbyte[FFFEC_MTU_SIZE + 0] = (uint8_t)(fffec->send_seq >> 0);
            pbyte[FFFEC_MTU_SIZE + 1] = (uint8_t)(fffec->send_seq >> 8);
            sendto(fffec->socket, pbyte, FFFEC_MTU_SIZE + 2, 0, (struct sockaddr*)dstaddr, sizeof(struct sockaddr));
            fffec->send_seq++;
        }
        fffec->counter_tx_frame_full++;
        return 0;
    }
}

int fffec_send(void *ctxt, char *buf, int len, void *dstaddr)
{
    int n;
    if (!ctxt) return -1;
    while (len > 0) {
        n = len < FFFEC_MTU_SIZE ? len : FFFEC_MTU_SIZE;
        fffec_send_frame(ctxt, buf, n, dstaddr);
        len -= n; buf += n;
    }
    return 0;
}

int fffec_recv(void *ctxt, char *buf, int len, void *srcaddr)
{
    struct sockaddr_in sockaddr;
    uint32_t addrlen = sizeof(sockaddr), *pdst, *psrc, val;
    uint8_t  recvbuf[FFFEC_MTU_SIZE + 2];
    int      ret, i, j, n;

    FFFEC *fffec = (FFFEC*)ctxt;
    if (!fffec) return -1;

    if ((ret = recvfrom(fffec->socket, recvbuf, sizeof(recvbuf), 0, (struct sockaddr*)&sockaddr, &addrlen)) <= 0) return -1;
    if (!(fffec->flags & FLAG_GET_SRCADDR)) {
        memcpy(&fffec->srcaddr, &sockaddr, addrlen);
        fffec->flags |= FLAG_GET_SRCADDR;
    } else if (memcmp(&fffec->srcaddr, &sockaddr, sizeof(struct sockaddr)) != 0) return -1;

    if (ret < FFFEC_MTU_SIZE) {
        memcpy(buf, recvbuf, ret);
        memcpy(srcaddr, &sockaddr, sizeof(struct sockaddr));
        fffec->counter_rx_frame_short++;
        return ret;
    } else if (ret == FFFEC_MTU_SIZE + 2) {
        uint16_t seq = (recvbuf[FFFEC_MTU_SIZE + 1] << 8) | (recvbuf[FFFEC_MTU_SIZE + 0] << 0);
        memcpy(fffec->recv_buf + (seq & (FFFEC_REDUNDANCY - 1)) * (FFFEC_MTU_SIZE + 2), recvbuf, FFFEC_MTU_SIZE + 2);
        if ((seq & ~(FFFEC_REDUNDANCY - 1)) != (fffec->recv_seq & ~(FFFEC_REDUNDANCY - 1))) fffec->recv_mask = 0;
        fffec->recv_seq = seq;
        if ((seq & (FFFEC_REDUNDANCY - 1)) != (FFFEC_REDUNDANCY - 1)) {
            fffec->recv_mask |= 1 << (seq & (FFFEC_REDUNDANCY - 1));
            memcpy(buf, recvbuf, FFFEC_MTU_SIZE);
            memcpy(srcaddr, &sockaddr, sizeof(struct sockaddr));
            fffec->counter_rx_frame_full++;
            return FFFEC_MTU_SIZE;
        } else if ((fffec->recv_mask & ((1 << (FFFEC_REDUNDANCY - 1)) - 1)) != ((1 << (FFFEC_REDUNDANCY - 1)) - 1)) {
            for (i=0,j=0; i<FFFEC_REDUNDANCY-1 && j<=1; i++) {
                if (!(fffec->recv_mask & (1 << i))) { n = i; j++; }
            }
            if (j != 1) {
                fffec->counter_rx_fec_ng++;
                return 0;
            }

            pdst = (uint32_t*)buf;
            for (i=0; i<FFFEC_MTU_SIZE/4; i++) {
                psrc = (uint32_t*)fffec->recv_buf + i;
                for (val=0,j=0; j<FFFEC_REDUNDANCY; j++) {
                    if (j != n) val ^= *psrc;
                    psrc += FFFEC_MTU_SIZE / 4;
                }
                *pdst++ = val;
            }
            memcpy(srcaddr, &sockaddr, sizeof(struct sockaddr));
            fffec->counter_rx_frame_full++;
            fffec->counter_rx_fec_ok++;
            return FFFEC_MTU_SIZE;
        }
    }
    return -1;
}

void fffec_dump(void *ctxt)
{
    FFFEC *fffec = (FFFEC*)ctxt;
    if (!ctxt) return;
    printf("send_seq              : %d\n"  , fffec->send_seq              );
    printf("recv_seq              : %d\n"  , fffec->recv_seq              );
    printf("recv_mask             : %08x\n", fffec->recv_mask             );
    printf("counter_tx_frame_short: %u\n"  , fffec->counter_tx_frame_short);
    printf("counter_tx_frame_full : %u\n"  , fffec->counter_tx_frame_full );
    printf("counter_rx_frame_short: %u\n"  , fffec->counter_rx_frame_short);
    printf("counter_rx_frame_full : %u\n"  , fffec->counter_rx_frame_full );
    printf("counter_rx_fec_ok     : %u\n"  , fffec->counter_rx_fec_ok     );
    printf("counter_rx_fec_ng     : %u\n"  , fffec->counter_rx_fec_ng     );
}

#if 1
static int  g_exit = 0;
static char server_bind_ip[32]   = "0.0.0.0";
static char client_cnnt_ip[32]   = "127.0.0.1";
static int  server_bind_port     = 8000;
static int  client_cnnt_port     = 8000;
static int  server_max_send_size = 16 * 1024;
static int  client_max_send_size = 16 * 1024;
static pthread_mutex_t g_mutex;
static void* server_thread(void *param)
{
    struct sockaddr_in server_addr = {0};
    struct sockaddr_in client_addr = {0};
    uint8_t *sendbuf= malloc(server_max_send_size);
    uint8_t *recvbuf= malloc(client_max_send_size);
    void    *fffec  = NULL;
    uint32_t tick_start, total_bytes;
    int      size, ret, fd;
    unsigned long opt;

    server_addr.sin_family      = AF_INET;
    server_addr.sin_port        = htons(server_bind_port);
    server_addr.sin_addr.s_addr = inet_addr(server_bind_ip);
    fd = (int)socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        printf("failed to open socket !\n");
        goto done;
    }

    if (bind(fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        printf("failed to bind !\n");
        goto done;
    }

#ifdef WIN32
    opt = 1; ioctlsocket(fd, FIONBIO, &opt); // setup non-block io mode
#else
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);  // setup non-block io mode
#endif
    opt = FFFEC_UDPRBUF_SIZE; setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (char*)&opt, sizeof(int)); // setup udp recv buffer size


    if (!sendbuf || !recvbuf) {
        printf("server failed to allocate send or recv buffer !\n");
        goto done;
    }

    fffec       = fffec_init(fd);
    tick_start  = get_tick_count();
    total_bytes = 0;
    while (!g_exit) {
        size = 1 + rand() % server_max_send_size;
        *(uint32_t*)(sendbuf + 4) = get_tick_count();
        strcpy((char*)sendbuf + 8, "rockcarry server data");
        ret = fffec_send(fffec, (char*)sendbuf, size, &client_addr);
        if (ret != size) {
//          printf("server send data failed: %d\n", size);
        }

        ret = fffec_recv(fffec, (char*)recvbuf, client_max_send_size, &client_addr);
        if (ret > 0) {
//          printf("ret: %d\n", ret);
            total_bytes += ret;
            if ((int32_t)get_tick_count() - (int32_t)tick_start > 10 * 1000) {
                pthread_mutex_lock(&g_mutex);
                printf("server receive: %.2f KB/s\n", (float)total_bytes / 10240);
                fffec_dump(fffec);
                pthread_mutex_unlock(&g_mutex);
                tick_start = get_tick_count();
                total_bytes= 0;
            }
        }
        usleep(1 * 1000);
    }

done:
    fffec_free(fffec);
    free(sendbuf);
    free(recvbuf);
    if (fd > 0) closesocket(fd);
    return NULL;
}

static void* client_thread(void *param)
{
    struct sockaddr_in server_addr = {0};
    void    *fffec  = NULL;
    uint8_t *sendbuf= malloc(client_max_send_size);
    uint8_t *recvbuf= malloc(server_max_send_size);
    uint32_t tick_start, total_bytes;
    int      size, ret, fd;
    unsigned long opt;

    server_addr.sin_family      = AF_INET;
    server_addr.sin_port        = htons(client_cnnt_port);
    server_addr.sin_addr.s_addr = inet_addr(client_cnnt_ip);
    fd = (int)socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        printf("failed to open socket !\n");
        goto done;
    }

#ifdef WIN32
    opt = 1; ioctlsocket(fd, FIONBIO, &opt); // setup non-block io mode
#else
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);  // setup non-block io mode
#endif
    opt = FFFEC_UDPRBUF_SIZE; setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (char*)&opt, sizeof(int)); // setup udp recv buffer size

    if (!sendbuf || !recvbuf) {
        printf("client failed to allocate send or recv buffer !\n");
        goto done;
    }

    fffec       = fffec_init(fd);
    tick_start  = get_tick_count();
    total_bytes = 0;
    while (!g_exit) {
        size = 1 + rand() % client_max_send_size;
        size = 1028;
        *(uint32_t*)(sendbuf + 4) = get_tick_count();
        strcpy((char*)sendbuf + 8, "rockcarry client data");

        ret = fffec_send(fffec, (char*)sendbuf, size, &server_addr);
        if (ret != size) {
//          printf("client send data failed: %d\n", size);
        }

        ret = fffec_recv(fffec, (char*)recvbuf, server_max_send_size, &server_addr);
        if (ret > 0) {
//          printf("ret: %d\n", ret);
            total_bytes += ret;
            if ((int32_t)get_tick_count() - (int32_t)tick_start > 10 * 1000) {
                pthread_mutex_lock(&g_mutex);
                printf("client receive: %.2f KB/s\n", (float)total_bytes / 10240);
                fffec_dump(fffec);
                pthread_mutex_unlock(&g_mutex);
                tick_start = get_tick_count();
                total_bytes= 0;
            }
        }
        usleep(1 * 1000);
    }

done:
    free(sendbuf);
    free(recvbuf);
    fffec_free(fffec);
    return NULL;
}

int main(int argc, char *argv[])
{
#ifdef WIN32
    WSADATA wsaData;
#endif
    int server_en = 0, client_en = 0, i;
    pthread_t hserver = 0, hclient = 0;
    char *str;

#ifdef WIN32
    timeBeginPeriod(1);
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        printf("WSAStartup failed !\n");
        return NULL;
    }
#endif

    if (argc <= 1) {
        printf("fffec test program - v1.0.0\n");
        printf("usage: fffec_test --server=ip:port --client=ip:port\n\n");
        return 0;
    }

    for (i=1; i<argc; i++) {
        if (strstr(argv[i], "--server=") == argv[i]) {
            server_en = 1;
            if (strcmp(argv[i] + 9, "") != 0) {
                strncpy(server_bind_ip, argv[i] + 9, sizeof(server_bind_ip));
            }
        } else if (strstr(argv[i], "--client=") == argv[i]) {
            client_en = 1;
            if (strcmp(argv[i] + 9, "") != 0) {
                strncpy(client_cnnt_ip, argv[i] + 9, sizeof(client_cnnt_ip));
            }
        } else if (strcmp(argv[i], "--server") == 0) {
            server_en = 1;
        } else if (strcmp(argv[i], "--client") == 0) {
            client_en = 1;
        } else if (strstr(argv[i], "--server_max_send_size=") == argv[i]) {
            server_max_send_size = atoi(argv[i] + 23);
        } else if (strstr(argv[i], "--client_max_send_size=") == argv[i]) {
            client_max_send_size = atoi(argv[i] + 23);
        }
    }

    strtok_s(server_bind_ip, ":", &str); if (str && *str) server_bind_port = atoi(str);
    strtok_s(client_cnnt_ip, ":", &str); if (str && *str) client_cnnt_port = atoi(str);
    if (server_en) {
        printf("server bind ip      : %s\n", server_bind_ip      );
        printf("server bind port    : %d\n", server_bind_port    );
        printf("server_max_send_size: %d\n", server_max_send_size);
    }
    if (client_en) {
        printf("client connect ip   : %s\n", client_cnnt_ip      );
        printf("client connect port : %d\n", client_cnnt_port    );
        printf("client_max_send_size: %d\n", client_max_send_size);
    }

    pthread_mutex_init(&g_mutex, NULL);
    if (server_en) pthread_create(&hserver, NULL, server_thread, NULL);
    if (client_en) pthread_create(&hclient, NULL, client_thread, NULL);

    while (!g_exit) {
        char cmd[256];
        scanf("%256s", cmd);
        if (stricmp(cmd, "quit") == 0 || stricmp(cmd, "exit") == 0) {
            g_exit = 1;
        }
    }

    if (hserver) pthread_join(hserver, NULL);
    if (hclient) pthread_join(hclient, NULL);
    pthread_mutex_destroy(&g_mutex);
#ifdef WIN32
    WSACleanup();
    timeEndPeriod(1);
#endif
    return 0;
}
#endif