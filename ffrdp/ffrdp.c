#include <stdlib.h>
#include <stdio.h>
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

#define SEND_BUFF_SIZE  (16 * 1024)
#define RECV_BUFF_SIZE  (16 * 1024)
#define FFRDP_MTU_SIZE   1024
#define FFRDP_MIN_RTO    10
#define FFRDP_WIN_CYCLE  100

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
    uint16_t size;
    uint8_t *data;
    #define FLAG_FIRST_SEND  (1 << 0)
    #define FLAG_GET_RXACK   (1 << 1)
    #define FLAG_RTO_RESEND  (1 << 2)
    #define FLAG_FAST_RESEND (1 << 3)
    uint32_t flags;
    uint32_t tick_send;
} FFRDP_FRAME_NODE;

typedef struct {
    uint8_t  send_buff[SEND_BUFF_SIZE];
    int32_t  send_size;
    int32_t  send_head;
    int32_t  send_tail;
    uint8_t  recv_buff[SEND_BUFF_SIZE];
    int32_t  recv_size;
    int32_t  recv_head;
    int32_t  recv_tail;
    SOCKET   udp_fd;
    #define FLAG_SERVER    (1 << 0)
    #define FLAG_CONNECTED (1 << 1)
    #define FLAG_BYEBYE0   (1 << 2)
    #define FLAG_BYEBYE1   (1 << 3)
    uint32_t flags;
    struct   sockaddr_in server_addr;
    struct   sockaddr_in client_addr;

    FFRDP_FRAME_NODE send_list_head;
    FFRDP_FRAME_NODE send_list_tail;
    FFRDP_FRAME_NODE recv_list_head;
    FFRDP_FRAME_NODE recv_list_tail;
    uint32_t send_seq;
    uint32_t recv_seq;
    uint32_t recv_win;
    uint32_t rttm, rtts, rttd, rto;
    uint32_t tick_query_rwin;
} FFRDPCONTEXT;

static uint32_t ringbuf_write(uint8_t *rbuf, uint32_t maxsize, uint32_t tail, uint8_t *src, uint32_t len)
{
    uint8_t *buf1 = rbuf    + tail;
    int      len1 = maxsize - tail < len ? maxsize - tail : len;
    uint8_t *buf2 = rbuf;
    int      len2 = len - len1;
    memcpy(buf1, src + 0   , len1);
    memcpy(buf2, src + len1, len2);
    return len2 ? len2 : tail + len1;
}

static uint32_t ringbuf_read(uint8_t *rbuf, uint32_t maxsize, uint32_t head, uint8_t *dst, uint32_t len)
{
    uint8_t *buf1 = rbuf    + head;
    int      len1 = maxsize - head < len ? maxsize - head : len;
    uint8_t *buf2 = rbuf;
    int      len2 = len - len1;
    if (dst) memcpy(dst + 0   , buf1, len1);
    if (dst) memcpy(dst + len1, buf2, len2);
    return len2 ? len2 : head + len1;
}

static FFRDP_FRAME_NODE* frame_node_new(int size)
{
    FFRDP_FRAME_NODE *node = malloc(sizeof(FFRDP_FRAME_NODE) + size);
    if (!node) return NULL;
    node->next = node->prev = NULL;
    node->size = size;
    node->data = (uint8_t*)node + sizeof(FFRDP_FRAME_NODE);
    node->flags= 0;
    return node;
}

static void list_add(FFRDP_FRAME_NODE *list, FFRDP_FRAME_NODE *node)
{
    if (list->next) list->next->prev = node;
    node->next = list->next;
    node->prev = list;
    list->next = node;
}

static void list_del(FFRDP_FRAME_NODE *node, int f)
{
    if (node->next) node->next->prev = node->prev;
    if (node->prev) node->prev->next = node->next;
    if (f) free(node);
}

static int32_t signed_extend(uint32_t a, int size)
{
    return (a & (1 << (size - 1))) ? (a | ~((1 << size) - 1)) : a;
}

static int seq_distance(uint32_t seq1, uint32_t seq2)
{
    return signed_extend(seq1, 24) - signed_extend(seq2, 24);
}

void* ffrdp_init(char *ip, int port, int server)
{
#ifdef WIN32
    unsigned long opt;
#endif
    FFRDPCONTEXT *ffrdp = calloc(1, sizeof(FFRDPCONTEXT));
    if (!ffrdp) return NULL;

    ffrdp->send_list_head.next = &ffrdp->send_list_tail;
    ffrdp->send_list_tail.prev = &ffrdp->send_list_head;
    ffrdp->recv_list_head.next = &ffrdp->recv_list_tail;
    ffrdp->recv_list_tail.prev = &ffrdp->recv_list_head;
    ffrdp->recv_win            = RECV_BUFF_SIZE;

    ffrdp->server_addr.sin_family      = AF_INET;
    ffrdp->server_addr.sin_port        = htons(port);
    ffrdp->server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
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
    return ffrdp;

failed:
    if (ffrdp->udp_fd > 0) closesocket(ffrdp->udp_fd);
    free(ffrdp);
    return NULL;
}

int ffrdp_send(void *ctxt, char *buf, int len)
{
    FFRDPCONTEXT *ffrdp = (FFRDPCONTEXT*)ctxt;
    int           n;
    if (!ffrdp || (ffrdp->flags & FLAG_SERVER) && (ffrdp->flags & FLAG_CONNECTED) == 0) return -1;
    n = len < (int)sizeof(ffrdp->send_buff) - ffrdp->send_size ? len : (int)sizeof(ffrdp->send_buff) - ffrdp->send_size;
    if (n > 0) ffrdp->send_tail = ringbuf_write(ffrdp->send_buff, sizeof(ffrdp->send_buff), ffrdp->send_tail, buf, n);
    return n;
}

int ffrdp_recv(void *ctxt, char *buf, int len)
{
    FFRDPCONTEXT *ffrdp = (FFRDPCONTEXT*)ctxt;
    int           n;
    if (!ctxt) return -1;
    n = len < ffrdp->recv_size ? len : ffrdp->recv_size;
    if (n > 0) ffrdp->recv_head = ringbuf_read(ffrdp->recv_buff, sizeof(ffrdp->recv_buff), ffrdp->recv_head, buf, n);
    return n;
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
    int      seq, size, ret, fdat, fack, una, mack, win, dist, i;
    uint8_t  data[8];
    uint32_t maxackseq, tick;

    if (!ctxt) return;
    dstaddr = ffrdp->flags & FLAG_SERVER ? &ffrdp->client_addr : &ffrdp->server_addr;
    una     = ffrdp->recv_seq;

    while (ffrdp->send_size > 0) {
        seq  = ffrdp->send_seq++ & 0xFFFFFF;
        size = 4 + (ffrdp->send_size < FFRDP_MTU_SIZE ? ffrdp->send_size : FFRDP_MTU_SIZE);
        node = frame_node_new(size);
        ffrdp->send_head = ringbuf_read(ffrdp->send_buff, sizeof(ffrdp->send_buff), ffrdp->send_head, node->data + 4, size);
        *(uint32_t*)node->data = (FFRDP_FRAME_TYPE_DATA << 0) | ((seq & 0xFFFFFF) << 8);
        list_add(&ffrdp->send_list_head, node);
    }

    p = ffrdp->send_list_tail.prev;
    while (p != &ffrdp->send_list_head) {
        if ((p->flags & FLAG_GET_RXACK) == 0) {
            if ((p->flags & FLAG_FIRST_SEND) == 0) { // first send
                if ((int)ffrdp->recv_win > p->size - 4) {
                    ret = sendto(ffrdp->udp_fd, p->data, p->size, 0, (struct sockaddr*)dstaddr, sizeof(struct sockaddr_in));
                    if (ret != p->size) break;
                    ffrdp->recv_win -= p->size - 4;
                    p->tick_send = get_tick_count();
                    p->flags    |= FLAG_FIRST_SEND;
                } else if ((int32_t)get_tick_count() - (int32_t)ffrdp->tick_query_rwin > FFRDP_WIN_CYCLE) { // query rx recv win
                    data[0] = FFRDP_FRAME_TYPE_WIN0;
                    sendto(ffrdp->udp_fd, data, 1, 0, (struct sockaddr*)dstaddr, sizeof(struct sockaddr_in));
                    ffrdp->tick_query_rwin = get_tick_count();
                }
            } else if (get_tick_count() > p->tick_send + ffrdp->rto || (p->flags & FLAG_FAST_RESEND)) { // resend
                ret = sendto(ffrdp->udp_fd, p->data, p->size, 0, (struct sockaddr*)dstaddr, sizeof(struct sockaddr_in));
                if (ret != p->size) break;
                p->tick_send = get_tick_count();
                p->flags    |= FLAG_RTO_RESEND;
                p->flags    &=~FLAG_FAST_RESEND;
            }
        }
        p = p->prev;
    }

    node = NULL; fack = 0;
    while (1) {
        if (!node) node = frame_node_new(4 + FFRDP_MTU_SIZE);
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
            node->size = ret; list_add(&ffrdp->recv_list_head, node); node = NULL; fdat = 1;
            break;
        case FFRDP_FRAME_TYPE_ACK:
            if (seq_distance(*(uint32_t*)(node->data + 0) >> 8, una) >= 0) {
                una = *(uint32_t*)(node->data + 0) >> 8;
                mack= *(uint32_t*)(node->data + 4) & 0xFFFF;
                win = *(uint32_t*)(node->data + 4) >> 16;
                tick= get_tick_count();
                fack= 1;
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
            ffrdp->recv_win = (data[1] << 0) | (data[2] << 8);
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

    if ((ffrdp->flags & FLAG_SERVER) == 0 && (ffrdp->flags & FLAG_BYEBYE0) && (ffrdp->flags & FLAG_BYEBYE1) == 0) {
        data[0] = FFRDP_FRAME_TYPE_BYE; sendto(ffrdp->udp_fd, data, 1, 0, (struct sockaddr*)dstaddr, sizeof(struct sockaddr_in));
    }

    if (fack) { // got ack frame
        for (i=15; i>=0 && !(mack&(1<<i)); i--);
        maxackseq = una + i;
        ffrdp->recv_win = win; // update rx recv window size
        p = ffrdp->send_list_tail.prev;
        while (p != &ffrdp->send_list_head) {
            seq = *(uint32_t*)p->data >> 8;
            dist= seq_distance(una, seq);
            if (dist >= 0) {
                t = p->prev; list_del(p, 1); p = t;
            } else if ((1 << (-dist)) & mack) {
                p->flags |= FLAG_GET_RXACK;
            } else if (seq_distance(maxackseq, seq) >= 0) {
                p->flags |= FLAG_FAST_RESEND;
            }
            if ((p->flags & FLAG_RTO_RESEND) == 0) {
                if (ffrdp->rttm == 0) {
                    ffrdp->rttm = (int32_t)tick - (int32_t)p->tick_send;
                    ffrdp->rtts = ffrdp->rttm;
                    ffrdp->rttd = ffrdp->rttm / 2;
                } else {
                    ffrdp->rtts = (7 * ffrdp->rtts + 1 * ffrdp->rttm) / 8;
                    ffrdp->rttd = (3 * ffrdp->rttd + 1 * (ffrdp->rttm - ffrdp->rtts)) / 4;
                }
                ffrdp->rto = ffrdp->rtts + 4 * ffrdp->rttd;
            } else {
                ffrdp->rto+= ffrdp->rto / 2;
            }
            p = p->prev;
        }
    }

    if (fdat) { // got data frame
        mack = 0;
        p = ffrdp->recv_list_tail.prev;
        while (p != &ffrdp->recv_list_head) {
            seq = *(uint32_t*)p->data >> 8;
            if (seq == ffrdp->recv_seq) {
                if (p->size - 4 < (int)(sizeof(ffrdp->recv_buff) - ffrdp->recv_size)) {
                    ffrdp->recv_tail = ringbuf_write(ffrdp->recv_buff, sizeof(ffrdp->recv_buff), ffrdp->recv_tail, p->data + 4, p->size - 4);
                } else {
                    printf("why size it not enough ?\n");
                    break;
                }
                ffrdp->recv_seq++; ffrdp->recv_seq &= 0xFFFFFF;
                t = p->prev; list_del(p, 1); p = t;
            } else break;
            p = p->prev;
        }
        while (p != &ffrdp->recv_list_head) {
            seq = *(uint32_t*)p->data >> 8;
            dist = seq_distance(seq, ffrdp->recv_seq);
            if (dist > 0 && dist < 16) mack |= (1 << dist);
            p = p->prev;
        }
        *(uint32_t*)(data + 0) = (FFRDP_FRAME_TYPE_ACK << 0) | (seq << 8);
        *(uint32_t*)(data + 4) = (sizeof(ffrdp->recv_buff) - ffrdp->recv_size) << 16;
        sendto(ffrdp->udp_fd, data, sizeof(data), 0, (struct sockaddr*)dstaddr, sizeof(struct sockaddr_in)); // send ack frame
    }
}

void ffrdp_free(void *ctxt)
{
    FFRDPCONTEXT *ffrdp = (FFRDPCONTEXT*)ctxt;
    if (!ctxt) return;
    if (ffrdp->udp_fd > 0) closesocket(ffrdp->udp_fd);
    free(ffrdp);
}

int main(void)
{
    return 0;
}
