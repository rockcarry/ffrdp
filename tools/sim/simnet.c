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

typedef struct { unsigned char buf[PKTSIZE]; int len; int due; } PKT;
static PKT q[2][MAXQ];
static int qh[2], qt[2], qn[2];
static int cursid = 0;
static uint32_t g_vtick = 0;
static int gstep = 0, gdelay = 0, gjitter = 0, gburst = 0, gburstleft[2];
static unsigned gpn[2];
static double gloss[2];
static double g_cap = 0;      // shaper rate, bytes per ms, 0 = unlimited
static int    g_qmax = 0;     // shaper queue limit in bytes
static double g_busy = 0;     // when the shaper finishes the packets already queued
static int    g_qbytes = 0;   // bytes currently in the shaper queue
static double g_pcap = 0;     // policer rate, bytes per ms, 0 = unlimited
static int    g_pburst = 0;   // token bucket depth in bytes
static double g_ptok = 0;     // tokens left
static int    g_plast = 0;    // tick of the last refill
static unsigned long g_alloc = 0, g_freed = 0;

uint32_t sim_tick(void) { return g_vtick; } // replaces get_tick_count() in ffrdp.c

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
    for (i = 0; i < 2; i++) { qh[i] = qt[i] = qn[i] = 0; gburstleft[i] = 0; gloss[i] = 0; gpn[i] = 0; }
    cursid = 0; gstep = 0; g_vtick = 0; gdelay = 0; gjitter = 0; gburst = 0;
    g_cap = 0; g_qmax = 0; g_busy = 0; g_qbytes = 0;
    g_pcap = 0; g_pburst = 0; g_ptok = 0; g_plast = 0;
    g_alloc = g_freed = 0;
}

void sim_config(double loss, int burst, int delay, int jitter)
{
    gloss[0] = gloss[1] = loss; gburst = burst; gdelay = delay; gjitter = jitter;
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

void sim_begin(int idx) { cursid = idx; }
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

static void push(int idx, void *buf, int len, unsigned k)
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
    q[idx][qt[idx]].due = due;
    qt[idx] = (qt[idx] + 1) % MAXQ; qn[idx]++;
}

void sim_inject(int idx, void *buf, int len) { push(idx, buf, len, 1); }

int sim_send(void *buf, int len) // the fake sendto
{
    int idx = 1 - cursid, drop = 0;
    unsigned k = ++gpn[idx];
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
    if (!drop) push(idx, buf, len, k);
    return len;
}

int sim_recv(void *buf, int maxlen) // the fake recvfrom
{
    int len;
    if (qn[cursid] == 0 || q[cursid][qh[cursid]].due > gstep) return -1;
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
