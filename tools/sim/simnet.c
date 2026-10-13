// fake socket layer for ffrdp: two in-memory queues with loss, burst loss,
// latency and jitter, plus calloc/free accounting so leaks can be counted.
//
// Everything here is deterministic: the clock is virtual (1 ms per step, driven
// by sim_setstep) and packet loss/jitter come from a hash of the packet counter,
// so two binaries always see exactly the same network.  That is what makes an
// A/B comparison meaningful - run-to-run noise is 0, not +-50%.
#include <stdlib.h>
#include <string.h>
#include "simdefs.h"

#define MAXQ    8192
#define PKTSIZE 2048

typedef struct { unsigned char buf[PKTSIZE]; int len; int due; uint64_t src; } PKT;
static PKT q[SIMNET_SLOTS][MAXQ];
static int qh[SIMNET_SLOTS], qt[SIMNET_SLOTS], qn[SIMNET_SLOTS];
static int cursid = 0;
static uint64_t g_slot[SIMNET_SLOTS];   // 每个 slot 自己的地址；0 = 未登记（走旧路径）
static uint64_t g_lastsrc = 0;          // 最近一次 sim_recv 取到的包的源地址
static uint32_t g_vtick = 0;
static int gstep = 0, gdelay = 0, gjitter = 0, gburst = 0, gburstleft[SIMNET_SLOTS];
static unsigned gpn[SIMNET_SLOTS];
static double gloss[SIMNET_SLOTS];
static double g_cap = 0;      // shaper rate, bytes per ms, 0 = unlimited
static int    g_qmax = 0;     // shaper queue limit in bytes
static double g_busy = 0;     // when the shaper finishes the packets already queued
static int    g_qbytes = 0;   // bytes currently in the shaper queue
static double g_pcap = 0;     // policer rate, bytes per ms, 0 = unlimited
static int    g_pburst = 0;   // token bucket depth in bytes
static double g_ptok = 0;     // tokens left
static int    g_plast = 0;    // tick of the last refill
static int    g_spikep = 0;   // percent of packets hit by a delay spike
static int    g_spikems = 0;  // extra delay of a spike, ms
static unsigned long g_alloc = 0, g_freed = 0;

uint32_t sim_tick(void) { return g_vtick; } // replaces the timestamp in utils.c

static unsigned hash(unsigned k, unsigned salt)
{
    unsigned h = k * 2654435761u + salt * 40503u + 12345u;
    h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
    return h;
}

void *sim_calloc(size_t n, size_t s) { g_alloc++; return calloc(n, s); }
void  sim_free(void *p) { if (!p) return; g_freed++; free(p); }
unsigned long sim_alloc(void) { return g_alloc; }
unsigned long sim_freed(void) { return g_freed; }

void sim_reset(void)
{
    int i;
    for (i = 0; i < SIMNET_SLOTS; i++) { qh[i] = qt[i] = qn[i] = 0; gburstleft[i] = 0; gloss[i] = 0; gpn[i] = 0; g_slot[i] = 0; }
    cursid = 0; g_lastsrc = 0; gstep = 0; g_vtick = 0; gdelay = 0; gjitter = 0; gburst = 0;
    g_cap = 0; g_qmax = 0; g_busy = 0; g_qbytes = 0;
    g_pcap = 0; g_pburst = 0; g_ptok = 0; g_plast = 0;
    g_spikep = 0; g_spikems = 0;
    g_alloc = g_freed = 0;
}

void sim_config(double loss, int burst, int delay, int jitter)
{
    int i;
    // 老的两端测试只用到 slot 0/1，所以这条循环与原来的 gloss[0] = gloss[1] = loss 完全等价 ✓
    for (i = 0; i < SIMNET_SLOTS; i++) gloss[i] = loss;
    gburst = burst; gdelay = delay; gjitter = jitter;
}

// One shared shaper for both directions, like an access point policing udp:
// data and acks are serialised at g_cap bytes/ms in one fifo, so bulk traffic in
// one direction delays the acks of the other - which is what the real link does.
// Packets that do not fit in the queue are dropped.
void sim_shaper(double bytes_per_ms, int queue_bytes)
{
    g_cap = bytes_per_ms; g_qmax = queue_bytes; g_busy = 0; g_qbytes = 0;
}

// token bucket: over the rate the excess is dropped, not queued (that is what the
// measurement shows - the achieved rate stays flat while the loss grows with the
// excess). Applies to the direction sent by side 0, i.e. the uplink of the client.
void sim_policer(double bytes_per_ms, int burst_bytes)
{
    g_pcap = bytes_per_ms; g_pburst = burst_bytes; g_ptok = burst_bytes; g_plast = 0;
}

// A small fraction of the packets is delayed a lot (a retry storm on the air).
// Because everything behind it is pushed back as well, the rtt distribution gets a
// long tail - and the frames that hit that tail are exactly the ones that time out,
// so they never enter the rtt samples (Karn). That is what keeps rto too small.
void sim_spike(int percent, int extra_ms)
{
    g_spikep = percent; g_spikems = extra_ms;
}

void sim_begin(int idx) { cursid = (idx >= 0 && idx < SIMNET_SLOTS) ? idx : 0; }

// 登记/查询 slot 的地址。不登记就是 0：包不带源地址，接收侧也就报不出对端 —— 与以前逐字相同。
void     sim_slot(int idx, uint64_t addr) { if (idx >= 0 && idx < SIMNET_SLOTS) g_slot[idx] = addr; }
uint64_t sim_recv_src(void) { return g_lastsrc; }
int  sim_queued(int idx) { return qn[idx]; }

void sim_setstep(int step)
{
    gstep   = step;
    g_vtick = (uint32_t)step; // 1 ms per step
}

void sim_advance(uint32_t ms)
{
    g_vtick += ms;
    gstep   += (int)ms;
}

static void push(int idx, uint64_t src, void *buf, int len, unsigned k)
{
    int due, prev, start;
    if (qn[idx] >= MAXQ) return;
    if (len > PKTSIZE) len = PKTSIZE;
    start = gstep;
    if (g_cap > 0) {
        if (g_qbytes + len > g_qmax) return;            // shaper queue is full, drop it
        g_qbytes += len;
        if (g_busy < (double)start) g_busy = (double)start;
        g_busy += len / g_cap;                          // serialise this packet at the shaper rate
        start  = (int)g_busy;
    }
    memcpy(q[idx][qt[idx]].buf, buf, len);
    q[idx][qt[idx]].len = len;
    due  = start + gdelay + (gjitter > 0 ? (int)(hash(k, 7 + idx) % (unsigned)(gjitter + 1)) : 0);
    prev = (qt[idx] + MAXQ - 1) % MAXQ;
    if (qn[idx] > 0 && due < q[idx][prev].due) due = q[idx][prev].due; // keep fifo order
    // a small share of packets is delayed a lot on its own. Those frames time out, and
    // because Karn drops the samples of retransmitted frames, the long tail never reaches
    // rtts/rttd - so rto stays tuned to the fast part and the tail keeps timing out.
    if (g_spikep > 0 && (int)(hash(k, 3 + idx) % 100u) < g_spikep) due += g_spikems;
    q[idx][qt[idx]].src = src;   // 随包携带：接收侧靠它告诉上层"这是谁发的"
    q[idx][qt[idx]].due = due;
    qt[idx] = (qt[idx] + 1) % MAXQ; qn[idx]++;
}

void sim_inject(int idx, void *buf, int len) { push(idx, g_slot[idx], buf, len, 1); }

// 显式版本：目的 slot 与包里带的源地址都由调用方给（多对端路由用，P0 新增）。
// 目的 slot 越界就直接丢弃 —— 旧路径只会在 slot 0/1 之间走，所以这条护栏碰不到老测试。
int sim_send_from(int dst, uint64_t src, void *buf, int len)
{
    int idx = dst, drop = 0;
    unsigned k;
    if (idx < 0 || idx >= SIMNET_SLOTS) return len;
    k = ++gpn[idx];
    unsigned r = hash(k, idx) % 100000u;
    if (g_pcap > 0 && cursid == 0) { // token bucket on the uplink of side 0
        int dt = gstep - g_plast;
        if (dt > 0) {
            g_plast  = gstep;
            g_ptok  += dt * g_pcap;
            if (g_ptok > g_pburst) g_ptok = g_pburst;
        }
        if (g_ptok < len) drop = 1; else g_ptok -= len;
    }
    if (gburst) {
        if (gburstleft[idx] > 0) { gburstleft[idx]--; drop = 1; }
        else if (r < (unsigned)(gloss[idx] * 100000)) { gburstleft[idx] = gburst - 1; drop = 1; }
    } else if (r < (unsigned)(gloss[idx] * 100000)) drop = 1;
    if (!drop) push(idx, src, buf, len, k);
    return len;
}

// 旧路径：发到"对面"那个 slot，源地址 = 本 slot 登记的地址（默认 0 ⇒ 与以前逐字相同）。
int sim_send(void *buf, int len) // the fake sendto
{
    return sim_send_from(1 - cursid, g_slot[cursid], buf, len);
}

int sim_recv(void *buf, int maxlen) // the fake recvfrom
{
    int len;
    if (qn[cursid] == 0 || q[cursid][qh[cursid]].due > gstep) return -1;
    g_lastsrc = q[cursid][qh[cursid]].src;   // 交付时把"谁发的"一起给出（旧路径是 0）
    len = q[cursid][qh[cursid]].len;
    if (len > maxlen) len = maxlen;
    memcpy(buf, q[cursid][qh[cursid]].buf, len);
    g_qbytes -= q[cursid][qh[cursid]].len; if (g_qbytes < 0) g_qbytes = 0;
    qh[cursid] = (qh[cursid] + 1) % MAXQ; qn[cursid]--;
    return len;
}

int sim_select(void)
{
    if (qn[cursid] == 0) return 0;
    return q[cursid][qh[cursid]].due <= gstep ? 1 : 0;
}
