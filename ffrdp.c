#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "ffrdp.h"
#include "utils.h"
#include "lowlevel.h"
#include <pthread.h>

#ifdef CONFIG_ENABLE_AES256
#include <openssl/aes.h>
#endif

#ifdef WIN32
// WSAStartup / WSACleanup 还在这里用（见 ffrdp_init / ffrdp_free），所以 winsock2.h 仍然需要。
#include <winsock2.h>
#define get_tick_count GetTickCount
#else
#include <time.h>
// 这里原来还有一整块 socket 时代的东西：winsock2.h、usleep、socklen_t、SOCKET / closesocket / stricmp /
// strtok_s 这些可移植别名、fcntl/sys-types/sys-socket/arpa-inet/netinet 这几个头。协议不再碰 socket 之后
// 现在它们一个都用不到了 —— 真正的 socket 在 lowlevel-udp.c + socket.c 里，那些别名也在它们自己的头里
// （socket.h）。唯一不能动的是下面这个时钟：协议到处在用它取时间，`ts.tv_nsec` 那一行也是差分仿真用来
// 把时钟虚拟化的锚点。
static uint32_t get_tick_count()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
#endif

// 帧的固定开销：4 字节头（类型/长度）+ FEC 帧再加 2 字节 fec seq。见 frame_node_new / frame_payload_size。
#define FFRDP_FRAME_HEAD      4
#define FFRDP_FRAME_FEC_TAIL  2
#define FFRDP_FRAME_OVERHEAD  (FFRDP_FRAME_HEAD + FFRDP_FRAME_FEC_TAIL)
// 单帧载荷上限：由传输层的一帧上限派生（**不再写死 1500**）——减掉最大帧开销，再向下对齐到 4 字节。
// 1500 - 6 = 1494 ⇒ &~3 = 1492，与旧值 (1500 - 8) 相同 ⇒ 这次只是把来源写清、行为不变。
#define FFRDP_MAX_MSS       ((FFRDP_LOWLEVEL_FRAME_MAX - FFRDP_FRAME_OVERHEAD) & ~3)
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

#define FFRDP_RECVBUF_SIZE  (128 * (FFRDP_MAX_MSS + 0))
// how far ahead of the hole a received frame may sit before it is dropped instead of stored. The window
// we advertise travels in a one byte field of the ack, so it is never more than 255 frames, and a peer
// that respects its credit cannot be further ahead than that. Anything beyond it is a lie or a
// duplicate, and storing it is how one stranger - with no session, no handshake and one datagram per
// frame - grows this process's memory without limit (measured before this bound existed: 4000
// datagrams, 4000 frames kept).
#define FFRDP_RECV_QUEUE_MAX 256
// socket 的缓冲区大小、等待方式（select/usleep）都随 socket 一起搬进了 lowlevel：协议这一层不再有
// "等多久"这个东西，它只声明"我下次要在什么时刻被叫"（lowlevel.h 的 timer）。

// 应用侧等待时，即使调用方没给期限也要周期性醒来一次：这样 close 之类的状态变化不会被漏掉，也让
// 调用方看到一个"还在等"的循环，而不是一次永久睡眠。
#define FFRDP_COND_WAIT_MS      200

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
    // 应用交给协议的数据（4 字节长度前缀 + 数据），协议在 tick 里取出并成帧。
    // 为什么要过这一道：成帧要碰的全是协议状态（cur_new_node / send_seq / 加密密钥），让应用线程直接
    // 去写它，就等于把协议的一半搬到应用线程上。放在这里之后，协议侧只被一个执行者碰；下一步协议搬
    // 到 lowlevel 的工作线程上时，这个队列就是两条线程之间唯一的界面（那时只需要给它加一把锁）。
    uint8_t *app_tx_buff;         // 运行时按 smss 分配，容量见 app_tx_cap
    uint32_t app_tx_cap;
    uint32_t app_tx_size;
    int32_t  recv_size, recv_head, recv_tail;
    #define FLAG_SERVER    (1 << 0)
    #define FLAG_CONNECTED (1 << 1)
    #define FLAG_FLUSH     (1 << 2)
    #define FLAG_TX_AES256 (1 << 3)
    #define FLAG_RX_AES256 (1 << 4)
    #define FLAG_ACK_DUE   (1 << 5) // 收到过需要应答的帧、还没回；由 tick 在这一段结束时统一回一个
    uint32_t flags;
    // ---- 上层边界 ----
    // 协议与应用线程只在下面这些东西上见面：一个发送字节队列（应用写、协议读）、一个接收字节队列
    // （协议写、应用读）、以及几个"协议发布出来给应用看"的镜像量。协议自己的状态（send_list /
    // inflight / rto / 各个计数器……）仍然只被一个执行者碰 —— 这正是这次搬家不需要给协议加锁的原因。
    FFRDP_LOWLEVEL   *lowlevel;      // 传输层：socket/收包/定时都在它自己的工作线程里
    uint64_t          peer_addr;     // 对端地址：客户端建连时从参数解出，服务端从第一个收到的包学会
    uint32_t          app_wait_snd;  // 协议发布的在途帧数：应用的受理判据用它，而不是自己去看协议
    int               closing;       // 应用说要关：让等待中的应用线程醒来并返回
    pthread_mutex_t   lock;          // 保护 app_tx_* / recv_buff+recv_size/head/tail / peer_addr / flags / 上列几个
    pthread_cond_t    cond;          // 协议侧腾出空间或送来数据时叫醒应用侧

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
    uint8_t  fec_rxbuf[4 + FFRDP_MAX_MSS + 2];
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
    uint32_t counter_send_bytes;   // payload of the frames the peer acked: bytes DELIVERED, not bytes put on the wire
    uint32_t counter_recv_bytes;   // ... of the frames we acked
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
    uint32_t counter_mack_bits_sent; // mack bits we put on the wire
    uint32_t counter_mack_bits_recv; // mack bits the peer sent us
    uint32_t rtt_min            ; // lowest round trip seen this interval: the path's floor, the number to judge latency by
    uint32_t inflight_used      ; // frames actually on the wire (recounted every pass): what step29 limits

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

// 原来这里有一个 ffrdp_sleep：select(udp_fd, 10ms) + FLAG_FLUSH 短路。它整个搬进了 lowlevel ——
// "等 socket 可读、按时醒来、立刻发"这三件事现在是传输层的实现，协议只说"下次在 X 时刻叫我"。
// 这不只是搬家：非 fd 的后端（串口、隧道）现在也能等了，而那正是"底层可以不是 UDP"的关键。
static int ffrdp_send_data_frame(FFRDPCONTEXT *ffrdp, FFRDP_FRAME_NODE *frame)
{
    switch (frame->size - ffrdp->smss) {
    case 6 : ffrdp->counter_fec_tx ++; *(uint16_t*)(frame->data + 4 + ffrdp->smss) = ffrdp->fec_txseq++; break; // tx fec frame
    case 4 : ffrdp->counter_txfull ++; break; // tx full  frame
    default: ffrdp->counter_txshort++; break; // tx short frame
    }
    // 0 = 传输层收下了这一帧；-1 = 它没收（非阻塞发送缓冲满、或链路报错）。协议不区分这两者：帧已经
    // 不在它手里了，丢没丢由上面的重传负责 —— 与旧版 sendto 失败的语义一致。
    if (ffrdp->lowlevel->send(ffrdp->lowlevel, ffrdp->peer_addr, (uint8_t*)frame->data, (int)frame->size) != 0) { ffrdp->counter_udpsenderr++; ffrdp->counter_send_err++; return -1; }
    else ffrdp->counter_udpsenderr = 0;
    if (frame->size == 4 + ffrdp->smss + 2) { // fec frame
        uint32_t *psrc = (uint32_t*)frame->data, *pdst = (uint32_t*)ffrdp->fec_txbuf, i;
        for (i = 0; i < (4 + ffrdp->smss) / sizeof(uint32_t); i++) *pdst++ ^= *psrc++; // make xor fec frame
        if (ffrdp->fec_txseq % ffrdp->fec_txredundancy == ffrdp->fec_txredundancy - 1) {
            *(uint16_t*)(ffrdp->fec_txbuf + 4 + ffrdp->smss) = ffrdp->fec_txseq++; ffrdp->fec_txbuf[0] = ffrdp->fec_txredundancy;
            // count it like any other send: a redundancy frame the socket refused is a repair that will
            // not happen, and it used to leave no trace anywhere.
            if (ffrdp->lowlevel->send(ffrdp->lowlevel, ffrdp->peer_addr, (uint8_t*)ffrdp->fec_txbuf, (int)frame->size) != 0) ffrdp->counter_send_err++; // send fec frame
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
        ffrdp->fec_rxseq = fecseq; ffrdp->fec_rxmask = 1 << (fecseq % fecrdc); ffrdp->fec_rxcnt = 1; ffrdp->fec_rxredundancy = fecrdc;
        return fecseq % fecrdc != fecrdc - 1 ? 0 : -1;
    } else ffrdp->fec_rxseq = fecseq; // group not changed
    if (fecseq % fecrdc == fecrdc - 1) { // it's redundance frame
        if (ffrdp->fec_rxcnt == fecrdc - 1) return -1;
        if (ffrdp->fec_rxcnt != fecrdc - 2) { ffrdp->counter_fec_failed++; return -1; }
        type = frame->data[0];
        psrc = (uint32_t*)ffrdp->fec_rxbuf; pdst = (uint32_t*)frame->data;
        for (i = 0; i < (frame->size - 2) / sizeof(uint32_t); i++) *pdst++ ^= *psrc++;
        frame->data[0] = type;
        ffrdp->counter_fec_ok++;
    } else if (!(ffrdp->fec_rxmask & (1 << (fecseq % fecrdc)))) { // update fec_rxbuf
        psrc = (uint32_t*)frame->data; pdst = (uint32_t*)ffrdp->fec_rxbuf;
        for (i = 0; i < (frame->size - 2) / sizeof(uint32_t); i++) *pdst++ ^= *psrc++;
        ffrdp->fec_rxmask |= 1 << (fecseq % fecrdc); ffrdp->fec_rxcnt++;
    }
    return 0;
}

static long ffrdp_lowlevel_cb(void *cbctx, int type, void *buf, int len);   // lowlevel 的工作线程从这里回到协议
void* ffrdp_init(char *ip, int port, char *txkey, char *rxkey, int server, int smss, int sfec)
{
    FFRDPCONTEXT *ffrdp = NULL;
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
    ffrdp->rmss     = FFRDP_MAX_MSS;
    ffrdp->smss     = MAX(1, MIN(smss, FFRDP_MAX_MSS));
    ffrdp->fec_txredundancy = MAX(0, MIN(sfec, FFRDP_FRAME_TYPE_FEC32));
    ffrdp->tick_ffrdp_dump  = get_tick_count();
    ffrdp->tick_ffrdp_init  = ffrdp->tick_ffrdp_dump;
    ffrdp->tick_rto_backoff = ffrdp->tick_ffrdp_dump - ffrdp->rto; // allow the first timeout to backoff
    // 应用队列：容量与旧版"最多 FFRDP_MAX_WAITSND 帧在途"同一量级，按 smss 算
    ffrdp->app_tx_cap = (uint32_t)FFRDP_MAX_WAITSND * (ffrdp->smss + 4);
    if (!(ffrdp->app_tx_buff = calloc(1, ffrdp->app_tx_cap))) goto failed;

    // 传输层：socket、bind、非阻塞、缓冲区大小、收包线程、定时，全在 lowlevel 里。协议只认
    // send / timer / 回调。服务端把 bind 地址交给它；客户端交 "none"（不绑定，由内核在第一次发包时
    // 择源端口，与旧版一致），再请后端把对端地址解析成它自己的形式。
    pthread_mutex_init(&ffrdp->lock, NULL);
    pthread_cond_init(&ffrdp->cond, NULL);
    {
        char params[64], peer[80];
        if (server) {
            ffrdp->flags |= FLAG_SERVER;
            snprintf(params, sizeof(params), "bind:%s:%d", ip ? ip : "0.0.0.0", port);
        } else snprintf(params, sizeof(params), "none");
        ffrdp->lowlevel = ffrdp_udp_lowlevel_init(params, ffrdp_lowlevel_cb, ffrdp);
        if (!ffrdp->lowlevel) {
            printf("failed to start the lowlevel transport !\n");
            goto failed;
        }
        if (!server) {
            // 客户端要先能说话：对端地址的书写形式属于后端，只有它能解析（addr 的布局对协议是黑盒）
        uint32_t peer_ip;
        int      peer_port;
        snprintf(peer, sizeof(peer), "%s:%d", ip ? ip : "127.0.0.1", port);
        // 解析走共享约定（utils）；"对端必须给真端口（0 不算）"是协议这边的策略。
        if (ffrdp_addr_parse_text(peer, &peer_ip, &peer_port, FFRDP_ADDR_DEFAULT_PORT) != 0 || peer_port <= 0) {
                printf("failed to parse peer address \"%s\" !\n", peer);
                goto failed;
            }
        ffrdp->peer_addr = ffrdp_addr_make(peer_ip, (uint16_t)peer_port);
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
    if (ffrdp->lowlevel) { ffrdp->lowlevel->exit(ffrdp->lowlevel); ffrdp->lowlevel = NULL; }
    pthread_cond_destroy(&ffrdp->cond);
    pthread_mutex_destroy(&ffrdp->lock);
    if (ffrdp->app_tx_buff) free(ffrdp->app_tx_buff);
    free(ffrdp);
    return NULL;
}

void ffrdp_free(void *ctxt)
{
    FFRDPCONTEXT *ffrdp = (FFRDPCONTEXT*)ctxt;
    FFRDP_LOWLEVEL *ll;
    if (!ctxt) return;
    // 顺序是有讲究的：先让等待中的应用线程醒过来（它们看到 closing 就返回），再停工作线程。反过来的话，
    // 一个卡在 cond 上的应用线程会等一个再也不会有人推进的队列，而 exit 又 join 工作线程 —— 两边互等。
    // exit 必须在锁外调用：它要 join 的正是那个每次回调都要拿这把锁的线程。
    pthread_mutex_lock(&ffrdp->lock);
    ffrdp->closing = 1;
    pthread_cond_broadcast(&ffrdp->cond);
    ll = ffrdp->lowlevel;
    pthread_mutex_unlock(&ffrdp->lock);
    // 先停线程 + join（socket 是它自己关的，见 lowlevel.h）。**必须在摘掉把手之前**：工作线程此刻可能正在
    // 回调里跑一整段（ffrdp_tick 会经 ffrdp_send_data_frame 用 ffrdp->lowlevel）—— 先前这里先把 lowlevel
    // 置 NULL 再 exit，那把正在跑的 tick 就会在 ffrdp.c:439 对 NULL 取成员，实测 0.17% 的收尾段错误就是这么来的。
    if (ll) ll->exit(ll);
    pthread_mutex_lock(&ffrdp->lock);
    ffrdp->lowlevel = NULL;                  // 线程已停、handle 也已被它自己释放 ⇒ 现在摘掉才安全
    pthread_mutex_unlock(&ffrdp->lock);
    if (ffrdp->app_tx_buff) free(ffrdp->app_tx_buff);
    if (ffrdp->cur_new_node) free(ffrdp->cur_new_node);
    list_free(&ffrdp->send_list_head, &ffrdp->send_list_tail);
    list_free(&ffrdp->recv_list_head, &ffrdp->recv_list_tail);
    pthread_cond_destroy(&ffrdp->cond);
    pthread_mutex_destroy(&ffrdp->lock);
    free(ffrdp);
#ifdef WIN32
    WSACleanup();
    timeEndPeriod(1);
#endif
}

static void ffrdp_tick(FFRDPCONTEXT *ffrdp);            // 一段：到期的重传/探测/速率（原 ffrdp_update 的主体）
static long ffrdp_lowlevel_cb(void *cbctx, int type, void *buf, int len);
static int  ffrdp_frame_message(FFRDPCONTEXT *ffrdp, char *buf, int len);
static void ffrdp_drain_tx(FFRDPCONTEXT *ffrdp);
static int  ffrdp_isdead_locked(FFRDPCONTEXT *ffrdp);
static void ffrdp_dump_locked(FFRDPCONTEXT *ffrdp, int clearhistory);

// 发送对外只有一个入口，语义没有变：**要么整条接受、要么一个字节都不接受**。
// 它只把消息（4 字节长度 + 数据）放进应用队列，成帧留给协议在 tick 里做 —— 这样所有协议状态
// （cur_new_node / send_seq / 加密密钥）都只被协议侧碰，为下一步把协议搬到 lowlevel 线程做好准备。
// timeout_ms：0 = 放不下就直接失败；>0 = 最多等这么久（等待期间跑协议，它会把队列腾出来）；<0 = 等到能放。
int ffrdp_send(void *ctxt, char *buf, int len, int timeout_ms)
{
    FFRDPCONTEXT *ffrdp = (FFRDPCONTEXT*)ctxt;
    uint32_t      t0;
    int           rc = -1;
    if (!ffrdp || !buf || len <= 0) return -1;
    pthread_mutex_lock(&ffrdp->lock);
    // 受理判据与旧版逐字相同：帧数上限（在途 + 这一条要占的帧）算在这里，不在出队时算。区别只在
    // "这个在途帧数从哪来"：协议跑在工作线程里，应用不再读协议自己的 wait_snd，而读它发布的镜像
    // （app_wait_snd，每个回调结束时刷新）—— 值与时序都和旧版一致，但不再越过线程边界。
    if (ffrdp->closing) {
        ffrdp->counter_send_failed++;
        pthread_mutex_unlock(&ffrdp->lock);
        return -1;
    }
    if ((ffrdp->flags & FLAG_SERVER) && (ffrdp->flags & FLAG_CONNECTED) == 0) {
        ffrdp->counter_send_failed++;                // 对端还没连上：与以前一样直接拒绝
        pthread_mutex_unlock(&ffrdp->lock);
        return -1;
    }
    if ((len + ffrdp->smss - 1) / ffrdp->smss + ffrdp->app_wait_snd > FFRDP_MAX_WAITSND) {
        ffrdp->counter_send_failed++;
        pthread_mutex_unlock(&ffrdp->lock);
        return -1;
    }
    t0 = get_tick_count();
    for (;;) {
        if (ffrdp->app_tx_buff && (uint32_t)len + 4 + ffrdp->app_tx_size <= ffrdp->app_tx_cap) {
            uint32_t l = (uint32_t)len;
            memcpy(ffrdp->app_tx_buff + ffrdp->app_tx_size, &l, 4);
            memcpy(ffrdp->app_tx_buff + ffrdp->app_tx_size + 4, buf, (size_t)len);
            ffrdp->app_tx_size += 4 + (uint32_t)len;
            rc = len;                                // 整条进去了：原子
            break;
        }
        if (timeout_ms == 0 || ffrdp->closing) break;
        if (timeout_ms > 0 && (int32_t)(get_tick_count() - t0) >= timeout_ms) break;
        // 放不下而调用方愿意等：睡在 cond 上，让工作线程去成帧、去收 ack 腾空间。旧版是在这里自己跑
        // 一轮协议 —— 那正是"应用被迫驱动协议"的最后一个入口，现在协议由它自己的线程驱动。
        {   struct timespec ts;
            int wait_ms = timeout_ms > 0 ? timeout_ms - (int32_t)(get_tick_count() - t0) : FFRDP_COND_WAIT_MS;
            if (wait_ms <= 0) break;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec  += wait_ms / 1000;
            ts.tv_nsec += (long)(wait_ms % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&ffrdp->cond, &ffrdp->lock, &ts);
        }
    }
    if (rc < 0) ffrdp->counter_send_failed++;
    // 刚排进去的东西不该等下一个期限：叫醒工作线程立刻跑一轮（这是 timer 的第二种用法）。
    if (rc > 0 && ffrdp->lowlevel) ffrdp->lowlevel->timer(ffrdp->lowlevel, get_tick_count());
    pthread_mutex_unlock(&ffrdp->lock);
    return rc;
}

// 把一条消息成帧（原 ffrdp_send 的主体，一字未改）：要么整条被接受，要么一个字节都不接受。
// 帧数上限（FFRDP_MAX_WAITSND）不在这里判 —— 那由 ffrdp_drain_tx 决定要不要继续取，见那里。
static int ffrdp_frame_message(FFRDPCONTEXT *ffrdp, char *buf, int len)
{
    FFRDP_FRAME_NODE *spare = NULL, *node;
    int           n = len, size, need, room;
    if (!ffrdp || !buf || len <= 0) return -1;
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

// 接收：有数据就立刻返回（不会为了凑满 len 而等），没数据时按 timeout_ms 等。
// 等待期间由协议自己驱动（ffrdp_update 里就是在传输上 nap），所以应用不再需要"主循环里调 update"
// 这件事 —— 这个函数就是应用侧的等待点。对端失联时提前返回 0，免得调用方永远等下去。
int ffrdp_recv(void *ctxt, char *buf, int len, int timeout_ms)
{
    FFRDPCONTEXT *ffrdp = (FFRDPCONTEXT*)ctxt;
    uint32_t      t0;
    int           ret = 0;
    if (!ffrdp || !buf || len <= 0) return -1;
    pthread_mutex_lock(&ffrdp->lock);
    t0 = get_tick_count();
    for (;;) {
        ret = MIN(len, ffrdp->recv_size);
        if (ret > 0) {
            ffrdp->recv_head = ringbuf_read(ffrdp->recv_buff, sizeof(ffrdp->recv_buff), ffrdp->recv_head, (uint8_t*)buf, ret);
            ffrdp->recv_size-= ret; ffrdp->counter_recv_bytes += ret;
            break;
        }
        if (timeout_ms == 0 || ffrdp->closing) { ret = 0; break; }
        if (ffrdp_isdead_locked(ffrdp)) { ret = 0; break; }
        if (timeout_ms > 0 && (int32_t)(get_tick_count() - t0) >= timeout_ms) { ret = 0; break; }
        // 睡在 cond 上等协议送数据来 —— 这一步之后，"等"是真正免费的：不再需要应用去驱动协议。
        {   struct timespec ts;
            int wait_ms = timeout_ms > 0 ? timeout_ms - (int32_t)(get_tick_count() - t0) : FFRDP_COND_WAIT_MS;
            if (wait_ms <= 0) { ret = 0; break; }
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec  += wait_ms / 1000;
            ts.tv_nsec += (long)(wait_ms % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&ffrdp->cond, &ffrdp->lock, &ts);
        }
    }
    pthread_mutex_unlock(&ffrdp->lock);
    return ret;
}

int ffrdp_isdead(void *ctxt)
{
    FFRDPCONTEXT *ffrdp = (FFRDPCONTEXT*)ctxt;
    int ret;
    if (!ctxt) return -1;
    pthread_mutex_lock(&ffrdp->lock);
    ret = ffrdp_isdead_locked(ffrdp);
    pthread_mutex_unlock(&ffrdp->lock);
    return ret;
}

static int ffrdp_isdead_locked(FFRDPCONTEXT *ffrdp)
{
    if (!ffrdp->send_list_head) return 0;
    if (ffrdp->send_list_head->flags & FLAG_FIRST_SEND) {
        return (int32_t)get_tick_count() - (int32_t)ffrdp->send_list_head->tick_1sts > FFRDP_DEAD_TIMEOUT;
    } else {
        return (int32_t)ffrdp->tick_send_query - (int32_t)ffrdp->tick_recv_ack > FFRDP_DEAD_TIMEOUT;
    }
}

static void ffrdp_recvdata_and_sendack(FFRDPCONTEXT *ffrdp)
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
    // with counter_mack_bits_recv on the other side: the difference between the two is the part of the
    // reporting that the path ate. a frame can contribute a bit to several acks in a row, so this is
    // not a frame count -- it answers "is the reporting happening at all", which is the question that
    // counter_ack_mack (acks carrying any bit) is too coarse to answer.
    for (i = 0; i < 24; i++) if (recv_mack & (1 << i)) ffrdp->counter_mack_bits_sent++;
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
    if (ffrdp->lowlevel->send(ffrdp->lowlevel, ffrdp->peer_addr, data, alen) != 0) ffrdp->counter_send_err++; // send ack frame
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

// 把应用队列里的消息取出来成帧。帧数上限在这里判（判据与旧版 ffrdp_send 里那条完全相同）：到上限
// 就停下，数据留在队列里 —— 不是拒绝，也不是丢，只是这一轮不取，下一轮还会来。
// 注意这里**不**动 counter_send_failed：那个计数器表示"应用被拒过"，含义不能变（dump 与仿真都看它）。
static void ffrdp_drain_tx(FFRDPCONTEXT *ffrdp)
{
    while (ffrdp->app_tx_size >= 4) {
        uint32_t len;
        memcpy(&len, ffrdp->app_tx_buff, 4);
        if (len == 0 || ffrdp->app_tx_size < 4 + len) {   // 不该发生；真发生了就清空队列，别把它卡死
            ffrdp->app_tx_size = 0;
            return;
        }
        if (ffrdp_frame_message(ffrdp, (char*)ffrdp->app_tx_buff + 4, (int)len) != (int)len) {
            return;                                       // 分配失败：消息留在队列里，下一轮再试
        }
        ffrdp->app_tx_size -= 4 + len;
        if (ffrdp->app_tx_size) {
            memmove(ffrdp->app_tx_buff, ffrdp->app_tx_buff + 4 + len, ffrdp->app_tx_size);
        }
    }
}

// 一批帧里跨帧累积的东西。旧写法在 recvfrom 循环外累积它；现在由调用方持有：
// 循环整批调用时是"一批一份"，RECV 回调里是"一帧一份" —— 语义相同，只是一次处理几帧的差别。
typedef struct {
    int32_t recv_una;   // 本批内预期的接收序号（洞被填上时往前走）
    int32_t send_una;   // 本批内确认到的最小未确认发送序号
    int32_t send_mack;  // 本批内对方向上报告的接收位图（按 seq 右移合并）
    int     got_data;   // 本批收到过数据帧 ⇒ 要回 ack
    int     got_query;  // 本批收到过查询帧 ⇒ 要回 ack
} FFRDP_RX_BATCH;

// 一帧：解析、记账。node 由调用方分配好（data/size 已就绪），这样"复用同一个节点"的旧写法保持
// 不变。返回 1 = 已移交给乱序队列（调用方不要再碰它）；返回 0 = 调用方可以复用或释放它。
static int ffrdp_on_frame(FFRDPCONTEXT *ffrdp, FFRDP_FRAME_NODE *node, int len, FFRDP_RX_BATCH *b)
{
    int32_t una, mack, dist, i, j;
    int     taken = 0;

        if (node->data[0] <= FFRDP_FRAME_TYPE_FEC32) { // data frame
            node->size = len; // frame size is what the transport handed over
            if (ffrdp_recv_data_frame(ffrdp, node) == 0) {
                dist = seq_distance(GET_FRAME_SEQ(node), b->recv_una);
                if (dist == 0) { b->recv_una++; }
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
                else if (dist >= 0) { list_enqueue(&ffrdp->recv_list_head, &ffrdp->recv_list_tail, node); taken = 1; }
                b->got_data = 1;
            }
        } else if (node->data[0] == FFRDP_FRAME_TYPE_ACK) { // ack frame
            una  = *(uint32_t*)(node->data + 0) >> 8;
            mack = *(uint32_t*)(node->data + 4) & 0xFFFFFF;
            ffrdp->counter_ack_recv++;
            // the peer's count of our retransmits that it had already accepted. this is the one piece of
            // evidence our own counters cannot fake, so it is what the rto floor is driven by.
            if (len >= 9 && node->data[8]) { ffrdp->counter_ack_dup += node->data[8]; ffrdp->win_dup += node->data[8]; }
            if (ffrdp->counter_ack_recv > 1 && (uint32_t)una == ffrdp->ack_una) ffrdp->counter_ack_una_dup++; // the peer did not move
            ffrdp->ack_una = (uint32_t)una;
            if (mack) { // the peer reported frames that arrived after a hole
                ffrdp->counter_ack_mack++;
                // counted in bits, to be compared with counter_mack_bits_sent on the other side: a
                // report that never arrived and a report that was never made look identical from here.
                for (j = 0; j < 24; j++) if (mack & (1 << j)) ffrdp->counter_mack_bits_recv++;
                for (i = 23; i >= 0 && !(mack & (1 << i)); i--);
                for (j = 0; j < i && (mack & (1 << j)); j++);
                if (j < i) ffrdp->counter_ack_hole++; // a gap below the highest acked frame
            }
            dist = seq_distance(una, b->send_una);
            if (dist >= 0) {
                b->send_una    = una;
                b->send_mack   = (b->send_mack >> dist) | mack;
                // the peer's window is a buffer size, not a rate: on the measured link it allowed 148
                // frames where the path carried five, and the rest sat in its buffer as a standing queue
                // of about 200ms. cap is what a probe has proven, and what is being tested right now.
                ffrdp->swnd = node->data[7];
                if (ffrdp->cap && ffrdp->swnd > ffrdp->cap) ffrdp->swnd = ffrdp->cap;
                ffrdp->tick_recv_ack = get_tick_count();
            }
        } else if (node->data[0] == FFRDP_FRAME_TYPE_QUERY) b->got_query = 1;
    return taken;
}

// 处理完一段接收之后要做的：把收到的 ack 应用到发送表 —— 释放已确认的帧、更新 rtt/rto 下限、
// 触发快速重传，并在收到数据帧时回一个 ack。旧写法是"整批一次"，RECV 回调里是"一帧一次"。
static void ffrdp_apply_acks(FFRDPCONTEXT *ffrdp, FFRDP_RX_BATCH *b)
{
    FFRDP_FRAME_NODE *p, *t;
    int32_t dist, maxack, i;

    // 不在这里回 ack，只记一笔。lowlevel 的一圈会把当前就绪的包都送上来（有上限），所以"这一轮收到的帧"
    // 就是一个批次；ack 报的是 recv_seq + 位图，第二个 ack 覆盖的帧第一个已经报过了，什么都不多带。
    // 真正发出去的地方是 ffrdp_tick —— 与搬走之前"一次 update 一个 ack"同形。
    if (b->got_data || b->got_query) ffrdp->flags |= FLAG_ACK_DUE;
    if (ffrdp->send_list_head && seq_distance(b->send_una, GET_FRAME_SEQ(ffrdp->send_list_head)) >= 0) { // handle frame ack
        for (p = ffrdp->send_list_head; p;) {
            dist = seq_distance(GET_FRAME_SEQ(p), b->send_una);
            for (i=23; i>=0 && !(b->send_mack&(1<<i)); i--);
            if (i < 0) maxack = (b->send_una - 1) & 0xFFFFFF;
            else maxack = (b->send_una + i + 1) & 0xFFFFFF;

            if (dist > 24 || !(p->flags & FLAG_FIRST_SEND)) break;
            else if (dist < 0 || (dist > 0 && (b->send_mack & (1 << (dist-1))))) { // this frame got ack
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
}


// 协议要 lowlevel 什么时候叫它：把所有"下一次要做的事"折算成一个最早时刻。0 = 没有期限，睡到有包为止。
// 四处期限的来源与上面 tick 的四段一一对应：在途帧的 ack 超时、攒了一半的那一帧该发了、速率采样、
// 以及探测的下一次动作（再加查询对端窗口的周期）。
// "下一件该做的活"最早什么时候到（**不含**欠着对端一个 ack 这件事，那一条由 ffrdp_arm_timer 单独并进来）。
// 0 = 眼下没有别的事。返回 now 的意思是"现在就有活"（有队可发、有帧可首发）。这个判断与 ffrdp_tick 的几段
// 一一对应，所以它同时也是"要不要跑整段"的判据 —— 只有 ack 欠着的时候，跑整段是白花的钱。
static uint32_t ffrdp_next_due(FFRDPCONTEXT *ffrdp)
{
    const FFRDP_FRAME_NODE *q;
    uint32_t now = get_tick_count(), due = 0, x;
    int      pending_first = 0;                            // 发送表里有还没首发出去的帧
    for (q = ffrdp->send_list_head; q; q = q->next) {
        if (!(q->flags & FLAG_FIRST_SEND)) { pending_first = 1; continue; }   // tick_timeout 是绝对时刻
        if (!due || (int32_t)(q->tick_timeout - due) < 0) due = q->tick_timeout;
    }
    if (ffrdp->cur_new_node) { x = ffrdp->cur_new_tick + FFRDP_FLUSH_TIMEOUT; if (!due || (int32_t)(x - due) < 0) due = x; }
    x = ffrdp->rate_tick + 1000;                          if (!due || (int32_t)(x - due) < 0) due = x;
    if (ffrdp->tick_send_query) { x = ffrdp->tick_send_query + FFRDP_QUERY_CYCLE; if (!due || (int32_t)(x - due) < 0) due = x; }
    if (ffrdp->probe_next)      { x = ffrdp->probe_next;                          if (!due || (int32_t)(x - due) < 0) due = x; }
    // 还差一条最容易漏、也最贵的：**现在就有发得出去的东西**（应用排了队，或者发送表里有还没首发的帧），
    // 而且发得出去（有信用、在途数没到限）⇒ 立刻叫一次。
    //   * 少了"应用队列"那一半：应用排完队要等某个 ack 或 RTO 到期才有人来发（实测 50 -> 10 MB/s）；
    //   * 少了"还没首发的帧"那一半：一个 ack 刚把窗口/在途数打开，却没有人立刻把队列里的帧顶上 ——
    //     管线在每个 ack 之后空一段。实测 tick 次数腰斩（45253 -> 22793 / 12s），吞吐同步腰斩。
    // 条件是"发得出去"而不是"有东西要发"：窗口关着时叫醒只是空转，那种情况下一个 ack 自然会来敲门。
    if (ffrdp->app_tx_size || pending_first) {
        uint32_t limit = MAX(ffrdp->inflight_max ? ffrdp->inflight_max : (uint32_t)FFRDP_INFLIGHT_MAX, (uint32_t)FFRDP_INFLIGHT_MIN);
        if (ffrdp->swnd > 0 && ffrdp->inflight_used < limit) due = now;
    }
    if (due && (int32_t)(due - now) < 0) due = now;       // 已经过期 ⇒ 立刻（这也是"快一点"的做法）
    return due;
}

// 协议要 lowlevel 什么时候叫它：上面那些期限，再加上"欠着对端一个 ack"（那件事这一刻就该做）。
static void ffrdp_arm_timer(FFRDPCONTEXT *ffrdp)
{
    uint32_t due;
    if (!ffrdp->lowlevel) return;
    due = ffrdp_next_due(ffrdp);
    if (ffrdp->flags & FLAG_ACK_DUE) due = get_tick_count();
    ffrdp->lowlevel->timer(ffrdp->lowlevel, due);
}

// lowlevel 的入口：它的工作线程在这里回到协议里。同一时刻只会有一路，锁保护整个回调，协议自己的状态
// 因此仍然只被一个线程碰。
static long ffrdp_lowlevel_cb(void *cbctx, int type, void *buf, int len)
{
    FFRDPCONTEXT *ffrdp = (FFRDPCONTEXT*)cbctx;
    long          ret  = 0;
    if (!ffrdp || ffrdp->closing) return -1;
    pthread_mutex_lock(&ffrdp->lock);
    if (type == FFRDP_LOWLEVEL_TYPE_RECV) {
        FFRDP_LOWLEVEL_MSG *msg = (FFRDP_LOWLEVEL_MSG*)buf;
        if (len != (int)sizeof(FFRDP_LOWLEVEL_MSG) || !msg->buf || msg->len <= 0) ret = -1;
        else {
            // 服务端从第一个收到的包学会对端（旧版是在收包循环里做同一件事）；连上之后，别人的包不理。
            if ((ffrdp->flags & FLAG_SERVER) && (ffrdp->flags & FLAG_CONNECTED) == 0) {
                ffrdp->flags |= FLAG_CONNECTED;
                ffrdp->peer_addr = msg->addr;
            }
            if (!(ffrdp->flags & FLAG_SERVER) || (ffrdp->flags & FLAG_CONNECTED)) {
                int n = msg->len;
                FFRDP_FRAME_NODE *node = frame_node_new(FFRDP_FRAME_TYPE_FEC2, FFRDP_MAX_MSS);
                if (!node) ret = -1;
                else {
                    FFRDP_RX_BATCH b;
                    // 传输层一帧最大 FFRDP_LOWLEVEL_FRAME_MAX，接收节点按 MSS 建：夹一下，别越界。
                    if (n > (int)node->size) n = (int)node->size;
                    memcpy(node->data, msg->buf, (size_t)n);
                    b.recv_una  = ffrdp->recv_seq;
                    b.send_una  = ffrdp->send_list_head ? GET_FRAME_SEQ(ffrdp->send_list_head) : 0;
                    b.send_mack = 0; b.got_data = 0; b.got_query = 0;
                    if (!ffrdp_on_frame(ffrdp, node, n, &b)) free(node);   // 没被乱序队列接管 ⇒ 归我们
                    ffrdp_apply_acks(ffrdp, &b);
                }
            }
        }
    } else if (type == FFRDP_LOWLEVEL_TYPE_TIMER) {
        // 别的到期工作都没有、只是欠着对端一个 ack 时，就别跑整段了。整段的开销是给"发送/超时/探测/速率"
        // 付的，收到一批帧就付一次，实测把回环吞吐压掉了两成（tick 次数 15592 -> 24915 / 12s）。
        uint32_t d = ffrdp_next_due(ffrdp);
        if ((ffrdp->flags & FLAG_ACK_DUE) && (!d || (int32_t)(d - get_tick_count()) > 0)) {
            ffrdp->flags &= ~FLAG_ACK_DUE;
            ffrdp_recvdata_and_sendack(ffrdp);
        } else ffrdp_tick(ffrdp);
    }
    ffrdp->app_wait_snd = ffrdp->wait_snd;   // 发布给应用看的镜像（受理判据用）
    ffrdp_arm_timer(ffrdp);
    pthread_cond_broadcast(&ffrdp->cond);    // 可能有应用线程正等着空间或数据
    pthread_mutex_unlock(&ffrdp->lock);
    return ret;
}

// 一段：所有"到期要做的事"。它由 lowlevel 的回调调用（TIMER），跑在它的工作线程上，并且被那一把锁
// 保护着。原来里面的"等 10ms + 把 socket 里的包全收下来"整段被删掉了 —— 收包现在是一帧一次的回调。
static void ffrdp_tick(FFRDPCONTEXT *ffrdp)
{
    FFRDP_FRAME_NODE   *p = NULL;
    int32_t   got_timeout = 0, i;
    uint8_t   data[8];
    // 先取应用排进来的消息并成帧：它们在上一轮 ffrdp_send 时只是被放进了队列。
    ffrdp_drain_tx(ffrdp);
    // how fast the application drives us: a starved update loop delays acks and retransmits
    // exactly like a congested network does, and the two need to be told apart.
    ffrdp->counter_upd_cnt++;
    if (ffrdp->tick_last_upd) {
        uint32_t gap = get_tick_count() - ffrdp->tick_last_upd;
        ffrdp->counter_upd_gap += gap;
        if (gap > ffrdp->counter_upd_gap_max) ffrdp->counter_upd_gap_max = gap;
    }
    ffrdp->tick_last_upd = get_tick_count();

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
                if (ffrdp_send_data_frame(ffrdp, p) != 0) { ffrdp_congestion_control(ffrdp, CEVENT_SEND_FAILED); break; }
                p->tick_1sts = p->tick_send = get_tick_count();
                p->tick_timeout = p->tick_send + ffrdp->rto;
                p->flags       |= FLAG_FIRST_SEND;
                ffrdp->swnd--; ffrdp->counter_send_1sttime++; ffrdp->win_first++; ffrdp->inflight_used++;
            } else if (ffrdp->tick_send_query == 0 || (int32_t)get_tick_count() - (int32_t)ffrdp->tick_send_query > FFRDP_QUERY_CYCLE) { // query remote receive window size
                data[0] = FFRDP_FRAME_TYPE_QUERY; if (ffrdp->lowlevel->send(ffrdp->lowlevel, ffrdp->peer_addr, data, 1) != 0) ffrdp->counter_send_err++;
                ffrdp->tick_send_query = get_tick_count(); ffrdp->counter_send_query++;
                break;
            }
        } else if ((p->flags & FLAG_FIRST_SEND) && ((int32_t)get_tick_count() - (int32_t)p->tick_timeout > 0 || (p->flags & FLAG_FAST_RESEND))) { // resend
            ffrdp_congestion_control(ffrdp, CEVENT_ACK_TIMEOUT);
            if (ffrdp_send_data_frame(ffrdp, p) != 0) break;
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

    // 这里原来是"等 socket 可读 + 把所有待收的包一次收完 + 处理收到的 ack"。收包搬走了：lowlevel 一圈把
    // 当前就绪的包都回调上来（一帧一次），帧的解析在 ffrdp_on_frame、ack 的应用在 ffrdp_apply_acks 里；
    // 而应答本身由下面这一句在这一段结束时统一发出去 —— 位置与旧版一致（发送段之后、速率/探测之前）。
    if (ffrdp->flags & FLAG_ACK_DUE) { ffrdp->flags &= ~FLAG_ACK_DUE; ffrdp_recvdata_and_sendack(ffrdp); }

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
    if (!ffrdp) return;
    pthread_mutex_lock(&ffrdp->lock);
    ffrdp->flags |= FLAG_FLUSH;
    // 工作线程可能正睡在"下一个期限"上：把它叫醒，这一帧才不用等到那时候。
    if (ffrdp->lowlevel) ffrdp->lowlevel->timer(ffrdp->lowlevel, get_tick_count());
    pthread_mutex_unlock(&ffrdp->lock);
}

void ffrdp_dump(void *ctxt, int clearhistory)
{
    FFRDPCONTEXT *ffrdp = (FFRDPCONTEXT*)ctxt;
    if (!ffrdp) return;
    // 要打印的全是协议状态，而协议现在跑在工作线程里：拿住同一把锁再读，打印出来的一整套数不会是被
    // 拆散的一半（这正是旧版单线程下不用操心、换线程后必须补上的一件小事）。
    pthread_mutex_lock(&ffrdp->lock);
    ffrdp_dump_locked(ffrdp, clearhistory);
    pthread_mutex_unlock(&ffrdp->lock);
}

static void ffrdp_dump_locked(FFRDPCONTEXT *ffrdp, int clearhistory)
{
    int secs;
    secs = ((int32_t)get_tick_count() - (int32_t)ffrdp->tick_ffrdp_dump) / 1000;
    secs = secs ? secs : 1;
    printf("total_send, total_recv: %.2fMB, %.2fMB\n"    , ffrdp->counter_send_bytes / (1024.0 * 1024), ffrdp->counter_recv_bytes / (1024.0 * 1024));
    printf("averg_send, averg_recv: %.2fKB/s, %.2fKB/s\n", ffrdp->counter_send_bytes / (1024.0 * secs), ffrdp->counter_recv_bytes / (1024.0 * secs));
    printf("rttm: %u, rtts: %u, rttd: %u, rto: %u\n", ffrdp->rttm, ffrdp->rtts, ffrdp->rttd, ffrdp->rto);
    // which binary is this? the compile stamp makes a stale executable impossible to mistake for a fresh
    // one: if this time is older than the build you just ran, you are running something else.
    printf("build               : probe %d, queue ratio %u, inflight default %u, compiled %s %s\n",
           FFRDP_PROBE_ENABLE, (uint32_t)FFRDP_PROBE_QUEUE_RATIO, (uint32_t)FFRDP_INFLIGHT_MAX, __DATE__, __TIME__);
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
    printf("counter_mack_bits_sent: %u\n" , ffrdp->counter_mack_bits_sent);
    printf("counter_mack_bits_recv: %u\n" , ffrdp->counter_mack_bits_recv);
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
