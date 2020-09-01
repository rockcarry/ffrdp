#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include "ffrdp.h"

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
static uint32_t get_tick_count()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
#endif

#define MIN(a, b)         ((a) < (b) ? (a) : (b))
#define MAX(a, b)         ((a) > (b) ? (a) : (b))
#define RECV_BUFF_SIZE    (16 * 1024)
#define FFRDP_MTU_SIZE     1024
#define FFRDP_MIN_RTO      2
#define FFRDP_MAX_RTO      1000
#define FFRDP_WIN_CYCLE    100
#define FFRDP_MAX_WAITSND  256

enum {
    FFRDP_FRAME_TYPE_DATA,
    FFRDP_FRAME_TYPE_ACK ,
    FFRDP_FRAME_TYPE_WIN0,
    FFRDP_FRAME_TYPE_WIN1,
    FFRDP_FRAME_TYPE_BYE ,
};

typedef struct tagFFRDP_FRAME_NODE {
    struct tagFFRDP_FRAME_NODE *next;
    struct tagFFRDP_FRAME_NODE *prev;
    uint16_t size; // frame size
    uint8_t *data; // frame data
    #define FLAG_FIRST_SEND  (1 << 0) // after frame first send, this flag will be set
    #define FLAG_DATA_RESEND (1 << 1) // after frame resend, this flag will be set
    #define FLAG_FAST_RESEND (1 << 2) // data frame need fast resend when next update
    #define FLAG_NEED_REMOVE (1 << 3) // if this flag set, frame need remove from send queue
    uint32_t flags;        // frame flags
    uint32_t tick_send;    // frame send tick
    uint32_t tick_timeout; // frame ack timeout
} FFRDP_FRAME_NODE;

typedef struct {
    uint8_t  recv_buff[RECV_BUFF_SIZE];
    int32_t  recv_size;
    int32_t  recv_head;
    int32_t  recv_tail;
    #define FLAG_SERVER    (1 << 0)
    #define FLAG_CONNECTED (1 << 1)
    #define FLAG_BYEBYE0   (1 << 2)
    #define FLAG_BYEBYE1   (1 << 3)
    uint32_t flags;
    SOCKET   udp_fd;
    struct   sockaddr_in server_addr;
    struct   sockaddr_in client_addr;

    FFRDP_FRAME_NODE *send_list_head;
    FFRDP_FRAME_NODE *send_list_tail;
    FFRDP_FRAME_NODE *recv_list_head;
    FFRDP_FRAME_NODE *recv_list_tail;
    uint32_t send_seq;
    uint32_t recv_seq;
    uint32_t recv_win; // remote receive window
    uint32_t rttm, rtts, rttd, rto;
    uint32_t tick_query_rwin;
    uint32_t wait_snd;
} FFRDPCONTEXT;

static uint32_t ringbuf_write(uint8_t *rbuf, uint32_t maxsize, uint32_t tail, uint8_t *src, uint32_t len)
{
    uint8_t *buf1 = rbuf + tail;
    int      len1 = MIN(maxsize-tail, len);
    uint8_t *buf2 = rbuf;
    int      len2 = len  - len1;
    memcpy(buf1, src + 0   , len1);
    memcpy(buf2, src + len1, len2);
    return len2 ? len2 : tail + len1;
}

static uint32_t ringbuf_read(uint8_t *rbuf, uint32_t maxsize, uint32_t head, uint8_t *dst, uint32_t len)
{
    uint8_t *buf1 = rbuf + head;
    int      len1 = MIN(maxsize-head, len);
    uint8_t *buf2 = rbuf;
    int      len2 = len  - len1;
    if (dst) memcpy(dst + 0   , buf1, len1);
    if (dst) memcpy(dst + len1, buf2, len2);
    return len2 ? len2 : head + len1;
}

static int32_t signed_extend(uint32_t a, int size) // signed extend 24bit to 32bit
{
    return (a & (1 << (size - 1))) ? (a | ~((1 << size) - 1)) : a;
}

static int seq_distance(uint32_t seq1, uint32_t seq2) // calculate seq distance
{
    return signed_extend(seq1, 24) - signed_extend(seq2, 24);
}

static FFRDP_FRAME_NODE* frame_node_new(int size) // create a new frame node
{
    FFRDP_FRAME_NODE *node = malloc(sizeof(FFRDP_FRAME_NODE) + size);
    if (!node) return NULL;
    memset(node, 0, sizeof(FFRDP_FRAME_NODE));
    node->size = size;
    node->data = (uint8_t*)node + sizeof(FFRDP_FRAME_NODE);
    return node;
}

static void list_enqueue(FFRDP_FRAME_NODE **head, FFRDP_FRAME_NODE **tail, FFRDP_FRAME_NODE *node)
{
    FFRDP_FRAME_NODE *p;
    uint32_t seqnew, seqcur;
    int      dist;
    if (*head == NULL) {
        *head = node;
        *tail = node;
    } else {
        seqnew = *(uint32_t*)node->data >> 8;
        for (p=*tail; p; p=p->prev) {
            seqcur = *(uint32_t*)p->data >> 8;
            dist   = seq_distance(seqnew, seqcur);
            if (dist == 0) return;
            if (dist >  0) {
                if (p->next) p->next->prev = node;
                else *tail = node;
                node->next = p->next;
                node->prev = p;
                p->next    = node;
                return;
            }
        }
        node->next = *head;
        node->prev =  NULL;
        node->next->prev = node;
        *head = node;
    }
}

static void list_remove(FFRDP_FRAME_NODE **head, FFRDP_FRAME_NODE **tail, FFRDP_FRAME_NODE *node, int f)
{
    if (node->next) node->next->prev = node->prev;
    else *tail = node->prev;
    if (node->prev) node->prev->next = node->next;
    else *head = node->next;
    if (f) free(node);
}

static int list_free(FFRDP_FRAME_NODE **head, FFRDP_FRAME_NODE **tail, int n)
{
    int k = 0;
    while (*head && ++k != n) list_remove(head, tail, *head, 1);
    return k;
}

void* ffrdp_init(char *ip, int port, int server)
{
#ifdef WIN32
    WSADATA   wsaData;
    unsigned long opt;
#endif
    FFRDPCONTEXT *ffrdp = calloc(1, sizeof(FFRDPCONTEXT));
    if (!ffrdp) return NULL;

#ifdef WIN32
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        printf("WSAStartup failed !\n");
        return NULL;
    }
#endif

    ffrdp->recv_win = RECV_BUFF_SIZE;
    ffrdp->rto      = FFRDP_MIN_RTO;

    ffrdp->server_addr.sin_family      = AF_INET;
    ffrdp->server_addr.sin_port        = htons(port);
    ffrdp->server_addr.sin_addr.s_addr = inet_addr(ip);
    ffrdp->udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (ffrdp->udp_fd < 0) {
        printf("failed to open socket !\n");
        goto failed;
    }

    if (server) {
        ffrdp->flags |= FLAG_SERVER;
        if (bind(ffrdp->udp_fd, (struct sockaddr*)&ffrdp->server_addr, sizeof(ffrdp->server_addr)) == -1) {
            printf("failed to bind !\n");
            goto failed;
        }
    }

#ifdef WIN32
    opt = 1; ioctlsocket(ffrdp->udp_fd, FIONBIO, &opt); // setup non-block io mode
#else
    fcntl(ffrdp->server_fd, F_SETFL, fcntl(ffrdp->server_fd, F_GETFL, 0) | O_NONBLOCK);  // setup non-block io mode
#endif
    opt = 16 * (4 + FFRDP_MTU_SIZE); setsockopt(ffrdp->udp_fd, SOL_SOCKET, SO_RCVBUF, (char*)&opt, sizeof(int)); // setup udp recv buffer size
    return ffrdp;

failed:
    if (ffrdp->udp_fd > 0) closesocket(ffrdp->udp_fd);
    free(ffrdp);
    return NULL;
}

void ffrdp_free(void *ctxt)
{
    FFRDPCONTEXT *ffrdp = (FFRDPCONTEXT*)ctxt;
    if (!ctxt) return;
    if (ffrdp->udp_fd > 0) closesocket(ffrdp->udp_fd);
    list_free(&ffrdp->send_list_head, &ffrdp->send_list_tail, -1);
    list_free(&ffrdp->recv_list_head, &ffrdp->recv_list_tail, -1);
    free(ffrdp);
#ifdef WIN32
    WSACleanup();
#endif
}

int ffrdp_send(void *ctxt, char *buf, int len)
{
    FFRDPCONTEXT     *ffrdp = (FFRDPCONTEXT*)ctxt;
    FFRDP_FRAME_NODE *node  = NULL;
    int               n = len, size;
    if (  !ffrdp || (ffrdp->flags & FLAG_SERVER) && (ffrdp->flags & FLAG_CONNECTED) == 0
       || (len + FFRDP_MTU_SIZE - 1) / FFRDP_MTU_SIZE + ffrdp->wait_snd > FFRDP_MAX_WAITSND) {
        return -1;
    }
    while (n > 0) {
        size = MIN(FFRDP_MTU_SIZE, n);
        if (!(node = frame_node_new(size + 4))) break;
        *(uint32_t*)node->data = (FFRDP_FRAME_TYPE_DATA << 0) | ((ffrdp->send_seq & 0xFFFFFF) << 8);
        memcpy(node->data + 4, buf, size);
        list_enqueue(&ffrdp->send_list_head, &ffrdp->send_list_tail, node);
        ffrdp->send_seq++; ffrdp->send_seq &= 0xFFFFFF; ffrdp->wait_snd++;
        buf += size; n -= size;
    }
    return len - n;
}

int ffrdp_recv(void *ctxt, char *buf, int len)
{
    FFRDPCONTEXT *ffrdp = (FFRDPCONTEXT*)ctxt;
    int           ret;
    if (!ctxt) return -1;
    ret = MIN(len, ffrdp->recv_size);
    if (ret > 0) {
        ffrdp->recv_head = ringbuf_read(ffrdp->recv_buff, sizeof(ffrdp->recv_buff), ffrdp->recv_head, buf, ret);
        ffrdp->recv_size-= ret;
    }
    return ret;
}

int ffrdp_byebye(void *ctxt)
{
    FFRDPCONTEXT *ffrdp = (FFRDPCONTEXT*)ctxt;
    if (!ffrdp || (ffrdp->flags & FLAG_SERVER)) return -1;
    ffrdp->flags |= FLAG_BYEBYE0;
    return 0;
}

void ffrdp_update(void *ctxt)
{
    FFRDPCONTEXT       *ffrdp   = (FFRDPCONTEXT*)ctxt;
    FFRDP_FRAME_NODE   *node    = NULL, *p = NULL, *t = NULL;
    struct sockaddr_in *dstaddr = NULL;
    struct sockaddr_in  srcaddr;
    int32_t  seq, una, mack, size, ret, send_una, send_mack = 0, recv_una, recv_mack = 0, recv_win, dist, recv_full, maxack, i;
    uint8_t  data[8];

    if (!ctxt) return;
    dstaddr  = ffrdp->flags & FLAG_SERVER ? &ffrdp->client_addr : &ffrdp->server_addr;
    send_una = ffrdp->send_list_head ? *(uint32_t*)ffrdp->send_list_head->data >> 8 : 0;
    recv_una = ffrdp->recv_seq;

    if (ffrdp->send_list_head && ffrdp->recv_win < ffrdp->send_list_head->size) {
        if ((int32_t)get_tick_count() - (int32_t)ffrdp->tick_query_rwin > FFRDP_WIN_CYCLE) { // query remote receive window size
            data[0] = FFRDP_FRAME_TYPE_WIN0; sendto(ffrdp->udp_fd, data, 1, 0, (struct sockaddr*)dstaddr, sizeof(struct sockaddr_in));
        }
    }
    for (recv_full=0,i=0,p=ffrdp->send_list_head; i<16&&p; i++,p=p->next) {
        if (!recv_full && !(p->flags & FLAG_FIRST_SEND)) { // first send
            if (p->size - 4 <= (int)ffrdp->recv_win) {
                ret = sendto(ffrdp->udp_fd, p->data, p->size, 0, (struct sockaddr*)dstaddr, sizeof(struct sockaddr_in));
                if (ret != p->size) break;
//              printf("first send packet seq: %d\n", *(uint32_t*)p->data >> 8);
                ffrdp->recv_win -= p->size - 4;
                p->tick_send     = get_tick_count();
                p->tick_timeout  = p->tick_send + ffrdp->rto;
                p->flags        |= FLAG_FIRST_SEND;
            } else recv_full = 1;
        } else if ((p->flags & FLAG_FIRST_SEND) && ((int32_t)get_tick_count() - (int32_t)p->tick_timeout > 0 || (p->flags & FLAG_FAST_RESEND))) { // resend
            ret = sendto(ffrdp->udp_fd, p->data, p->size, 0, (struct sockaddr*)dstaddr, sizeof(struct sockaddr_in));
            if (ret != p->size) break;
//          printf("re-send packet seq: %d %d\n", *(uint32_t*)p->data >> 8, ffrdp->rto);
            if (!(p->flags & FLAG_FAST_RESEND)) {
                ffrdp->rto += ffrdp->rto / 2;
                ffrdp->rto  = MIN(FFRDP_MAX_RTO, ffrdp->rto);
            }
            p->tick_send    = get_tick_count();
            p->tick_timeout = p->tick_send + ffrdp->rto;
            p->flags       &=~FLAG_FAST_RESEND;
            p->flags       |= FLAG_DATA_RESEND;
        }
    }

    for (node=NULL;;) { // receive data
        if (!node && !(node = frame_node_new(4 + FFRDP_MTU_SIZE))) break;;
        size = sizeof(srcaddr);
        if ((ret = recvfrom(ffrdp->udp_fd, node->data, node->size, 0, (struct sockaddr*)&srcaddr, &size)) <= 0) break;
        if ((ffrdp->flags & FLAG_SERVER) && (ffrdp->flags & FLAG_CONNECTED) == 0) {
            if (ffrdp->flags & FLAG_CONNECTED) {
                if (memcmp(&srcaddr, &ffrdp->client_addr, sizeof(srcaddr)) != 0) continue;
            } else {
                ffrdp->flags |= FLAG_CONNECTED;
                memcpy(&ffrdp->client_addr, &srcaddr, sizeof(ffrdp->client_addr));
            }
        }

        switch (node->data[0]) {
        case FFRDP_FRAME_TYPE_DATA:
            seq  = *(uint32_t*)node->data >> 8;
            dist = seq_distance(seq, recv_una);
            if (dist == 0) {
                for (i=0; i<16 && (recv_mack & (1<<i)); i++);
                recv_una  += i + 1;
                recv_mack>>= i;
            } else if (dist > 0 && dist <= 16) {
                recv_mack |= (1 << (dist - 1));
            }
            if (dist >= 0 && dist <= 16) {
                node->size = ret; list_enqueue(&ffrdp->recv_list_head, &ffrdp->recv_list_tail, node); node = NULL;
            }
            break;
        case FFRDP_FRAME_TYPE_ACK:
            una  = *(uint32_t*)(node->data + 0) >> 8;
            mack = *(uint32_t*)(node->data + 4) & 0xFFFF;
            dist = seq_distance(una, send_una);
            if (dist == 0) {
                send_mack|= mack;
            } if (dist > 0) {
                send_una  = una;
                send_mack = mack | (send_mack >> dist);
                recv_win  = *(uint32_t*)(node->data + 4) >> 16;
            }
            break;
        case FFRDP_FRAME_TYPE_WIN0:
            size    = sizeof(ffrdp->recv_buff) - ffrdp->recv_size;
            data[0] = FFRDP_FRAME_TYPE_WIN1;
            data[1] = (size >> 0) & 0xFF;
            data[2] = (size >> 8) & 0xFF;
            sendto(ffrdp->udp_fd, data, 3, 0, (struct sockaddr*)dstaddr, sizeof(struct sockaddr_in));
            break;
        case FFRDP_FRAME_TYPE_WIN1:
            ffrdp->recv_win = (data[1] << 0) | (data[2] << 8); ffrdp->tick_query_rwin = get_tick_count();
            break;
        case FFRDP_FRAME_TYPE_BYE:
            if (ffrdp->flags & FLAG_SERVER) {
                ffrdp->flags &= ~FLAG_CONNECTED;
                data[0] = FFRDP_FRAME_TYPE_BYE; sendto(ffrdp->udp_fd, data, 1, 0, (struct sockaddr*)dstaddr, sizeof(struct sockaddr_in));
            } else {
                ffrdp->flags |= FLAG_BYEBYE1;
            }
            break;
        }
    }
    if (node) free(node);

    if ((ffrdp->flags & FLAG_SERVER) == 0 && (ffrdp->flags & FLAG_BYEBYE0) && (ffrdp->flags & FLAG_BYEBYE1) == 0) {
        data[0] = FFRDP_FRAME_TYPE_BYE; sendto(ffrdp->udp_fd, data, 1, 0, (struct sockaddr*)dstaddr, sizeof(struct sockaddr_in));
    }

    if (seq_distance(recv_una, ffrdp->recv_seq) > 0 || recv_mack) { // got data frame
        while (ffrdp->recv_list_head) {
            seq  = *(uint32_t*)ffrdp->recv_list_head->data >> 8;
            dist = seq_distance(seq, ffrdp->recv_seq);
            if (dist == 0 && ffrdp->recv_list_head->size - 4 <= (int)(sizeof(ffrdp->recv_buff) - ffrdp->recv_size)) {
                ffrdp->recv_tail = ringbuf_write(ffrdp->recv_buff, sizeof(ffrdp->recv_buff), ffrdp->recv_tail, ffrdp->recv_list_head->data + 4, ffrdp->recv_list_head->size - 4);
                ffrdp->recv_size+= ffrdp->recv_list_head->size - 4;
                ffrdp->recv_seq++; ffrdp->recv_seq &= 0xFFFFFF;
                list_remove(&ffrdp->recv_list_head, &ffrdp->recv_list_tail, ffrdp->recv_list_head, 1);
            } else break;
        }
        *(uint32_t*)(data + 0) = (FFRDP_FRAME_TYPE_ACK << 0) | (recv_una << 8);
        *(uint32_t*)(data + 4) = (recv_mack << 0) | ((sizeof(ffrdp->recv_buff) - ffrdp->recv_size) << 16);
        sendto(ffrdp->udp_fd, data, sizeof(data), 0, (struct sockaddr*)dstaddr, sizeof(struct sockaddr_in)); // send ack frame
    }

    if (ffrdp->send_list_head && seq_distance(send_una, *(uint32_t*)ffrdp->send_list_head->data >> 8) > 0) { // got ack frame
        ffrdp->recv_win = recv_win; ffrdp->tick_query_rwin = get_tick_count(); // update rx recv window size
        for (p=ffrdp->send_list_head; p;) {
            seq = *(uint32_t*)p->data >> 8;
            dist= seq_distance(send_una, seq);
            for (i=15; i>=0 && !(send_mack&(1<<i)); i--);
            if (i < 0) maxack = (send_una - 1) & 0xFFFFFF;
            else maxack = (send_una + i + 1) & 0xFFFFFF;

            if (dist < -16) {
                break;
            } else if (dist > 0 || dist < 0 && (send_mack & (1 << (-dist-1)))) {
                p->flags |= FLAG_NEED_REMOVE;
            } else if (seq_distance(maxack, seq) >= 0) {
                p->flags |= FLAG_FAST_RESEND;
            }

            if ((p->flags & FLAG_FIRST_SEND) && (p->flags & FLAG_DATA_RESEND) == 0) {
                ffrdp->rttm = (int32_t)get_tick_count() - (int32_t)p->tick_send;
                if (ffrdp->rtts == 0) {
                    ffrdp->rtts = ffrdp->rttm;
                    ffrdp->rttd = ffrdp->rttm / 2;
                } else {
                    ffrdp->rtts = (7 * ffrdp->rtts + 1 * ffrdp->rttm) / 8;
                    ffrdp->rttd = (3 * ffrdp->rttd + 1 * (ffrdp->rttm - ffrdp->rtts)) / 4;
                }
                ffrdp->rto = ffrdp->rtts + 4 * ffrdp->rttd;
                ffrdp->rto = MAX(FFRDP_MIN_RTO, ffrdp->rto);
                ffrdp->rto = MIN(FFRDP_MAX_RTO, ffrdp->rto);
            }
            if (p->flags & FLAG_NEED_REMOVE) {
                t = p; p = p->next; list_remove(&ffrdp->send_list_head, &ffrdp->send_list_tail, t, 1);
                ffrdp->wait_snd--; continue;
            }
            p = p->next;
        }
    }
}

#if 1
static g_exit = 0;

static void* server_thread(void *param)
{
    void *ffrdp = ffrdp_init("0.0.0.0", 8002, 1);
    uint8_t buffer[8*1024];
    int     ret;

    while (!g_exit) {
        ret = ffrdp_recv(ffrdp, buffer, sizeof(buffer));
        if (ret > 0) printf("ret: %d\n", ret);

        ffrdp_update(ffrdp);
        usleep(10 * 1000);
    }
    ffrdp_free(ffrdp);
    return NULL;
}

static void* client_thread(void *param)
{
    void *ffrdp = ffrdp_init("192.168.0.148", 8002, 0);
    uint8_t sendbuf[8*1024];
    int     size, ret;

    while (!g_exit) {
        size = 1 + rand() & 0x1FFF;
        *(uint32_t*)(sendbuf + 4) = get_tick_count();
        strcpy(sendbuf + 8, "rock");
        ret = ffrdp_send(ffrdp, sendbuf, size);
        if (ret != size) {
            printf("client send data failed: %d\n", size);
        }

        ffrdp_update(ffrdp);
        usleep(10 * 1000);
    }
    ffrdp_free(ffrdp);
    return NULL;
}

int main(void)
{
//  pthread_t hserver;
    pthread_t hclient;

//  pthread_create(&hserver, NULL, server_thread, NULL);
    pthread_create(&hclient, NULL, client_thread, NULL);

    while (!g_exit) {
        char cmd[256];
        scanf("%256s", cmd);
        if (stricmp(cmd, "quit") == 0 || stricmp(cmd, "exit") == 0) {
            g_exit = 1;
        }
    }

//  pthread_join(hserver, NULL);
    pthread_join(hclient, NULL);
    return 0;
}
#endif
