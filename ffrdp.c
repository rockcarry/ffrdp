#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "ffrdp.h"

#ifdef CONFIG_ENABLE_AES256
#include <openssl/aes.h>
#endif

#ifdef WIN32
#include <winsock2.h>
#define usleep(t) Sleep((t) / 1000)
#define get_tick_count GetTickCount
#define socklen_t int32_t
#else
#include <time.h>
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

// 收 / 发两个 MSS **必须分开**：线上字节 = 本帧 + UDP 8 + IPv4 20 = 帧 + 28，
// 只按 1500 建帧 ⇒ 满帧 4+1492 = 1496 加头 = 1524 > 1500（以太网 MTU）⇒ **每个满帧都被 IP 分片** ✗。
//   · FFRDP_MAX_RX_MSS：**能收多大**，**不能缩** ✗（收包节点按它建，超了是静默截断 ⇒ 老对端的满帧
//     会被悄悄切掉尾巴）；(1500 - 8) & ~3 = 1492 = 旧值 ⇒ **收侧行为不变** ✓。
//   · FFRDP_MAX_MSS：**发多大** = 再扣掉网络头，并对齐 4 字节。取 1464 是为了让 FEC 帧
//     4 + 1464 + 2 + 28 = 1498 也不出 MTU ✓（普通满帧 4 + 1464 + 28 = 1496 ✓）。
#define FFRDP_NET_HEADER_RESERVE 28    // UDP 8 + IPv4 20
#define FFRDP_MAX_RX_MSS    ((1500 - 8) & ~3)   // 1492
#define FFRDP_MAX_MSS       ((FFRDP_MAX_RX_MSS - FFRDP_NET_HEADER_RESERVE) & ~3)   // 1464
#define FFRDP_ETHERNET_MTU  1500
#if (FFRDP_MAX_MSS + 4 + 2 + FFRDP_NET_HEADER_RESERVE) > FFRDP_ETHERNET_MTU
#error "满 FEC 帧 + UDP/IP 头超过以太网 MTU ⇒ 会 IP 分片：该缩 FFRDP_MAX_MSS"
#endif
#define FFRDP_MIN_RTO        16
#define FFRDP_MAX_RTO        2000
#define FFRDP_MAX_WAITSND    256
#define FFRDP_QUERY_CYCLE    500
#define FFRDP_FLUSH_TIMEOUT  500
#define FFRDP_DEAD_TIMEOUT   5000
#define FFRDP_MIN_CWND_SIZE  1
#define FFRDP_DEF_CWND_SIZE  32
#define FFRDP_MAX_CWND_SIZE  64

// step29: the real brake -- the number of frames allowed on the wire at once. The peer's advertised window
// cannot serve as one: it is handed out again on every ack, so a limit placed on it never binds (measured:
// 63 frames in flight while the cap read 22). Measured on the target path: link ~365KB/s, idle round trip
// 5..16ms, so the bandwidth-delay product is 1..6 frames -- and the standing queue we were holding came
// from keeping 64 of them in flight, which is exactly the 219ms the ping sees while the test runs.
#define FFRDP_INFLIGHT_MAX   64   // default ceiling for frames on the wire. The number that is useful on a
                                  // given path cannot be picked in advance: the simulator (fast links, coarse
                                  // acks) breaks below 64, while the target path needs 2..6 to stop holding a
                                  // 219ms queue. So the default is the harmless value, and the probe (see
                                  // FFRDP_PROBE_ENABLE) is what experiments its way down to the right one.
                                  // It is a source constant, not a build switch: change it here and rebuild.
#define FFRDP_INFLIGHT_MIN   4    // the floor the brake never goes below, so the probe never asks for less.
                                  // Named because the three places that used to write their own 4 or 8 drifted
                                  // apart and that is what the sweep caught: the brake clamped to 4, the probe
                                  // asked for 4, but its gate demanded 8 frames on the wire -- so once a
                                  // proven ceiling had been given back up to 5 (see FFRDP_PROBE_HEALTHY_PCT),
                                  // the probe could never ask about it again and the run sat at 5 for ever:
                                  // measured inflight, cap = 5, 5, holding 4ms of queue more than the same
                                  // path held at 4. One constant, three users, no drift.
                                  // 4 is also where the descent ends by measurement, not by taste: the sweep
                                  // tried caps 2 and 3, both were silently clamped to this floor, and both
                                  // measured the same rtts 14ms over a 3..5ms floor. Nothing below 4 has ever
                                  // been observed to help, so this is the smallest question worth asking.

// the delay probe: take half the in-flight away for a moment and watch whether the round trip follows.
#define FFRDP_PROBE_HOLD_MS       500 // held for 2 round trips plus this much, long enough to drain a queue
#define FFRDP_PROBE_RTT_DROP_MS   2   // the typical round trip must fall by at least this many ms. Absolute, and
                                      // that is the whole point of the change: the rule used to be relative --
                                      // below 85% of the value in front of it -- and the value in front of it
                                      // was ffrdp->rttm, the LATEST sample rather than the typical one, so the
                                      // same experiment passed or failed depending on which single sample
                                      // happened to precede it. On the measured path the winning step is rtts
                                      // 18 -> 14ms, i.e. 4ms: 85% of 18 asks for 2.7ms and got it only when
                                      // the sample in front was high enough. A relative rule also cannot see
                                      // a queue that refuses to drain: 15% of a 5ms floor is far under the
                                      // jitter. What the experiment can actually answer is "did taking half
                                      // the frames away visibly lower the latency", which is a difference in
                                      // milliseconds -- so it is compared as one. The rate guard below is what
                                      // keeps an over-eager keep harmless, and the giveback undoes a stale one.
#define FFRDP_PROBE_RATE_KEEP_PCT 85  // ... while the rate the peer confirms stays above this percentage
#define FFRDP_PROBE_ENABLE        1   // on by default: it is a net win wherever a queue is being held, and
                                      // where there is none the rate guard reverts it and nothing is lost.
                                      // it is also how a new path gets asked "is the delay ours?", so it is
                                      // left on rather than built specially for the question.
// the queue test: how far the typical round trip sits above the path's own floor is what decides whether an
// experiment is worth running. at 4x the probe stops asking while the excess is merely smaller, not gone:
// on the target path it converged to rtts 12~19ms over a 4ms floor, i.e. 3~4.75x, and the 4x gate then left
// it idle although a queue it could still have halved was there. 3x asks the question one step closer to
// the floor. being curious here stays safe because the answer must still pass the same two guards (the round
// trip must fall, the confirmed rate must hold) and a failed question doubles the gap -- so the cost is
// bounded by one half window per back off, never by a permanently smaller pipe.
#define FFRDP_PROBE_QUEUE_RATIO   3
#define FFRDP_PROBE_WASTE_PM      5   // retransmits per thousand first transmissions, over the last second
                                      // (the gate that used to sit here, "at least 8 frames on the wire", is gone:
                                      //  what earns an experiment is not a big pipe but a ceiling that can still
                                      //  be lowered, so the test is now "the halved ceiling is strictly under the
                                      //  one in force" against FFRDP_INFLIGHT_MIN's floor -- see below.)
                                      // two older gates -- "rttm >= 50ms" and "enough retransmits in the window" --
                                      // were replaced by the queue test and are gone.
#define FFRDP_PROBE_GAP_MS        10000
#define FFRDP_PROBE_GAP_MAX_MS    60000
#define FFRDP_PROBE_HEALTHY_PCT   1   // below this much waste the kept window is given back a little

#define FFRDP_RTO_FLOOR_MAX  1000 // ms: a longer repair delay than this is never worth paying
#define FFRDP_RTO_WIN        128  // retransmits per evidence window, so the loop reacts in about a second
#define FFRDP_WASTE_MIN      16   // retransmits a window must contain before it is allowed to decide anything
#define FFRDP_WASTE_HI       80   // % of our retransmits the peer already had => the rto is sitting too short
#define FFRDP_WASTE_LO       40   // % below which the repairs are real and the floor is given back

#define FFRDP_RECVBUF_SIZE  (128 * (FFRDP_MAX_RX_MSS + 0))   // 收侧容量 ⇒ 按**收** MSS 算（用发 MSS 会缩掉窗口）
// how far ahead of the hole a received frame may sit before it is dropped instead of stored. The window
// we advertise travels in a one byte field of the ack, so it is never more than 255 frames, and a peer
// that respects its credit cannot be further ahead than that. Anything beyond it is a lie or a
// duplicate, and storing it is how one stranger - with no session, no handshake and one datagram per
// frame - grows this process's memory without limit (measured before this bound existed: 4000
// datagrams, 4000 frames kept).
#define FFRDP_RECV_QUEUE_MAX 256
#define FFRDP_UDPSBUF_SIZE  (128 * (FFRDP_MAX_RX_MSS + 6))   // 内核收发缓冲 ⇒ 也按收 MSS（保持原容量）
#define FFRDP_UDPRBUF_SIZE  (128 * (FFRDP_MAX_RX_MSS + 6))
#define FFRDP_SELECT_SLEEP   1
#define FFRDP_SELECT_TIMEOUT 10000
#define FFRDP_USLEEP_TIMEOUT 1000

#define MIN(a, b)               ((a) < (b) ? (a) : (b))
#define MAX(a, b)               ((a) > (b) ? (a) : (b))
#define GET_FRAME_SEQ(f)        (*(uint32_t*)(f)->data >> 8)
#define SET_FRAME_SEQ(f, seq)   do { *(uint32_t*)(f)->data = ((f)->data[0]) | (((seq) & 0xFFFFFF) << 8); } while (0)

enum {
    FFRDP_FRAME_TYPE_FULL,       // full  frame
    FFRDP_FRAME_TYPE_SHORT,      // short frame
    FFRDP_FRAME_TYPE_FEC2,       // fec2  frame
    FFRDP_FRAME_TYPE_FEC32 = 32, // fec32 frame
    FFRDP_FRAME_TYPE_ACK   = 33, // ack   frame
    FFRDP_FRAME_TYPE_QUERY = 34, // query frame
};

typedef struct tagFFRDP_FRAME_NODE {
    struct tagFFRDP_FRAME_NODE *next;
    struct tagFFRDP_FRAME_NODE *prev;
    uint16_t size; // frame size
    uint8_t *data; // frame data
    #define FLAG_FIRST_SEND     (1 << 0) // after frame first send, this flag will be set
    #define FLAG_TIMEOUT_RESEND (1 << 1) // data frame wait ack timeout and be resend
    #define FLAG_FAST_RESEND    (1 << 2) // data frame need fast resend when next update
    uint32_t flags;        // frame flags
    uint32_t tick_1sts;    // frame first time send tick
    uint32_t tick_send;    // frame send tick
    uint32_t tick_timeout; // frame ack timeout tick
} FFRDP_FRAME_NODE;

typedef struct {
    uint8_t  recv_buff[FFRDP_RECVBUF_SIZE];
    int32_t  recv_size, recv_head, recv_tail;
    #define FLAG_SERVER    (1 << 0)
    #define FLAG_CONNECTED (1 << 1)
    #define FLAG_FLUSH     (1 << 2)
    #define FLAG_TX_AES256 (1 << 3)
    #define FLAG_RX_AES256 (1 << 4)
    uint32_t flags;
    SOCKET   udp_fd;
    struct   sockaddr_in server_addr;
    struct   sockaddr_in client_addr;

    FFRDP_FRAME_NODE *send_list_head;
    FFRDP_FRAME_NODE *send_list_tail;
    FFRDP_FRAME_NODE *recv_list_head;
    FFRDP_FRAME_NODE *recv_list_tail;
    FFRDP_FRAME_NODE *cur_new_node;
    uint32_t          cur_new_size;
    uint32_t          cur_new_tick;
    uint32_t send_seq; // send seq
    uint32_t recv_seq; // send seq
    uint32_t wait_snd; // data frame number wait to send
    uint32_t rttm, rtts, rttd, rto;
    uint32_t rmss, smss, swnd, cwnd, ssthresh;
    uint32_t rto_floor;           // persistent lower bound on the rto, moved only by the peer's duplicate reports
    uint32_t win_need, win_dup;   // retransmits attempted in this evidence window, and how many the peer called pointless
    uint32_t tick_win;            // when the current evidence window was opened
    uint32_t hole_wait;           // ms a frame must be outstanding before a reported hole is believed
    uint32_t tick_recv_ack;
    uint32_t tick_send_query;
    uint32_t tick_ffrdp_dump;
    uint32_t tick_ffrdp_init;  // tick of ffrdp_init, for the elapsed time in the dump
    uint32_t tick_rto_backoff; // tick of last rto backoff, rto backs off once per round

    uint8_t  fec_txbuf[4 + FFRDP_MAX_MSS + 2];
    uint8_t  fec_rxbuf[4 + FFRDP_MAX_RX_MSS + 2];   // 接收侧：按**收** MSS（对端可以比我们发得大）
    uint8_t  fec_txredundancy, fec_rxredundancy;
    uint16_t fec_txseq;
    uint16_t fec_rxseq;
    uint16_t fec_rxcnt;
    uint32_t fec_rxmask;

#ifdef CONFIG_ENABLE_AES256
    AES_KEY  aes_encrypt_key;
    AES_KEY  aes_decrypt_key;
#endif

    uint32_t counter_udpsenderr;
    // every sendto that did not take the frame, counted over the dump interval, whichever path it came
    // from: a data frame, an ack, a query, an fec frame. counter_udpsenderr above cannot answer the
    // question this exists for -- it is a CONSECUTIVE failure count, reset by the next success, so a
    // socket that refuses one frame in ten (a send buffer too small for the burst) reads as zero at
    // almost every sampling instant. The ack/query/fec paths do not check their sendto at all, so until
    // this counter existed, an ack that could not be sent was invisible everywhere.
    uint32_t counter_send_err;
    // 注意：这两个是【字节】计数，必须是 64 位。32 位版本累计到 4 GiB 就回绕，而 averg_* 拿
    // 回绕后的值去除以"已经过去的秒数"，打印出来就是突然掉到地板上的假速率 —— 实测 140s 处
    // total_send 4031MB、150s 处 207MB，而同一刻瞬时 confirmed rate 一直是 2.2 万帧/s。
    uint64_t counter_send_bytes;   // payload of the frames the peer acked: bytes DELIVERED, not bytes put on the wire
    uint64_t counter_recv_bytes;   // ... of the frames we acked
    uint32_t counter_send_1sttime; // first transmissions: the denominator of counter_resend_ratio
    uint32_t counter_send_failed;  // ffrdp_send refused the call (app queue full, or an allocation failed)
    uint32_t counter_send_query;
    uint32_t counter_resend_fast;
    uint32_t counter_resend_rto;
    uint32_t counter_reach_maxrto;
    uint32_t counter_txfull , counter_rxfull ;
    uint32_t counter_recv_dup;   // data frames that arrived although we already had them
    uint32_t counter_ack_dup;    // ... as reported back to us by the peer: retransmits of ours that were pointless
    uint32_t dup_seen;           // duplicates seen since our last ack, the value that ack carries
    uint32_t counter_txshort, counter_rxshort;
    // fragments that were thrown away instead of being acted on, counted separately because they mean
    // different things: rx_bad is a fragment whose length cannot describe anything (it is malformed,
    // whoever sent it), recv_drop is a well formed frame that sat further ahead of the hole than the
    // window we advertise, so storing it would only spend our memory on someone else's lie.
    uint32_t counter_rx_bad;
    uint32_t counter_recv_drop;
    uint32_t counter_fec_tx;
    uint32_t counter_fec_rx;
    uint32_t counter_fec_ok;
    uint32_t counter_fec_failed;
    uint32_t counter_ack_recv   ; // ack frames received
    uint32_t counter_ack_mack   ; // ack frames carrying out of order (mack) info
    uint32_t counter_ack_hole   ; // ack frames reporting a gap below the highest acked frame
    uint32_t counter_recv_ooo   ; // data frames that arrived out of order: there is a hole below them
    uint32_t counter_recv_far   ; // ... and they are more than 23 ahead of the hole, so the 24 bit ack mask cannot carry them
    // how far ahead of the hole the out of order arrivals were, in the three bands that decide how wide
    // an ack mask would have to be: 1..24 is what today's 24 bit mask carries (bit dist-1, so dist 24 is
    // its last bit), 25..64 is what a 64 bit one would add, and beyond 64 not even that could speak.
    // Nothing acts on these: they exist so that the cost of widening the mask (both ends must be
    // upgraded) is only paid after the distribution says it would buy anything. Note that these bands
    // are one frame wider than counter_recv_far above, which counts dist > 23 -- dist 24 does fit in
    // the mask, so the old counter is a frame stricter than the thing it is describing.
    uint32_t counter_ooo_d1_24  ; // ... and the distance was 1..24 frames: the mask of today reaches these
    uint32_t counter_ooo_d25_64 ; // ... 25..64 frames: a 64 bit mask would reach these, this one cannot
    uint32_t counter_ooo_dgt64  ; // ... more than 64 frames: out of reach for a single extension word
    // ... and how far past 64 the tail actually goes. On a real link 8% of the out of order arrivals
    // sat beyond 64, while nothing did in the simulator (whose window is only 64 wide), so the width
    // of the extension has to be decided by this, not by the argument that the in-flight gate would
    // bound the distance -- it does not: a frame that the peer reports in its mask leaves our send
    // list while the hole stays open, so the sender keeps running ahead of the hole without limit.
    uint32_t counter_ooo_d65_128; // ... 65..128 frames: a second extension word would reach these
    uint32_t counter_ooo_dgt128 ; // ... more than 128 frames: out of reach of two words
    // the mask itself counted in bits instead of frames. counter_recv_ooo says how many out of order
    // frames arrived; bits_sent says how many of them we actually told the peer about, and a frame keeps
    // its bit in every later ack until the hole below it fills, so bits_sent >= ooo is the healthy
    // reading. Comparing bits_sent (the receiver's count) with bits_recv (the sender's) is the only way
    // to tell a report that was never made from one that was made and never arrived -- neither end can
    // see that on its own.
    uint32_t counter_mack_bits_tx;// mack bits we put on the wire
    uint32_t counter_mack_bits_rx;// mack bits the peer sent us
    uint32_t rtt_min;             // lowest round trip seen this interval: the path's floor, the number to judge latency by
    uint32_t inflight_used;       // frames actually on the wire (recounted every pass): what step29 limits

    uint32_t counter_rto_backoff; // times the rto was multiplied
    uint32_t counter_timeout_evt; // updates in which at least one frame timed out
    uint32_t tick_last_upd  ;     // tick of the previous ffrdp_update call
    uint32_t counter_upd_cnt;     // number of ffrdp_update calls
    uint32_t counter_upd_gap;     // sum of the gaps between ffrdp_update calls, in ms
    uint32_t counter_upd_gap_max; // longest gap between two calls, in ms
    uint32_t counter_upd_cost_max;// longest time spent inside one call, in ms
    uint32_t counter_ack_una_dup; // ack frames whose una is the same as the previous one: the peer reports no progress
    uint32_t counter_rto_head ;   // rto retransmits of the send list head, the frame that blocks the stream
    uint32_t counter_rto_chain;   // rto retransmits of a frame queued behind the head
    uint32_t counter_rto_stale;   // rto retransmits of a frame the peer had already reported as received
    uint32_t counter_rto_behind;  // rto retransmits of a frame the peer had not reported yet
    uint32_t ack_una;             // last una seen in an ack frame
    uint32_t reserved;            // ffrdp_dump clears everything up to here: nothing that must outlive a
                                  // dump, and nothing a decision depends on, may sit above this line
                                  // (the probe would otherwise be reset exactly when it is about to run).
    // ---- delay probe (step28): is the delay we see our own queue, or the path's? ----
    // No observation can tell those apart: the lowest round trip ever seen already contains a queue
    // that never drains, so the delay-versus-minimum comparison that every delay based scheme uses is
    // blind exactly where it is needed. The only way to know is an experiment -- take half the frames
    // out of flight and see whether the delay follows -- and the only reason it is safe to run is that
    // a link that queues and a link that loses answer differently: if the delay falls while the rate
    // the peer confirms does not, the queue was ours; if the delay does not fall, the delay belongs to
    // the path and the window goes straight back. Needs nothing from the peer.
    uint32_t probe_state;         // 0 idle, 1 probing
    uint32_t probe_tick;          // when the current probe started
    uint32_t probe_hold;          // how long the reduced window is held
    uint32_t probe_cap;           // the reduced in-flight limit under test
    uint32_t probe_rtt_ref;       // lowest round trip just before the probe
    uint32_t probe_best;          // lowest round trip seen during the probe
    uint32_t probe_del_ref;       // delivered frames at the start of the probe
    uint32_t probe_rate_ref;      // frames per second the peer confirmed just before the probe
    uint32_t probe_gap;           // current back off between probes
    uint32_t probe_next;          // earliest tick for the next probe
    uint32_t cap, cap_kept;       // in-flight limit enforced now, and the one a probe proved
    uint32_t inflight_max;        // the frames-on-the-wire ceiling actually used (0 = FFRDP_INFLIGHT_MAX)
    uint32_t rate_tick, rate_del_ref, rate_now; // delivered frames per second, sampled once a second
    uint32_t probe_waste;         // retransmits as a percentage of first transmissions over the last second
    uint32_t probe_resend_seen, probe_first_seen; // ... the raw counts that percentage is taken against
    uint32_t counter_delivered;   // frames the peer has confirmed and we have let go of
    uint32_t win_first;           // first transmissions in this evidence window
    uint32_t counter_probe, counter_probe_keep, counter_probe_revert;
    uint32_t counter_probe_narrow, counter_giveback;
} FFRDPCONTEXT;

static uint32_t ringbuf_write(uint8_t *rbuf, uint32_t maxsize, uint32_t tail, uint8_t *src, uint32_t len)
{
    uint8_t *buf1 = rbuf + tail;
    int      len1 = MIN(maxsize - tail, len);
    uint8_t *buf2 = rbuf;
    int      len2 = len  - len1;
    memcpy(buf1, src + 0   , len1);
    memcpy(buf2, src + len1, len2);
    return (tail + len) % maxsize;
}

static uint32_t ringbuf_read(uint8_t *rbuf, uint32_t maxsize, uint32_t head, uint8_t *dst, uint32_t len)
{
    uint8_t *buf1 = rbuf + head;
    int      len1 = MIN(maxsize - head, len);
    uint8_t *buf2 = rbuf;
    int      len2 = len  - len1;
    if (dst) {
        memcpy(dst + 0   , buf1, len1);
        memcpy(dst + len1, buf2, len2);
    }
    return (head + len) % maxsize;
}

static int seq_distance(uint32_t seq1, uint32_t seq2) // calculate seq distance
{
    int c = seq1 - seq2;
    if      (c >=  0x7FFFFF) return c - 0x1000000;
    else if (c <= -0x7FFFFF) return c + 0x1000000;
    else return c;
}

static FFRDP_FRAME_NODE* frame_node_new(int type, int size) // create a new frame node
{
    FFRDP_FRAME_NODE *node = calloc(1, sizeof(FFRDP_FRAME_NODE) + 4 + size + (type <= FFRDP_FRAME_TYPE_SHORT ? 0 : 2));
    if (!node) return NULL;
    node->size    = 4 + size + (type <= FFRDP_FRAME_TYPE_SHORT ? 0 : 2); // 4 bytes header + data size + 2 bytes fec seq
    node->data    = (uint8_t*)node + sizeof(FFRDP_FRAME_NODE);
    node->data[0] = type;
    return node;
}

#ifdef CONFIG_ENABLE_AES256
static void frame_node_encrypt(FFRDP_FRAME_NODE *node, AES_KEY *key, int enc)
{
    uint8_t *pdata = node->data + 4, *pend = node->data + node->size - (node->data[0] <= FFRDP_FRAME_TYPE_SHORT ? 0 : 2) - AES_BLOCK_SIZE;
    while (pdata <= pend) {
        AES_ecb_encrypt(pdata, pdata, key, enc);
        pdata += AES_BLOCK_SIZE;
    }
}
#endif

static int frame_payload_size(FFRDP_FRAME_NODE *node) {
    return  node->size - 4 - (node->data[0] <= FFRDP_FRAME_TYPE_SHORT ? 0 : 2);
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
        seqnew = GET_FRAME_SEQ(node);
        for (p = *tail; p; p = p->prev) {
            seqcur = GET_FRAME_SEQ(p);
            dist   = seq_distance(seqnew, seqcur);
            if (dist == 0) { free(node); return; } // duplicated frame, drop it and free memory
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
        node->next->prev = node;
        *head = node;
    }
}

static void list_remove(FFRDP_FRAME_NODE **head, FFRDP_FRAME_NODE **tail, FFRDP_FRAME_NODE *node)
{
    if (node->next) node->next->prev = node->prev;
    else *tail = node->prev;
    if (node->prev) node->prev->next = node->next;
    else *head = node->next;
    free(node);
}

static void list_free(FFRDP_FRAME_NODE **head, FFRDP_FRAME_NODE **tail)
{
    while (*head) list_remove(head, tail, *head);
}

static int ffrdp_sleep(FFRDPCONTEXT *ffrdp, int flag)
{
    if (ffrdp->flags & FLAG_FLUSH) { ffrdp->flags &= ~FLAG_FLUSH; return 0; }
    if (flag) {
        struct timeval tv;
        fd_set  rs;
        FD_ZERO(&rs);
        FD_SET(ffrdp->udp_fd, &rs);
        tv.tv_sec  = 0;
        tv.tv_usec = FFRDP_SELECT_TIMEOUT;
        if (select((int)ffrdp->udp_fd + 1, &rs, NULL, NULL, &tv) <= 0) return -1;
    } else usleep(FFRDP_USLEEP_TIMEOUT);
    return 0;
}

static int ffrdp_send_data_frame(FFRDPCONTEXT *ffrdp, FFRDP_FRAME_NODE *frame, struct sockaddr_in *dstaddr)
{
    switch (frame->size - ffrdp->smss) {
    case 6 : ffrdp->counter_fec_tx ++; *(uint16_t*)(frame->data + 4 + ffrdp->smss) = ffrdp->fec_txseq++; break; // tx fec frame
    case 4 : ffrdp->counter_txfull ++; break; // tx full  frame
    default: ffrdp->counter_txshort++; break; // tx short frame
    }
    if (sendto(ffrdp->udp_fd, (void*)frame->data, frame->size, 0, (struct sockaddr*)dstaddr, sizeof(struct sockaddr_in)) != frame->size) { ffrdp->counter_udpsenderr++; ffrdp->counter_send_err++; return -1; }
    else ffrdp->counter_udpsenderr = 0;
    if (frame->size == 4 + ffrdp->smss + 2) { // fec frame
        uint32_t *psrc = (uint32_t*)frame->data, *pdst = (uint32_t*)ffrdp->fec_txbuf, i;
        for (i = 0; i < (4 + ffrdp->smss) / sizeof(uint32_t); i++) *pdst++ ^= *psrc++; // make xor fec frame
        if (ffrdp->fec_txseq % ffrdp->fec_txredundancy == ffrdp->fec_txredundancy - 1) {
            *(uint16_t*)(ffrdp->fec_txbuf + 4 + ffrdp->smss) = ffrdp->fec_txseq++; ffrdp->fec_txbuf[0] = ffrdp->fec_txredundancy;
            // count it like any other send: a redundancy frame the socket refused is a repair that will
            // not happen, and it used to leave no trace anywhere.
            if (sendto(ffrdp->udp_fd, (void*)ffrdp->fec_txbuf, frame->size, 0, (struct sockaddr*)dstaddr, sizeof(struct sockaddr_in)) != frame->size) ffrdp->counter_send_err++; // send fec frame
            memset(ffrdp->fec_txbuf, 0, sizeof(ffrdp->fec_txbuf)); // clear tx_fecbuf
            ffrdp->counter_fec_tx++;
        }
    }
    return 0;
}

static int ffrdp_recv_data_frame(FFRDPCONTEXT *ffrdp, FFRDP_FRAME_NODE *frame)
{
    uint32_t fecseq, fecrdc, *psrc, *pdst, type, i;
    // The length below is not just a number: the payload size of the peer (rmss) is learned from it, and
    // rmss is used as a divisor (the window this ack advertises) and as a copy length (the bytes handed
    // to the application). A fragment shorter than its own overhead therefore does not merely look odd:
    // a four byte "full" frame makes rmss zero (divide by zero, SIGFPE), a four byte "fec" frame -- whose
    // overhead is six -- makes it negative, which underflows to four billion and reaches memcpy as a
    // length (SIGSEGV). Both were reachable with a single datagram sent by anyone who can reach the port,
    // and both are what tools/sim/t_fuzz.c exists to keep out.
    switch (frame->data[0]) {
    case FFRDP_FRAME_TYPE_SHORT: ffrdp->counter_rxshort++; return 0; // short frame
    case FFRDP_FRAME_TYPE_FULL :
        if (frame->size <= 4) { ffrdp->counter_rx_bad++; return -1; }  // header only: no payload at all
        ffrdp->counter_rxfull ++; ffrdp->rmss = frame->size - 4; return 0; // full frame
    default:
        if (frame->size <= 6) { ffrdp->counter_rx_bad++; return -1; }  // header + fec seq, no payload
        ffrdp->counter_fec_rx ++; ffrdp->rmss = frame->size - 6; break;    // fec  frame
    }
    fecseq = *(uint16_t*)(frame->data + frame->size - 2);
    fecrdc = frame->data[0];
    if (fecseq / fecrdc != ffrdp->fec_rxseq / fecrdc || ffrdp->fec_rxredundancy != fecrdc) { // group changed or fec_rxredundancy changed
        memcpy(ffrdp->fec_rxbuf, frame->data, frame->size);
        ffrdp->fec_rxseq = fecseq; ffrdp->fec_rxredundancy = fecrdc;
        if (fecseq % fecrdc == fecrdc - 1) {
            // ⚠️ 这一帧**就是冗余帧** ⇒ 它不能算成"收到一个数据帧" ✗：此刻组里收到的数据是 0 个 ✓。
            //    旧写法把它记成 rxcnt = 1 并直接 return -1 ⇒ 组里只剩一个数据帧时（rdc == 2 ✓）
            //    那一帧永远补不回来 ✗（实测：fec:2 的冗余等于白搭 ✓，由 t_fec 阶段 C 钉住 ✓）。
            ffrdp->fec_rxmask = 0;
            ffrdp->fec_rxcnt  = 0;
        } else {
            ffrdp->fec_rxmask = 1 << (fecseq % fecrdc);
            ffrdp->fec_rxcnt  = 1;
        }
        // 这里**不再 return**：接着走下面的"冗余帧恢复 / 数据帧累积" ✓（rdc == 2 正是靠这条才恢复 ✓）
    } else ffrdp->fec_rxseq = fecseq; // group not changed
    if (fecseq % fecrdc == fecrdc - 1) { // it's redundance frame
        if (ffrdp->fec_rxcnt == fecrdc - 1) return -1;
        if (ffrdp->fec_rxcnt != fecrdc - 2) { ffrdp->counter_fec_failed++; return -1; }
        type = frame->data[0];
        // rxcnt == 0（只有 rdc == 2 到得了这儿 ✓）：组里一个数据帧都没收到 ⇒ 冗余帧**本身就是**
        // 那一帧 ✓（异或空集 = 原样 ✓）⇒ 不能再拿它跟自己异或一遍（那是全 0 ✗）。
        if (ffrdp->fec_rxcnt > 0) {
            psrc = (uint32_t*)ffrdp->fec_rxbuf; pdst = (uint32_t*)frame->data;
            for (i = 0; i < (frame->size - 2) / sizeof(uint32_t); i++) *pdst++ ^= *psrc++;
        }
        frame->data[0] = type;
        ffrdp->counter_fec_ok++;
    } else if (!(ffrdp->fec_rxmask & (1 << (fecseq % fecrdc)))) { // update fec_rxbuf
        psrc = (uint32_t*)frame->data; pdst = (uint32_t*)ffrdp->fec_rxbuf;
        for (i = 0; i < (frame->size - 2) / sizeof(uint32_t); i++) *pdst++ ^= *psrc++;
        ffrdp->fec_rxmask |= 1 << (fecseq % fecrdc); ffrdp->fec_rxcnt++;
    }
    return 0;
}

void* ffrdp_init(char *ip, int port, char *txkey, char *rxkey, int server, int smss, int sfec)
{
    FFRDPCONTEXT *ffrdp = NULL;
    unsigned long opt;
#ifdef WIN32
    WSADATA wsaData;
    timeBeginPeriod(1);
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        printf("WSAStartup failed !\n");
        return NULL;
    }
#endif

    if (!(ffrdp = calloc(1, sizeof(FFRDPCONTEXT)))) return NULL;
    ffrdp->swnd     = FFRDP_DEF_CWND_SIZE;
    ffrdp->cwnd     = FFRDP_DEF_CWND_SIZE;
    ffrdp->ssthresh = FFRDP_DEF_CWND_SIZE;
    ffrdp->rtts     = (uint32_t) -1;
    ffrdp->rto      = FFRDP_MIN_RTO;
    ffrdp->rmss     = FFRDP_MAX_RX_MSS;   // rmss = "对端能发多大" ⇒ 用收 MSS，不是我们发多大
    ffrdp->smss     = MAX(1, MIN(smss, FFRDP_MAX_MSS));
    ffrdp->fec_txredundancy = MAX(0, MIN(sfec, FFRDP_FRAME_TYPE_FEC32));
    ffrdp->tick_ffrdp_dump  = get_tick_count();
    ffrdp->tick_ffrdp_init  = ffrdp->tick_ffrdp_dump;
    ffrdp->tick_rto_backoff = ffrdp->tick_ffrdp_dump - ffrdp->rto; // allow the first timeout to backoff

    ffrdp->server_addr.sin_family      = AF_INET;
    ffrdp->server_addr.sin_port        = htons(port);
    ffrdp->server_addr.sin_addr.s_addr = inet_addr(ip);
    ffrdp->udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (ffrdp->udp_fd < 0) {
        printf("failed to open socket !\n");
        goto failed;
    }

#ifdef WIN32
    opt = 1; ioctlsocket(ffrdp->udp_fd, FIONBIO, &opt); // setup non-block io mode
#else
    fcntl(ffrdp->udp_fd, F_SETFL, fcntl(ffrdp->udp_fd, F_GETFL, 0) | O_NONBLOCK);  // setup non-block io mode
#endif
    opt = FFRDP_UDPSBUF_SIZE; setsockopt(ffrdp->udp_fd, SOL_SOCKET, SO_SNDBUF   , (char*)&opt, sizeof(int)); // setup udp send buffer size
    opt = FFRDP_UDPRBUF_SIZE; setsockopt(ffrdp->udp_fd, SOL_SOCKET, SO_RCVBUF   , (char*)&opt, sizeof(int)); // setup udp recv buffer size
    opt = 1;                  setsockopt(ffrdp->udp_fd, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(int)); // setup reuse addr

    if (server) {
        ffrdp->flags |= FLAG_SERVER;
        if (bind(ffrdp->udp_fd, (struct sockaddr*)&ffrdp->server_addr, sizeof(ffrdp->server_addr)) == -1) {
            printf("failed to bind !\n");
            goto failed;
        }
    }

    if (txkey) {
#ifdef CONFIG_ENABLE_AES256
        ffrdp->flags |= FLAG_TX_AES256;
        AES_set_encrypt_key((uint8_t*)txkey, 256, &ffrdp->aes_encrypt_key);
#endif
    }
    if (rxkey) {
#ifdef CONFIG_ENABLE_AES256
        ffrdp->flags |= FLAG_RX_AES256;
        AES_set_decrypt_key((uint8_t*)rxkey, 256, &ffrdp->aes_decrypt_key);
#endif
    }
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
    if (ffrdp->cur_new_node) free(ffrdp->cur_new_node);
    list_free(&ffrdp->send_list_head, &ffrdp->send_list_tail);
    list_free(&ffrdp->recv_list_head, &ffrdp->recv_list_tail);
    free(ffrdp);
#ifdef WIN32
    WSACleanup();
    timeEndPeriod(1);
#endif
}

int ffrdp_send(void *ctxt, char *buf, int len)
{
    FFRDPCONTEXT *ffrdp = (FFRDPCONTEXT*)ctxt;
    FFRDP_FRAME_NODE *spare = NULL, *node;
    int           n = len, size, need, room;
    if (  !ffrdp || ((ffrdp->flags & FLAG_SERVER) && (ffrdp->flags & FLAG_CONNECTED) == 0)
        || ((len + ffrdp->smss - 1) / ffrdp->smss + ffrdp->wait_snd > FFRDP_MAX_WAITSND)) {
        if (ffrdp) ffrdp->counter_send_failed++;
        return -1;
    }
    // Allocate every frame this call will need before writing a single byte: if one of the
    // allocations fails the whole call is abandoned (return -1, nothing accepted). Doing it the
    // other way round ("allocate while copying") lets the caller end up with 0 < ret < len, and it
    // cannot safely retry that message then: the part already taken would show up twice in the
    // stream. With this order ffrdp_send is atomic from the caller's point of view -- it either
    // accepts the entire message (returns len) or accepts nothing at all (returns -1).
    room = ffrdp->cur_new_node ? (int)(ffrdp->smss - ffrdp->cur_new_size) : 0;
    need = (len > room) ? ((len - room) + (int)ffrdp->smss - 1) / (int)ffrdp->smss : 0;
    while (need-- > 0) {
        node = frame_node_new(ffrdp->fec_txredundancy, ffrdp->smss);
        if (!node) {
            while (spare) { node = spare; spare = spare->next; free(node); }
            ffrdp->counter_send_failed++;
            return -1;
        }
        node->next = spare; spare = node;
    }
    while (n > 0) {
        if (!ffrdp->cur_new_node) { // the pre-allocated chain is exactly enough, never runs dry
            ffrdp->cur_new_node = spare; spare = spare->next;
            ffrdp->cur_new_node->next = NULL;
        }
        SET_FRAME_SEQ(ffrdp->cur_new_node, ffrdp->send_seq);
        size = MIN(n, (int)(ffrdp->smss - ffrdp->cur_new_size));
        memcpy(ffrdp->cur_new_node->data + 4 + ffrdp->cur_new_size, buf, size);
        ffrdp->cur_new_size += size; buf += size; n -= size;
        if (ffrdp->cur_new_size == ffrdp->smss) {
#ifdef CONFIG_ENABLE_AES256
            if ((ffrdp->flags & FLAG_TX_AES256)) frame_node_encrypt(ffrdp->cur_new_node, &ffrdp->aes_encrypt_key, AES_ENCRYPT);
#endif
            list_enqueue(&ffrdp->send_list_head, &ffrdp->send_list_tail, ffrdp->cur_new_node);
            ffrdp->send_seq++; ffrdp->wait_snd++;
            ffrdp->cur_new_node = NULL;
            ffrdp->cur_new_size = 0;
        } else ffrdp->cur_new_tick = get_tick_count();
    }
    while (spare) { node = spare; spare = spare->next; free(node); } // cannot happen, but never leak
    return len - n; // with the pre-allocation above this is always len: the call is atomic
}

int ffrdp_recv(void *ctxt, char *buf, int len)
{
    FFRDPCONTEXT *ffrdp = (FFRDPCONTEXT*)ctxt;
    int           ret;
    if (!ctxt) return -1;
    ret = MIN(len, ffrdp->recv_size);
    if (ret > 0) {
        ffrdp->recv_head = ringbuf_read(ffrdp->recv_buff, sizeof(ffrdp->recv_buff), ffrdp->recv_head, (uint8_t*)buf, ret);
        ffrdp->recv_size-= ret; ffrdp->counter_recv_bytes += ret;
    }
    return ret;
}

int ffrdp_isdead(void *ctxt)
{
    FFRDPCONTEXT *ffrdp = (FFRDPCONTEXT*)ctxt;
    if (!ctxt) return -1;
    if (!ffrdp->send_list_head) return 0;
    if (ffrdp->send_list_head->flags & FLAG_FIRST_SEND) {
        return (int32_t)get_tick_count() - (int32_t)ffrdp->send_list_head->tick_1sts > FFRDP_DEAD_TIMEOUT;
    } else {
        return (int32_t)ffrdp->tick_send_query - (int32_t)ffrdp->tick_recv_ack > FFRDP_DEAD_TIMEOUT;
    }
}

static void ffrdp_recvdata_and_sendack(FFRDPCONTEXT *ffrdp, struct sockaddr_in *dstaddr)
{
    FFRDP_FRAME_NODE *p;
    int32_t dist, recv_mack, recv_wnd, size, i, alen;
    uint8_t data[9];
    while (ffrdp->recv_list_head) {
        dist = seq_distance(GET_FRAME_SEQ(ffrdp->recv_list_head), ffrdp->recv_seq);
        // size > 0 is the second line of defence against a frame whose length arithmetic went negative:
        // ringbuf_write would take that as four billion bytes to copy. The recv path no longer lets such
        // a frame in (see ffrdp_recv_data_frame), and this keeps a future path from doing it silently.
        if (dist == 0 && (size = frame_payload_size(ffrdp->recv_list_head)) > 0 &&
            size <= (int)(sizeof(ffrdp->recv_buff) - ffrdp->recv_size)) {
#ifdef CONFIG_ENABLE_AES256
            if ((ffrdp->flags & FLAG_RX_AES256)) frame_node_encrypt(ffrdp->recv_list_head, &ffrdp->aes_decrypt_key, AES_DECRYPT);
#endif
            ffrdp->recv_tail = ringbuf_write(ffrdp->recv_buff, sizeof(ffrdp->recv_buff), ffrdp->recv_tail, ffrdp->recv_list_head->data + 4, size);
            ffrdp->recv_size+= size;
            ffrdp->recv_seq++; ffrdp->recv_seq &= 0xFFFFFF;
            list_remove(&ffrdp->recv_list_head, &ffrdp->recv_list_tail, ffrdp->recv_list_head);
        } else break;
    }
    for (recv_mack = 0, i = 0, p = ffrdp->recv_list_head; i <= 24 && p; i++, p = p->next) {
        dist = seq_distance(GET_FRAME_SEQ(p), ffrdp->recv_seq);
        if (dist <= 24) recv_mack |= 1 << (dist - 1); // dist is obviously > 0
    }
    // how many frames this ack is telling the peer about, counted in bits so that it can be compared
    // with counter_mack_bits_rx on the other side: the difference between the two is the part of the
    // reporting that the path ate. a frame can contribute a bit to several acks in a row, so this is
    // not a frame count -- it answers "is the reporting happening at all", which is the question that
    // counter_ack_mack (acks carrying any bit) is too coarse to answer.
    for (i = 0; i < 24; i++) if (recv_mack & (1 << i)) ffrdp->counter_mack_bits_tx++;
    // rmss is a divisor learned from whatever arrived (ffrdp_recv_data_frame rejects the lengths that
    // cannot carry a payload; it starts at FFRDP_MAX_MSS). Never divide by it unconditionally.
    recv_wnd = ffrdp->rmss ? (sizeof(ffrdp->recv_buff) - ffrdp->recv_size) / ffrdp->rmss : 0;
    recv_wnd = MIN(recv_wnd, 255);
    *(uint32_t*)(data + 0) = (FFRDP_FRAME_TYPE_ACK << 0) | (ffrdp->recv_seq << 8);
    *(uint32_t*)(data + 4) = (recv_mack <<  0);
    *(uint32_t*)(data + 4)|= (recv_wnd  << 24);
    // one byte of independent evidence for the sender: how many frames arrived since our last ack that we
    // already had, i.e. how many of its retransmits were pointless. the sender's own retransmit counter
    // cannot answer that (it counts attempts, not arrivals), this can. a peer that does not send the byte
    // simply leaves the field zero, and then the sender's floor never moves -- old peers stay safe.
    // pay for the extra byte only when there is something to say: on a link where nothing is being
    // retransmitted twice the ack stays exactly as long as it always was, so nothing about the sender's
    // behaviour can change there at all.
    if (ffrdp->dup_seen) { data[8] = (uint8_t)MIN(ffrdp->dup_seen, 255); alen = (int32_t)sizeof(data); }
    else                 { alen = 8; }
    ffrdp->dup_seen = 0;
    // this sendto used to be unchecked, which made an ack the socket refused indistinguishable from an ack
    // the network ate -- and the peer pays for both with a timeout. counting it is the whole change: the
    // ack is still attempted, and nothing here acts on the result.
    if (sendto(ffrdp->udp_fd, (void*)data, alen, 0, (struct sockaddr*)dstaddr, sizeof(struct sockaddr_in)) != alen) ffrdp->counter_send_err++; // send ack frame
}

enum { CEVENT_ACK_OK, CEVENT_ACK_TIMEOUT, CEVENT_FAST_RESEND, CEVENT_SEND_FAILED };
static void ffrdp_congestion_control(FFRDPCONTEXT *ffrdp, int event)
{
    switch (event) {
    case CEVENT_ACK_OK:
        if (ffrdp->cwnd < ffrdp->ssthresh) ffrdp->cwnd *= 2;
        else ffrdp->cwnd++;
        ffrdp->cwnd = MIN(ffrdp->cwnd, FFRDP_MAX_CWND_SIZE);
        ffrdp->cwnd = MAX(ffrdp->cwnd, FFRDP_MIN_CWND_SIZE);
        break;
    case CEVENT_ACK_TIMEOUT:
    case CEVENT_SEND_FAILED:
        ffrdp->ssthresh = MAX(ffrdp->cwnd / 2, FFRDP_MIN_CWND_SIZE);
        ffrdp->cwnd     = FFRDP_MIN_CWND_SIZE;
        break;
    case CEVENT_FAST_RESEND:
        ffrdp->ssthresh = MAX(ffrdp->cwnd / 2, FFRDP_MIN_CWND_SIZE);
        ffrdp->cwnd     = ffrdp->ssthresh;
        break;
    }
}

void ffrdp_update(void *ctxt)
{
    FFRDPCONTEXT       *ffrdp   = (FFRDPCONTEXT*)ctxt;
    FFRDP_FRAME_NODE   *node    = NULL, *p = NULL, *t = NULL;
    struct sockaddr_in *dstaddr = NULL, srcaddr;
    socklen_t addrlen = sizeof(srcaddr);
    int32_t   una, mack, ret, got_data = 0, got_query = 0, got_timeout = 0, send_una, send_mack = 0, recv_una, dist, maxack, i, j;
    uint8_t   data[8];

    if (!ctxt) return;
    // how fast the application drives us: a starved update loop delays acks and retransmits
    // exactly like a congested network does, and the two need to be told apart.
    ffrdp->counter_upd_cnt++;
    if (ffrdp->tick_last_upd) {
        uint32_t gap = get_tick_count() - ffrdp->tick_last_upd;
        ffrdp->counter_upd_gap += gap;
        if (gap > ffrdp->counter_upd_gap_max) ffrdp->counter_upd_gap_max = gap;
    }
    ffrdp->tick_last_upd = get_tick_count();
    dstaddr  = ffrdp->flags & FLAG_SERVER ? &ffrdp->client_addr : &ffrdp->server_addr;
    send_una = ffrdp->send_list_head ? GET_FRAME_SEQ(ffrdp->send_list_head) : 0;
    recv_una = ffrdp->recv_seq;

    if (ffrdp->cur_new_node && ((int32_t)get_tick_count() - (int32_t)ffrdp->cur_new_tick > FFRDP_FLUSH_TIMEOUT || ffrdp->flags & FLAG_FLUSH)) {
        ffrdp->cur_new_node->data[0] = FFRDP_FRAME_TYPE_SHORT;
        ffrdp->cur_new_node->size    = 4 + ffrdp->cur_new_size;
        list_enqueue(&ffrdp->send_list_head, &ffrdp->send_list_tail, ffrdp->cur_new_node);
        ffrdp->send_seq++; ffrdp->wait_snd++;
        ffrdp->cur_new_node = NULL;
        ffrdp->cur_new_size = 0;
    }

    {   // count what is really on the wire, fresh every pass, so no counter can drift or leak
        FFRDP_FRAME_NODE *q;
        for (ffrdp->inflight_used = 0, q = ffrdp->send_list_head; q; q = q->next) if (q->flags & FLAG_FIRST_SEND) ffrdp->inflight_used++;
    }
    for (i = 0, p = ffrdp->send_list_head; i < (int32_t)ffrdp->cwnd && p; i++, p = p->next) {
        if (!(p->flags & FLAG_FIRST_SEND)) { // first send
            // frames are first sent in sequence order, so everything already sent sits in front of this
            // one and breaking here skips nothing that needed attention. retransmits are untouched: this
            // only stops adding new frames to a wire that already holds FFRDP_INFLIGHT_MAX of them.
            if (ffrdp->inflight_used >= MAX(ffrdp->inflight_max ? ffrdp->inflight_max : (uint32_t)FFRDP_INFLIGHT_MAX, (uint32_t)FFRDP_INFLIGHT_MIN)) break;
            if (ffrdp->swnd > 0) {
                if (ffrdp_send_data_frame(ffrdp, p, dstaddr) != 0) { ffrdp_congestion_control(ffrdp, CEVENT_SEND_FAILED); break; }
                p->tick_1sts = p->tick_send = get_tick_count();
                p->tick_timeout = p->tick_send + ffrdp->rto;
                p->flags       |= FLAG_FIRST_SEND;
                ffrdp->swnd--; ffrdp->counter_send_1sttime++; ffrdp->win_first++; ffrdp->inflight_used++;
            } else if (ffrdp->tick_send_query == 0 || (int32_t)get_tick_count() - (int32_t)ffrdp->tick_send_query > FFRDP_QUERY_CYCLE) { // query remote receive window size
                data[0] = FFRDP_FRAME_TYPE_QUERY; if (sendto(ffrdp->udp_fd, (void*)data, 1, 0, (struct sockaddr*)dstaddr, sizeof(struct sockaddr_in)) != 1) ffrdp->counter_send_err++;
                ffrdp->tick_send_query = get_tick_count(); ffrdp->counter_send_query++;
                break;
            }
        } else if ((p->flags & FLAG_FIRST_SEND) && ((int32_t)get_tick_count() - (int32_t)p->tick_timeout > 0 || (p->flags & FLAG_FAST_RESEND))) { // resend
            ffrdp_congestion_control(ffrdp, CEVENT_ACK_TIMEOUT);
            if (ffrdp_send_data_frame(ffrdp, p, dstaddr) != 0) break;
            if (!(p->flags & FLAG_FAST_RESEND)) {
                if (p == ffrdp->send_list_head) ffrdp->counter_rto_head++; else ffrdp->counter_rto_chain++;
                // was this frame already reported as received (una passed it) when the timer fired?
                // stale: we were told, but had not removed it -> the retransmit is pure waste.
                if (seq_distance(GET_FRAME_SEQ(p), (uint32_t)ffrdp->ack_una) < 0) ffrdp->counter_rto_stale++;
                else ffrdp->counter_rto_behind++;
                if (ffrdp->rto == FFRDP_MAX_RTO) {
                    p->tick_send = get_tick_count();
                    p->flags    &=~FLAG_TIMEOUT_RESEND;
                } else p->flags |= FLAG_TIMEOUT_RESEND;
                if ((int32_t)get_tick_count() - (int32_t)ffrdp->tick_rto_backoff >= (int32_t)ffrdp->rto) { // rto backs off once per round, not once per frame
                    ffrdp->tick_rto_backoff = get_tick_count();
                    if (ffrdp->rto == FFRDP_MAX_RTO) ffrdp->counter_reach_maxrto++;
                    ffrdp->rto += ffrdp->rto / 2;
                    ffrdp->rto  = MIN(ffrdp->rto, FFRDP_MAX_RTO);
                    ffrdp->counter_rto_backoff++;
                }
                ffrdp->counter_resend_rto++;
                ffrdp->win_need++;
                got_timeout = 1;
            } else {
                p->flags &= ~(FLAG_FAST_RESEND|FLAG_TIMEOUT_RESEND);
                // count fast retransmits in the same evidence window as the timeout ones: they are
                // retransmits like any other, and on a link with reordering they are where most of the
                // waste sits (the peer confirms almost all of them as duplicates).
                ffrdp->counter_resend_fast++;
                ffrdp->win_need++;
            }
            p->tick_timeout+= ffrdp->rto;
        }
    }
    if (got_timeout) ffrdp->counter_timeout_evt++;

    if (ffrdp_sleep(ffrdp, FFRDP_SELECT_SLEEP) != 0) return;
    for (node = NULL;;) { // receive data
        if (!node && !(node = frame_node_new(FFRDP_FRAME_TYPE_FEC2, FFRDP_MAX_RX_MSS))) break;   // 收包节点按**收** MSS 建
        if ((ret = recvfrom(ffrdp->udp_fd, (void*)node->data, node->size, 0, (struct sockaddr*)&srcaddr, &addrlen)) <= 0) break;
        if ((ffrdp->flags & FLAG_SERVER) && (ffrdp->flags & FLAG_CONNECTED) == 0) {
            if (ffrdp->flags & FLAG_CONNECTED) {
                if (memcmp(&srcaddr, &ffrdp->client_addr, sizeof(srcaddr)) != 0) continue;
            } else {
                ffrdp->flags |= FLAG_CONNECTED;
                memcpy(&ffrdp->client_addr, &srcaddr, sizeof(ffrdp->client_addr));
            }
        }

        if (node->data[0] <= FFRDP_FRAME_TYPE_FEC32) { // data frame
            node->size = ret; // frame size is the return size of recvfrom
            if (ffrdp_recv_data_frame(ffrdp, node) == 0) {
                dist = seq_distance(GET_FRAME_SEQ(node), recv_una);
                if (dist == 0) { recv_una++; }
                // out of order arrival = this frame is ahead of a hole. If this counter grows while the
                // peer's acks carry no mack bit, then the hole is never being reported, and the sender
                // has no choice but to wait for timeouts.
                if (dist > 0) {
                    ffrdp->counter_recv_ooo++;
                    if (dist > 23) ffrdp->counter_recv_far++;
                    // how far ahead of the hole this one was. the distance only ever shrinks by the hole
                    // being filled, and by then this frame is below una and gets acked cumulatively
                    // instead, so a frame further than the mask's last bit is never reported at all --
                    // which is why the band it falls in decides whether a wider mask would buy anything.
                    if      (dist <= 24) ffrdp->counter_ooo_d1_24++;
                    else if (dist <= 64) ffrdp->counter_ooo_d25_64++;
                    else {   // > 64: the tail one extension word cannot carry, split by how far it goes
                        ffrdp->counter_ooo_dgt64++;
                        if (dist <= 128) ffrdp->counter_ooo_d65_128++;
                        else             ffrdp->counter_ooo_dgt128++;
                    }
                }
                // below the range we have already taken in: the peer sent this again although we had it,
                // which is exactly the waste we want to count and report back.
                if (dist < 0) { ffrdp->counter_recv_dup++; ffrdp->dup_seen++; }
                // further ahead than the window we hand out in every ack: the peer is credit limited to
                // that, so this is a lie or a duplicate. storing it would let anyone who can reach the
                // port spend our memory at one frame per datagram, for ever (see FFRDP_RECV_QUEUE_MAX).
                if (dist >= (int32_t)FFRDP_RECV_QUEUE_MAX) { ffrdp->counter_recv_drop++; }
                else if (dist >= 0) { list_enqueue(&ffrdp->recv_list_head, &ffrdp->recv_list_tail, node); node = NULL; }
                got_data = 1;
            }
        } else if (node->data[0] == FFRDP_FRAME_TYPE_ACK) { // ack frame
            una  = *(uint32_t*)(node->data + 0) >> 8;
            mack = *(uint32_t*)(node->data + 4) & 0xFFFFFF;
            ffrdp->counter_ack_recv++;
            // the peer's count of our retransmits that it had already accepted. this is the one piece of
            // evidence our own counters cannot fake, so it is what the rto floor is driven by.
            if (ret >= 9 && node->data[8]) { ffrdp->counter_ack_dup += node->data[8]; ffrdp->win_dup += node->data[8]; }
            if (ffrdp->counter_ack_recv > 1 && (uint32_t)una == ffrdp->ack_una) ffrdp->counter_ack_una_dup++; // the peer did not move
            ffrdp->ack_una = (uint32_t)una;
            if (mack) { // the peer reported frames that arrived after a hole
                ffrdp->counter_ack_mack++;
                // counted in bits, to be compared with counter_mack_bits_tx on the other side: a
                // report that never arrived and a report that was never made look identical from here.
                for (j = 0; j < 24; j++) if (mack & (1 << j)) ffrdp->counter_mack_bits_rx++;
                for (i = 23; i >= 0 && !(mack & (1 << i)); i--);
                for (j = 0; j < i && (mack & (1 << j)); j++);
                if (j < i) ffrdp->counter_ack_hole++; // a gap below the highest acked frame
            }
            dist = seq_distance(una, send_una);
            if (dist >= 0) {
                send_una    = una;
                send_mack   = (send_mack >> dist) | mack;
                // the peer's window is a buffer size, not a rate: on the measured link it allowed 148
                // frames where the path carried five, and the rest sat in its buffer as a standing queue
                // of about 200ms. cap is what a probe has proven, and what is being tested right now.
                ffrdp->swnd = node->data[7];
                if (ffrdp->cap && ffrdp->swnd > ffrdp->cap) ffrdp->swnd = ffrdp->cap;
                ffrdp->tick_recv_ack = get_tick_count();
            }
        } else if (node->data[0] == FFRDP_FRAME_TYPE_QUERY) got_query = 1;
    }
    if (node) free(node);

    if (got_data || got_query) ffrdp_recvdata_and_sendack(ffrdp, dstaddr); // send ack frame
    if (ffrdp->send_list_head && seq_distance(send_una, GET_FRAME_SEQ(ffrdp->send_list_head)) >= 0) { // handle frame ack
        for (p = ffrdp->send_list_head; p;) {
            dist = seq_distance(GET_FRAME_SEQ(p), send_una);
            for (i=23; i>=0 && !(send_mack&(1<<i)); i--);
            if (i < 0) maxack = (send_una - 1) & 0xFFFFFF;
            else maxack = (send_una + i + 1) & 0xFFFFFF;

            if (dist > 24 || !(p->flags & FLAG_FIRST_SEND)) break;
            else if (dist < 0 || (dist > 0 && (send_mack & (1 << (dist-1))))) { // this frame got ack
                ffrdp->counter_send_bytes += frame_payload_size(p); ffrdp->wait_snd--;
                ffrdp_congestion_control(ffrdp, CEVENT_ACK_OK);
                if (!(p->flags & FLAG_TIMEOUT_RESEND)) {
                    ffrdp->rttm = (int32_t)get_tick_count() - (int32_t)p->tick_send;
                    // rttm above is only the latest sample despite its name; this is the one that says how
                    // quick the path can be at all, and the one a queue on the path would show up in.
                    if (ffrdp->rtt_min == 0 || ffrdp->rttm < ffrdp->rtt_min) ffrdp->rtt_min = ffrdp->rttm;
                    if (ffrdp->rtts == (uint32_t)-1) {
                        ffrdp->rtts = ffrdp->rttm;
                        ffrdp->rttd = ffrdp->rttm / 2;
                    } else {
                        ffrdp->rtts = (7 * ffrdp->rtts + 1 * ffrdp->rttm) / 8;
                        ffrdp->rttd = (3 * ffrdp->rttd + 1 * abs((int)ffrdp->rttm - (int)ffrdp->rtts)) / 4;
                    }
                    {   // The peer's duplicate reports are the one piece of evidence on this path that our own
                        // retransmit counter cannot corrupt: it says, from the receiving end, how many of the
                        // frames we sent a second time it had already accepted. When that share is high the rto
                        // is still sitting inside the round trip distribution, and every one of those
                        // retransmits is paid for on the wire for nothing -- so push the floor up until they
                        // stop. The floor starts from the current rto when it is lower, so a single step is
                        // immediately binding: a raiser seeded below the rto is exactly what made the earlier
                        // attempts at this do nothing at all.
                        // the decision is taken once per evidence window, and both directions move on that
                        // same clock: raising per window while decaying per ack means the decay wins on any
                        // link with a high ack rate, and the floor then never moves at all.
                        // the evidence window closes on whichever comes first: enough retransmits to speak
                        // with, or two seconds. a window with too little evidence may never *raise* the
                        // floor (a handful of retransmits can read 100% pointless by accident), but it does
                        // let it decay: a link that has gone quiet has nothing a long rto would be
                        // repairing, and leaving the floor up would only hang repair delay on a link that
                        // has become fast again.
                        if (ffrdp->win_need >= FFRDP_RTO_WIN ||
                            (int32_t)get_tick_count() - (int32_t)ffrdp->tick_win >= 2000) {
                            ffrdp->tick_win = get_tick_count();
                            if (ffrdp->win_need >= FFRDP_WASTE_MIN) {
                                uint32_t waste = ffrdp->win_need ? ffrdp->win_dup * 100 / ffrdp->win_need : 0;
                                // the same evidence decides how much to trust a hole the peer reports: when it
                                // calls most of our repairs pointless, those holes are mostly frames that were
                                // overtaken on the way, so the fast path should wait; when the repairs are
                                // real, it must not wait at all.
                                {   uint32_t rtt = (ffrdp->rtts == (uint32_t)-1 || ffrdp->rtts < FFRDP_MIN_RTO) ? FFRDP_MIN_RTO : ffrdp->rtts;
                                    ffrdp->hole_wait = (waste >= FFRDP_WASTE_HI) ? MIN(3 * rtt, 300u)
                                                     : (waste <  FFRDP_WASTE_LO) ? 0 : rtt;
                                }
                                if (waste >= FFRDP_WASTE_HI) {
                                    if (ffrdp->rto_floor < ffrdp->rto) ffrdp->rto_floor = ffrdp->rto;
                                    ffrdp->rto_floor = MIN(ffrdp->rto_floor * 5 / 4, (uint32_t)FFRDP_RTO_FLOOR_MAX);
                                } else if (waste < FFRDP_WASTE_LO) {
                                    ffrdp->rto_floor = 0;      // the retransmits are reaching a peer that did not have them: repair fast
                                } else {
                                    ffrdp->rto_floor = ffrdp->rto_floor * 9 / 10; // in between: give ground slowly
                                }
                            }
                            // The cap is applied on every window, not only when the floor is raised: a floor
                            // that was reasonable while the link had a long round trip must come down by
                            // itself once the link speeds up, even if by then nothing is being retransmitted
                            // any more. Note that a quiet window is *not* fed into the decay path above --
                            // on a rate limited link a quiet window is usually the floor doing its job, and
                            // decaying on it would make the loop undo its own progress.
                            {   uint32_t rtt = (ffrdp->rtts == (uint32_t)-1 || ffrdp->rtts < FFRDP_MIN_RTO) ? FFRDP_MIN_RTO : ffrdp->rtts;
                                uint32_t cap = MAX(3 * rtt, 100u);
                                if (ffrdp->rto_floor > cap) ffrdp->rto_floor = cap;
                            }
                            ffrdp->win_need = ffrdp->win_dup = ffrdp->win_first = 0;
                        }
                        ffrdp->rto = MAX(ffrdp->rtts + 4 * ffrdp->rttd, ffrdp->rto_floor);
                        ffrdp->rto = MAX(FFRDP_MIN_RTO, ffrdp->rto);
                        ffrdp->rto = MIN(FFRDP_MAX_RTO, ffrdp->rto);
                    }
                }
                t = p; p = p->next; list_remove(&ffrdp->send_list_head, &ffrdp->send_list_tail, t); ffrdp->counter_delivered++; continue;
            } else if (seq_distance(maxack, GET_FRAME_SEQ(p)) > 0) {
                // the peer reported something above this frame, so it is either lost or merely late.
                // how long to wait before believing it is not a guess: it comes from the peer's own
                // duplicate reports (hole_wait, set once per evidence window). on a link where the peer
                // keeps confirming our retransmits as duplicates, holes are usually just frames that were
                // overtaken on the way, so wait; where the retransmits are really repairing something,
                // hole_wait is zero and this reacts exactly as before.
                if ((int32_t)get_tick_count() - (int32_t)p->tick_send >= (int32_t)ffrdp->hole_wait) {
                    ffrdp_congestion_control(ffrdp, CEVENT_FAST_RESEND);
                    p->flags |= FLAG_FAST_RESEND;
                }
            }
            p = p->next;
        }
    }
    {   // delay probe: take half the in-flight away for a moment and see whether the round trip follows.
        // if it does and the confirmed rate does not, the delay was our own queue, so the smaller window
        // is kept and the experiment repeats later; if the delay does not follow, it belongs to the path
        // and the window goes straight back. that asymmetry is the whole safety of this: a link that
        // loses frames rather than queueing them fails the first test and is left exactly as it was.
        uint32_t now = get_tick_count();
        FFRDP_FRAME_NODE *q;
        if (!ffrdp->probe_gap) ffrdp->probe_gap = FFRDP_PROBE_GAP_MS;
        // the probe now drives the real brake (frames on the wire), not the peer's credit: the credit is
        // handed out again on every ack, so limiting it never bound anything and the experiment could not
        // change the round trip even in principle. without a proven cap the ceiling stays at its default.
        ffrdp->inflight_max = ffrdp->probe_state ? ffrdp->probe_cap
                            : (ffrdp->cap_kept ? ffrdp->cap_kept : (uint32_t)FFRDP_INFLIGHT_MAX);
        ffrdp->cap = ffrdp->inflight_max;
        if ((int32_t)now - (int32_t)ffrdp->rate_tick >= 1000) {
            uint32_t resend = ffrdp->counter_resend_rto + ffrdp->counter_resend_fast;
            uint32_t firsts = ffrdp->counter_send_1sttime;
            ffrdp->rate_now     = ffrdp->counter_delivered - ffrdp->rate_del_ref;
            ffrdp->rate_tick    = now;
            ffrdp->rate_del_ref = ffrdp->counter_delivered;
            // the trigger needs evidence over a whole second. the window the rto floor uses holds a few
            // tens of frames, and one retransmit out of thirty is a ratio of three percent -- that is
            // sampling noise, not a wasteful path. the counters can also be cleared by a dump, so a
            // count that moved backwards is read as no evidence at all.
            ffrdp->probe_waste = (resend >= ffrdp->probe_resend_seen && firsts >= ffrdp->probe_first_seen)
                               ? (resend - ffrdp->probe_resend_seen) * 1000 / MAX(firsts - ffrdp->probe_first_seen, 1)
                               : 0;
            ffrdp->probe_resend_seen = resend;
            ffrdp->probe_first_seen  = firsts;
        }
        if (ffrdp->probe_state == 0) {
            uint32_t waste = ffrdp->probe_waste;
            if ((int32_t)now - (int32_t)ffrdp->probe_next >= 0) {
                uint32_t out = 0, cap_want, holding, idle;
                // count only frames that are really on the wire: the list also holds frames the
                // application has queued but that have not been sent yet, and halving those would not
                // halve what is in flight at all.
                for (q = ffrdp->send_list_head; q; q = q->next) if (q->flags & FLAG_FIRST_SEND) out++;
                // what earns an experiment is a path that is holding a queue, and rtt_min now makes that
                // visible directly: typical round trip far above the path's floor is a queue by definition,
                // and it is the delay a ping sees while the transfer runs. a retransmit count is a much
                // weaker trigger -- on this path it is down to a few per mille once the floor is up -- so it
                // stays only as a second way in for paths whose floor is already low.
                // the size gate used to be "at least 8 frames on the wire", read as "a pipe worth halving",
                // and it acted as a one way ratchet: with the ceiling already down at 4 or 5 there are never
                // 8 frames to see, so the probe could not ask again -- while the giveback below, whose job
                // is to notice a path that got faster, lifts a proven 4 to 5 and leaves it there. measured
                // state on the target path: inflight, cap = 5, 5, holding 4ms of queue over what the very
                // same path held at 4. what is worth asking is not a big pipe but a ceiling that can still
                // come down, so the gate is the halved ceiling being strictly under the one in force, and
                // the floor of FFRDP_INFLIGHT_MIN is where the descent legitimately ends.
                cap_want = MAX(out / 2, (uint32_t)FFRDP_INFLIGHT_MIN);
                // "is the path holding a queue" is asked twice below, but with two DIFFERENT yardsticks --
                // and that is what the measured sweep of 2026-10-10 taught: the ratio is a good gate for
                // starting an experiment and a bad one for ending it. On the target path rtts/rtt_min sat
                // at 3x in 18 of 29 dumps, at 4x in 8 and under 3x in 2, so a giveback gated on it opens
                // and closes from one check to the next; seven givebacks slipped through in 300 seconds,
                // each lifting a proven 4 back to 5 (once to 6), and each one pushing the next probe a whole
                // FFRDP_PROBE_GAP_MS away. The window therefore spent the run at 5 and measured rtts 17ms,
                // where the very same path pinned at 4 measured 11ms. So the trigger keeps the ratio (3x is
                // where a queue becomes worth a question) and the giveback needs the queue to be genuinely
                // gone, judged in milliseconds -- the same scale as the keep test it exists to undo.
                holding = (ffrdp->rtt_min && ffrdp->rtts != (uint32_t)-1 &&
                           ffrdp->rtts > FFRDP_PROBE_QUEUE_RATIO * ffrdp->rtt_min) ? 1u : 0u;
                idle    = (ffrdp->rtt_min && ffrdp->rtts != (uint32_t)-1 &&
                           ffrdp->rtts < ffrdp->rtt_min + FFRDP_PROBE_RTT_DROP_MS) ? 1u : 0u;
                if (FFRDP_PROBE_ENABLE && out > FFRDP_INFLIGHT_MIN && cap_want < ffrdp->inflight_max &&
                    (holding || waste >= FFRDP_PROBE_WASTE_PM)) {
                    uint32_t rtt = (ffrdp->rtts == (uint32_t)-1 || ffrdp->rtts < FFRDP_MIN_RTO) ? FFRDP_MIN_RTO : ffrdp->rtts;
                    ffrdp->probe_state   = 1;
                    ffrdp->probe_tick    = now;
                    ffrdp->probe_hold    = 2 * rtt + FFRDP_PROBE_HOLD_MS;
                    ffrdp->probe_cap     = cap_want;
                    // the reference is the TYPICAL round trip, and the experiment tracks that same
                    // estimate, so the two numbers being subtracted come from one filter -- which is what
                    // makes a two millisecond difference readable at all. rttm is only a fallback, for a
                    // connection too young to have a typical yet.
                    ffrdp->probe_rtt_ref = ffrdp->rtts != (uint32_t)-1 ? ffrdp->rtts : ffrdp->rttm;
                    ffrdp->probe_rate_ref= ffrdp->rate_now;
                    ffrdp->probe_best    = 0xFFFFFFFF;
                    ffrdp->probe_del_ref = ffrdp->counter_delivered;
                    ffrdp->cap           = ffrdp->probe_cap;
                    ffrdp->counter_probe++;
                } else if (waste < FFRDP_PROBE_HEALTHY_PCT && ffrdp->cap_kept && idle) {
                    // nothing is being wasted any more, and the queue is genuinely gone (idle, the absolute
                    // test above -- not the ratio, which sits right at its own threshold on this path): give
                    // a little of the window back, so a path that has become faster is not held down for
                    // ever by a limit proven on an older path. the idle half is not decoration: a giveback
                    // schedules the next look a whole FFRDP_PROBE_GAP_MS away, so giving back while a queue
                    // is still there parks a proven 4 at 5 for ten seconds at a time and hands back the win
                    // the probe just measured -- measured: seven times in 300s, which is why the run sat at
                    // 5. a path that got faster is a path whose queue has drained, and that is this test.
                    ffrdp->cap_kept   = MIN(ffrdp->cap_kept + MAX(ffrdp->cap_kept / 4, 1), (uint32_t)FFRDP_INFLIGHT_MAX);
                    ffrdp->cap        = ffrdp->cap_kept;
                    ffrdp->probe_next = now + FFRDP_PROBE_GAP_MS;
                    ffrdp->counter_giveback++;
                } else {
                    // nothing to ask about yet: the queue test is not satisfied, so wait a full back off
                    // before looking again. looking sooner was tried and rejected on the measured link: it
                    // moved the first probe into the connection's ramp up, where both rate samples (before
                    // and during) are still climbing, so the rate guard read a false "the rate fell" and
                    // reverted -- even though the delay had plainly followed (probe rtt ref/best = 86/6).
                    // the revert doubles the back off, so asking early cost twenty seconds instead of
                    // saving ten. a rate guard needs a connection that has settled down before it means
                    // anything.
                    ffrdp->probe_next = now + FFRDP_PROBE_GAP_MS;
                }
            }
        } else {
            // track the same estimate the reference came from (an eight sample average, so it settles in
            // roughly a tenth of the hold): comparing rttm's noisiest sample with rtts' typical value was
            // the other half of why this experiment answered at random.
            if (ffrdp->rtts != (uint32_t)-1 && ffrdp->rtts < ffrdp->probe_best) ffrdp->probe_best = ffrdp->rtts;
            if ((int32_t)now - (int32_t)ffrdp->probe_tick >= (int32_t)ffrdp->probe_hold) {
                uint32_t span      = MAX(now - ffrdp->probe_tick, 1);
                uint32_t rate_held = (ffrdp->counter_delivered - ffrdp->probe_del_ref) * 1000 / span;
                // the delay followed if the typical round trip now sits at least FFRDP_PROBE_RTT_DROP_MS
                // under where it stood -- an absolute difference, because that is the size of the thing
                // being measured. a missing reference means a connection with no typical yet, and then
                // nothing has been proven: never keep a smaller window on a missing measurement. the order
                // of the two tests matters: probe_best is the 0xFFFFFFFF "no sample" sentinel until the
                // first update, and it must be rejected before it is subtracted.
                uint32_t rtt_down  = ffrdp->probe_best != 0xFFFFFFFF &&
                                     ffrdp->probe_rtt_ref > ffrdp->probe_best &&
                                     ffrdp->probe_rtt_ref - ffrdp->probe_best >= FFRDP_PROBE_RTT_DROP_MS;
                uint32_t rate_kept = rate_held * 100 >= ffrdp->probe_rate_ref * FFRDP_PROBE_RATE_KEEP_PCT;
                if (rtt_down && rate_kept) {
                    ffrdp->cap_kept  = ffrdp->probe_cap;
                    ffrdp->probe_gap = FFRDP_PROBE_GAP_MS;
                    // the experiment just proved there is more to win, so do not sit out a whole back off
                    // before asking the next question: walk the rest of the descent now. it stays self
                    // limiting -- the first empty answer doubles the gap again -- so a path nobody has
                    // measured converges in a few seconds instead of a few tens of them, and no value has
                    // to be picked for that path in advance.
                    ffrdp->probe_next = now + ffrdp->probe_hold;
                    ffrdp->counter_probe_keep++;
                } else {
                    // the delay did not follow, or it did but the confirming rate fell with it: either way
                    // the smaller window bought nothing, so go back to what was proven and wait longer.
                    if (rtt_down) ffrdp->counter_probe_narrow++;
                    ffrdp->probe_gap  = MIN(ffrdp->probe_gap * 2, (uint32_t)FFRDP_PROBE_GAP_MAX_MS);
                    ffrdp->probe_next = now + ffrdp->probe_gap;
                    ffrdp->counter_probe_revert++;
                }
                ffrdp->probe_state = 0;
                ffrdp->cap         = ffrdp->cap_kept;
            }
        }
    }
    {   // cost of one pass, so a slow application or a slow peer can be told apart from a slow link
        uint32_t cost = get_tick_count() - ffrdp->tick_last_upd;
        if (cost > ffrdp->counter_upd_cost_max) ffrdp->counter_upd_cost_max = cost;
    }
}

void ffrdp_flush(void *ctxt)
{
    FFRDPCONTEXT *ffrdp = (FFRDPCONTEXT*)ctxt;
    if (ffrdp) ffrdp->flags |= FLAG_FLUSH;
}

void ffrdp_dump(void *ctxt, int clearhistory)
{
    FFRDPCONTEXT *ffrdp = (FFRDPCONTEXT*)ctxt; int secs;
    if (!ctxt) return;
    secs = ((int32_t)get_tick_count() - (int32_t)ffrdp->tick_ffrdp_dump) / 1000;
    secs = secs ? secs : 1;
    printf("total_send, total_recv: %.2fMB, %.2fMB\n"    , ffrdp->counter_send_bytes / (1024.0 * 1024), ffrdp->counter_recv_bytes / (1024.0 * 1024));
    // secs 是距上一次 dump 的秒数：刚起来的第一次 dump 可能不足一秒，别拿 0 当除数。
    printf("averg_send, averg_recv: %.2fKB/s, %.2fKB/s\n", ffrdp->counter_send_bytes / (1024.0 * MAX(secs, 1)), ffrdp->counter_recv_bytes / (1024.0 * MAX(secs, 1)));
    printf("rttm: %u, rtts: %u, rttd: %u, rto: %u\n", ffrdp->rttm, ffrdp->rtts, ffrdp->rttd, ffrdp->rto);
    // which binary is this? the compile stamp makes a stale executable impossible to mistake for a fresh
    // one: if this time is older than the build you just ran, you are running something else.
    printf("version & build     : %s probe %d, queue ratio %u, inflight default %u, compiled %s %s\n",
           FFRDP_VERSION, FFRDP_PROBE_ENABLE, (uint32_t)FFRDP_PROBE_QUEUE_RATIO, (uint32_t)FFRDP_INFLIGHT_MAX, __DATE__, __TIME__);
    // rttm is the LATEST sample (its name is misleading); rtt_min is the lowest of this interval, rtts the
    // smoothed typical latency, rttd the jitter. rto is a timer, not a delay: never read it as latency.
    printf("rtt_min, rtts, rttd : %u, %u, %u (min / typical / jitter, ms)\n",
           ffrdp->rtt_min, ffrdp->rtts, ffrdp->rttd);
    printf("elapsed             : %d s (dump interval %d s)\n", ((int32_t)get_tick_count() - (int32_t)ffrdp->tick_ffrdp_init) / 1000, secs);
    printf("recv_size           : %d\n"  , ffrdp->recv_size           );
    printf("flags               : %x\n"  , ffrdp->flags               );
    printf("send_seq            : %u\n"  , ffrdp->send_seq            );
    printf("recv_seq            : %u\n"  , ffrdp->recv_seq            );
    printf("wait_snd            : %u\n"  , ffrdp->wait_snd            );
    printf("rmss, smss          : %u, %u\n"    , ffrdp->rmss, ffrdp->smss);
    printf("swnd, cwnd, ssthresh: %u, %u, %u\n", ffrdp->swnd, ffrdp->cwnd, ffrdp->ssthresh);
    {   // the delay probe: how many frames are really outstanding, what limit is enforced, and what the
        // experiment reported. rtt ref->best is the number to read: if best is well below ref the window
        // was keeping a queue alive; if they are close the delay is the path's and nothing was touched.
        FFRDP_FRAME_NODE *q; uint32_t out = 0;
        for (q = ffrdp->send_list_head; q; q = q->next) if (q->flags & FLAG_FIRST_SEND) out++;
        printf("inflight, cap       : %u, %u (kept %u; probes %u keep %u revert %u too-far %u giveback %u)\n",
               out, ffrdp->cap, ffrdp->cap_kept, ffrdp->counter_probe, ffrdp->counter_probe_keep,
               ffrdp->counter_probe_revert, ffrdp->counter_probe_narrow, ffrdp->counter_giveback);
        // step29: the brake that actually holds. it limits frames on the wire, so the round trip stops
        // carrying a queue we built ourselves; the peer's credit is left alone (it never limited anything).
        printf("in-flight limit     : %u frames (default %u; the probe lowers it when it proves a win)\n",
               ffrdp->inflight_max ? ffrdp->inflight_max : (uint32_t)FFRDP_INFLIGHT_MAX,
               (uint32_t)FFRDP_INFLIGHT_MAX);
        // what the probe's trigger sees at the moment it is asked: waste is retransmits per mille over the
        // last second, out is what is on the wire, and the queue test is the typical round trip against the
        // path's own floor -- that is the gate which actually earns a probe (the old "rttm >= 50ms" line
        // printed here had stopped being a gate when the queue test replaced it). the size gate is "the
        // halved ceiling is still under the one in force and above the floor", so it is read as two numbers:
        // out/2 is what a probe would ask for, and it must land under the limit on the next line.
        printf("probe trigger       : waste %u pm, rtts/rtt_min %u/%u, out %u (needs %u pm, >%ux, out > %u and out/2 under the limit above)\n",
               ffrdp->probe_waste, ffrdp->rtts, ffrdp->rtt_min, out,
               FFRDP_PROBE_WASTE_PM, FFRDP_PROBE_QUEUE_RATIO, (uint32_t)FFRDP_INFLIGHT_MIN);
        // 1x is the floor itself, and that is the number to watch fall when the probe wins. rtts is
        // smoothed across intervals while rtt_min is the lowest of this one, so rtts can legitimately read
        // below the floor; printing that as 0x would read like a fault, so anything at or under it shows 1x.
        printf("queue test          : rtts %u vs rtt_min %u -> %ux (probe needs >%ux, or waste >= %u pm)\n",
               ffrdp->rtts, ffrdp->rtt_min,
               (ffrdp->rtt_min && ffrdp->rtts != (uint32_t)-1 && ffrdp->rtts > ffrdp->rtt_min)
                   ? ffrdp->rtts / ffrdp->rtt_min : 1,
               FFRDP_PROBE_QUEUE_RATIO, FFRDP_PROBE_WASTE_PM);
        // both are the same estimate -- the typical round trip before the probe and its lowest value during
        // it -- so the difference is directly the proof the probe is looking for: a fall of at least
        // FFRDP_PROBE_RTT_DROP_MS keeps the smaller window (if the confirmed rate also held).
        printf("probe rtt ref/best  : %u, %u (typical before, lowest during; a keep needs a fall of >= %u ms)\n",
               ffrdp->probe_rtt_ref, ffrdp->probe_best == 0xFFFFFFFF ? 0 : ffrdp->probe_best,
               (uint32_t)FFRDP_PROBE_RTT_DROP_MS);
        printf("confirmed rate      : %u frames/s\n", ffrdp->rate_now);
    }
    printf("fec_txredundancy    : %d\n"  , ffrdp->fec_txredundancy    );
    printf("fec_rxredundancy    : %d\n"  , ffrdp->fec_rxredundancy    );
    printf("fec_txseq           : %d\n"  , ffrdp->fec_txseq           );
    printf("fec_rxseq           : %d\n"  , ffrdp->fec_rxseq           );
    printf("fec_rxmask          : %08x\n", ffrdp->fec_rxmask          );
    printf("counter_send_1sttime: %u\n"  , ffrdp->counter_send_1sttime);
    printf("counter_send_failed : %u\n"  , ffrdp->counter_send_failed );
    // the other half of "could not send": ffrdp_send counts refusals by the library (queue full), this
    // counts refusals by the kernel (sendto did not take the datagram), over this dump interval. if this
    // is not zero, the send buffer is too small for how we burst -- and the frames affected were retried,
    // so the cost is invisible except here.
    printf("counter_send_err    : %u\n"  , ffrdp->counter_send_err    );
    printf("counter_send_query  : %u\n"  , ffrdp->counter_send_query  );
    printf("counter_resend_rto  : %u\n"  , ffrdp->counter_resend_rto  );
    printf("counter_resend_fast : %u\n"  , ffrdp->counter_resend_fast );
    // the three ratios below each need a denominator that can legitimately be zero (a sender that has
    // not sent anything yet, a receiver-only direction, a sparse sender whose frames were all short).
    // printing a number computed against a made-up denominator of 1 is worse than printing nothing:
    // it reads like a real measurement. so: n/a when the denominator is zero.
    //   ratio  —— 每次首发尝试里有多少变成了重传。不按交付归一，所以"饿着"的发送方会显得很好看；
    //   waste  —— 对端说"这一帧我早就有了"的重传，占我们自己重传总数的比例。这是链路上唯一一个
    //            本端计数污染不了的浪费判据（本端只知道自己重传了几次，不知道对端是否早就有）；
    //   perMB  —— 每交付 1MB 花了多少次重传（按交付归一，比 ratio 可信）。
    {   uint32_t resend = ffrdp->counter_resend_rto + ffrdp->counter_resend_fast;
        // ratio 与 waste 后面都跟着 (分子 / 分母)：分母小的时候（极低频发送）一眼就能看出这个百分比
        // 不能当稳态读。例如限速 1KB/s 时"重传 6 次、累计首发 20 次"会印 30.00%，而它其实只是 6/20；
        // 同理 waste 的 (6 / 6) 说明"对端说我们 6 次重传全部白费"，而不是一个笼统的 100%。
        if (ffrdp->counter_send_1sttime) printf("counter_resend_ratio: %.2f%% (%u / %u)\n",
                                                100.0 * resend / ffrdp->counter_send_1sttime, resend, ffrdp->counter_send_1sttime);
        else                             printf("counter_resend_ratio: n/a\n");
        if (resend)                      printf("counter_resend_waste: %.2f%% (%u / %u)\n",
                                                100.0 * ffrdp->counter_ack_dup / resend, ffrdp->counter_ack_dup, resend);
        else                             printf("counter_resend_waste: n/a\n");
        if (ffrdp->counter_send_bytes)   printf("counter_resend_perMB: %.1f\n"  , resend * 1048576.0 / ffrdp->counter_send_bytes);
        else                             printf("counter_resend_perMB: n/a\n");
    }
    printf("counter_reach_maxrto: %u\n"  , ffrdp->counter_reach_maxrto);
    printf("counter_ack_recv    : %u\n"  , ffrdp->counter_ack_recv    );
    printf("counter_ack_mack    : %u\n"  , ffrdp->counter_ack_mack    );
    printf("counter_ack_hole    : %u\n"  , ffrdp->counter_ack_hole    );
    printf("counter_recv_ooo    : %u\n"  , ffrdp->counter_recv_ooo    );
    printf("counter_recv_far    : %u\n"  , ffrdp->counter_recv_far    );
    // where out of order arrivals sat relative to the hole, and how much of that we told the peer
    // about. the three bands add up to counter_recv_ooo; the reach ratio answers the only question
    // that matters before widening the mask: how much of the out of order traffic is even addressable.
    printf("counter_ooo_d1_24   : %u\n"  , ffrdp->counter_ooo_d1_24   );
    printf("counter_ooo_d25_64  : %u\n"  , ffrdp->counter_ooo_d25_64  );
    printf("counter_ooo_dgt64   : %u\n"  , ffrdp->counter_ooo_dgt64   );
    printf("counter_ooo_d65_128 : %u\n"  , ffrdp->counter_ooo_d65_128 );
    printf("counter_ooo_dgt128  : %u\n"  , ffrdp->counter_ooo_dgt128  );
    {   uint32_t ooo = ffrdp->counter_recv_ooo;
        uint32_t r64  = ffrdp->counter_ooo_d1_24 + ffrdp->counter_ooo_d25_64;
        uint32_t r128 = r64 + ffrdp->counter_ooo_d65_128;
        if (ooo) { printf("counter_ooo_64reach : %.1f%% (%u / %u)\n" , 100.0 * r64  / ooo, r64 , ooo);
                   printf("counter_ooo_128reach: %.1f%% (%u / %u)\n" , 100.0 * r128 / ooo, r128, ooo); }
        else     { printf("counter_ooo_64reach : n/a\n");
                   printf("counter_ooo_128reach: n/a\n"); }
    }
    printf("counter_mack_bits_tx: %u\n"  , ffrdp->counter_mack_bits_tx);
    printf("counter_mack_bits_rx: %u\n"  , ffrdp->counter_mack_bits_rx);
    printf("counter_rto_backoff : %u\n"  , ffrdp->counter_rto_backoff );
    printf("counter_timeout_evt : %u\n"  , ffrdp->counter_timeout_evt );
    printf("counter_ack_una_dup : %u\n"  , ffrdp->counter_ack_una_dup );
    printf("counter_rto_head    : %u\n"  , ffrdp->counter_rto_head    );
    printf("counter_rto_chain   : %u\n"  , ffrdp->counter_rto_chain   );
    printf("counter_rto_stale   : %u\n"  , ffrdp->counter_rto_stale   );
    printf("counter_recv_dup    : %u\n"  , ffrdp->counter_recv_dup    );
    // what was thrown away instead of acted on: bad = a fragment whose length cannot describe anything,
    // drop = a well formed frame further ahead of the hole than the window we advertise. On a healthy
    // run both stay at zero; a non zero one means somebody is probing the port.
    printf("counter_rx_bad      : %u\n"  , ffrdp->counter_rx_bad      );
    printf("counter_recv_drop   : %u\n"  , ffrdp->counter_recv_drop   );
    printf("counter_ack_dup     : %u\n"  , ffrdp->counter_ack_dup     );
    printf("rto_floor           : %u\n"  , ffrdp->rto_floor); // 白费率见上面的 counter_resend_waste（同一个量，不再打两遍）
    printf("counter_rto_behind  : %u\n"  , ffrdp->counter_rto_behind  );
    printf("update period avgmax: %u, %u ms (%u calls, cost max %u ms)\n",
           ffrdp->counter_upd_gap / MAX(ffrdp->counter_upd_cnt - 1, 1), ffrdp->counter_upd_gap_max,
           ffrdp->counter_upd_cnt, ffrdp->counter_upd_cost_max);
    printf("counter_txfull      : %u\n"  , ffrdp->counter_txfull      );
    printf("counter_txshort     : %u\n"  , ffrdp->counter_txshort     );
    printf("counter_rxfull      : %u\n"  , ffrdp->counter_rxfull      );
    printf("counter_rxshort     : %u\n"  , ffrdp->counter_rxshort     );
    printf("counter_fec_tx      : %u\n"  , ffrdp->counter_fec_tx      );
    printf("counter_fec_rx      : %u\n"  , ffrdp->counter_fec_rx      );
    printf("counter_fec_ok      : %u\n"  , ffrdp->counter_fec_ok      );
    printf("counter_fec_failed  : %u\n\n", ffrdp->counter_fec_failed  );
    if (secs > 1 && clearhistory) {
        ffrdp->tick_ffrdp_dump = get_tick_count();
        memset(&ffrdp->counter_send_bytes, 0, (uint8_t*)&ffrdp->reserved - (uint8_t*)&ffrdp->counter_send_bytes);
    }
}
