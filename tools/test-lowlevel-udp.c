// Conformance test for the UDP lowlevel backend, lowlevel-udp.c.
//
// The point of this layer in the v2.0.0 split is to be the only place that knows what a socket is, so
// that everything above it (the protocol layer, later the VPN/raw modes) can be pointed at another
// backend without being touched. What that requires of this test is that it judges the layer's CONTRACT
// and not its implementation:
//
//   * params are parsed as documented, and a malformed bind is refused rather than half applied
//   * the receive callback is invoked with a FFRDP_LOWLEVEL_MSG (type, size, addr, buf, len), and the
//     addr really identifies the sender -- including the port, which is easy to get wrong and invisible
//     until something tries to reply
//   * send() delivers to an addr obtained from a receive, from any thread, while the receive thread is
//     running
//   * init/exit are leak free and safe to call back to back, including exit() before the thread has
//     created its socket at all
//
// One deliberate piece of white box: mk_addr() below reproduces the addr encoding documented in
// lowlevel-udp.c (ip << 16 | port, both host order) because that encoding is currently private to the
// implementation, and without it this test cannot address an endpoint before anything has been received
// from it. The protocol layer will need a public constructor for the same thing; when that exists, this
// helper should be replaced by it rather than kept in parallel with it.
//
// Build and run: bash tools/test-lowlevel-udp.sh            (ASAN=1 for the sanitizer)

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include "socket.h"
#include "utils.h"
#include "lowlevel.h"

/* ------------------------------------------------------------------ *
 *  tiny test framework
 * ------------------------------------------------------------------ */

static int g_pass, g_fail, g_section_fail;

static void section(const char *name)
{
    if (g_section_fail) printf("  -- %d check(s) failed above\n", g_section_fail);
    g_section_fail = 0;
    printf("\n== %s ==\n", name);
    fflush(stdout);
}

static int check_at(int line, const char *expr, int ok)
{
    if (ok) g_pass++;
    else { g_fail++; g_section_fail++; printf("  FAIL line %-4d %s\n", line, expr); fflush(stdout); }
    return ok;
}
#define CHECK(cond)      check_at(__LINE__, #cond, !!(cond))
#define CHECK_EQ(a, b)   check_at(__LINE__, #a " == " #b, (long)(a) == (long)(b))

static void note(const char *fmt, ...)
{
    va_list ap;
    printf("  .. ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

/* ------------------------------------------------------------------ *
 *  the endpoint under test, plus a plain socket to be the other side
 * ------------------------------------------------------------------ */

#define MAXPAYLOAD 4096

typedef struct {
    pthread_mutex_t lock;
    int             count;        // RECV callbacks seen
    int             last_type;    // type argument of the last callback
    int             last_msgsz;   // len argument of the last callback (the message size)
    int             last_len;     // length of the payload inside the message
    uint64_t        last_addr;    // addr inside the message
    pthread_t       last_thread;  // which thread ran the last callback
    unsigned char   last[MAXPAYLOAD];
    FFRDP_LOWLEVEL *fl;           // for replying, and for arming ticks, from inside the callback
    int             reply_left;   // reply to this many more callbacks; armed before traffic starts, so
                                  // that a reply can never be armed too late for a datagram already sent
    int             reply_sent;   // replies send() accepted
    int             reply_fail;   // replies send() refused
    char            reply[256];
    int             reply_len;

    int             ticks;        // TIMER callbacks seen
    uint32_t        tick_ms;      // when the last one ran
    int             tick_send_rc; // send() return code when a tick sent a frame
    char            seq[512];     // one letter per callback, in order: R = RECV, T = TIMER
    int             seq_len;
    int             chain;        // when > 0, arm the next tick from inside the TIMER callback
    int             arm_on_recv;  // when set, the next RECV callback arms a tick for now
    int             send_on_tick; // when set, the next TIMER callback sends tick_send
    char            tick_send[64];
    int             in_cb;        // set while this endpoint's own callback is running. Atomic, and taken
                                  // before the lock: a nested entry would otherwise block on that lock and
                                  // never reach the check that is supposed to catch it.
} RX;

// The layer promises never to enter a callback from inside another one -- per endpoint, which is what the
// contract says: each endpoint has its own thread, so two endpoints running callbacks at the same time is
// ordinary and is counted separately below. Both counters are written from several threads, so the
// read-modify-write goes through the compiler's atomics instead of relying on a plain int load and store.
static int g_in_callback, g_nested, g_overlap;

static void rx_init(RX *rx)
{
    memset(rx, 0, sizeof(*rx));
    pthread_mutex_init(&rx->lock, NULL);
}

static void rx_attach(RX *rx, FFRDP_LOWLEVEL *fl)
{
    pthread_mutex_lock(&rx->lock);
    rx->fl = fl;
    pthread_mutex_unlock(&rx->lock);
}

// arm the next n callbacks to reply with this text
static void rx_ask_reply(RX *rx, int n, const char *text)
{
    pthread_mutex_lock(&rx->lock);
    rx->reply_len = (int)strlen(text) + 1;
    memcpy(rx->reply, text, rx->reply_len);
    rx->reply_left = n;
    pthread_mutex_unlock(&rx->lock);
}

static int rx_reply_sent(RX *rx)
{
    int n;
    pthread_mutex_lock(&rx->lock);
    n = rx->reply_sent;
    pthread_mutex_unlock(&rx->lock);
    return n;
}

static int rx_reply_fail(RX *rx)
{
    int n;
    pthread_mutex_lock(&rx->lock);
    n = rx->reply_fail;
    pthread_mutex_unlock(&rx->lock);
    return n;
}

static int rx_count(RX *rx)
{
    int n;
    pthread_mutex_lock(&rx->lock);
    n = rx->count;
    pthread_mutex_unlock(&rx->lock);
    return n;
}

static int rx_ticks(RX *rx)
{
    int n;
    pthread_mutex_lock(&rx->lock);
    n = rx->ticks;
    pthread_mutex_unlock(&rx->lock);
    return n;
}

static uint32_t rx_tick_ms(RX *rx)
{
    uint32_t t;
    pthread_mutex_lock(&rx->lock);
    t = rx->tick_ms;
    pthread_mutex_unlock(&rx->lock);
    return t;
}

static int rx_tick_send_rc(RX *rx)
{
    int n;
    pthread_mutex_lock(&rx->lock);
    n = rx->tick_send_rc;
    pthread_mutex_unlock(&rx->lock);
    return n;
}

// arm the next n ticks from inside the TIMER callback, to show a callback can set its own next deadline
static void rx_tick_chain(RX *rx, int n)
{
    pthread_mutex_lock(&rx->lock);
    rx->chain = n;
    pthread_mutex_unlock(&rx->lock);
}

// make the next RECV callback arm a tick for now
static void rx_tick_arm_on_recv(RX *rx)
{
    pthread_mutex_lock(&rx->lock);
    rx->arm_on_recv = 1;
    pthread_mutex_unlock(&rx->lock);
}

// make the next TIMER callback send this text to whoever was last heard from
static void rx_tick_send(RX *rx, const char *text)
{
    pthread_mutex_lock(&rx->lock);
    snprintf(rx->tick_send, sizeof(rx->tick_send), "%s", text);
    rx->tick_send_rc = -2;
    rx->send_on_tick = 1;
    pthread_mutex_unlock(&rx->lock);
}

// The callbacks seen so far as letters, so an ordering claim can be checked instead of assumed. "Since"
// takes a mark (the current length) and copies everything after it.
static int rx_seq_mark(RX *rx)
{
    int n;
    pthread_mutex_lock(&rx->lock);
    n = rx->seq_len;
    pthread_mutex_unlock(&rx->lock);
    return n;
}

// Start the record over, so a long earlier section cannot fill it up and leave the ordering check of a
// later one looking at an empty string.
static void rx_seq_reset(RX *rx)
{
    pthread_mutex_lock(&rx->lock);
    rx->seq_len = 0;
    pthread_mutex_unlock(&rx->lock);
}

static void rx_seq_since(RX *rx, int mark, char *out, int len)
{
    pthread_mutex_lock(&rx->lock);
    int n = rx->seq_len - mark;
    if (n < 0) n = 0;
    if (n > len - 1) n = len - 1;
    memcpy(out, rx->seq + mark, (size_t)n);
    out[n] = '\0';
    pthread_mutex_unlock(&rx->lock);
}

static long on_recv(void *cbctx, int type, void *buf, int len)
{
    RX      *rx  = (RX *)cbctx;
    uint32_t now = ffrdp_get_timestamp32_ms();
    int      arm_now = 0;
    char     send_text[64];

    if (__atomic_exchange_n(&rx->in_cb, 1, __ATOMIC_RELAXED)) {
        __atomic_fetch_add(&g_nested, 1, __ATOMIC_RELAXED);   // the same endpoint re-entered itself
    }
    if (__atomic_exchange_n(&g_in_callback, 1, __ATOMIC_RELAXED)) {
        __atomic_fetch_add(&g_overlap, 1, __ATOMIC_RELAXED);  // a different endpoint, not a violation
    }
    send_text[0] = '\0';

    pthread_mutex_lock(&rx->lock);
    if (rx->seq_len < (int)sizeof(rx->seq) - 1) {
        rx->seq[rx->seq_len++] = (type == FFRDP_LOWLEVEL_TYPE_TIMER) ? 'T' : 'R';
    }
    if (type == FFRDP_LOWLEVEL_TYPE_TIMER) {
        // a tick carries no message at all: buf is NULL and len is 0 by contract
        rx->ticks++;
        rx->tick_ms = now;
        if (rx->chain > 0)         { rx->chain--; arm_now = 1; }
        if (rx->send_on_tick)      { rx->send_on_tick = 0; memcpy(send_text, rx->tick_send, sizeof(send_text)); }
    } else {
        FFRDP_LOWLEVEL_MSG *msg = (FFRDP_LOWLEVEL_MSG *)buf;
        rx->count++;
        rx->last_type   = type;
        rx->last_msgsz  = len;
        rx->last_addr   = msg->addr;
        rx->last_len    = msg->len;
        rx->last_thread = pthread_self();
        if (msg->len > 0 && msg->len <= MAXPAYLOAD) memcpy(rx->last, msg->buf, msg->len);
        if (rx->reply_left > 0 && rx->fl) {
            // reply to wherever this came from, using the addr straight out of the message
            rx->reply_left--;
            if (rx->fl->send(rx->fl, msg->addr, (uint8_t *)rx->reply, rx->reply_len) == 0) rx->reply_sent++;
            else rx->reply_fail++;
        }
        if (rx->arm_on_recv) { rx->arm_on_recv = 0; arm_now = 1; }
    }
    pthread_mutex_unlock(&rx->lock);

    if (arm_now && rx->fl) rx->fl->timer(rx->fl, now);     // "come back on this lap, please"
    if (send_text[0] && rx->fl) {
        // a tick is an ordinary moment to talk: send to whoever we last heard from
        pthread_mutex_lock(&rx->lock);
        uint64_t to = rx->last_addr;
        pthread_mutex_unlock(&rx->lock);
        rx->tick_send_rc = rx->fl->send(rx->fl, to, (uint8_t *)send_text, (int)strlen(send_text) + 1);
    }

    __atomic_store_n(&rx->in_cb, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_in_callback, 0, __ATOMIC_RELAXED);
    return 0;
}

// wait until at least want callbacks have been seen, or ms elapses
static int rx_wait(RX *rx, int want, int ms)
{
    int waited = 0;
    while (waited < ms) {
        if (rx_count(rx) >= want) return 1;
        usleep(2 * 1000);
        waited += 2;
    }
    return rx_count(rx) >= want;
}

static int rx_wait_ticks(RX *rx, int want, int ms)
{
    int waited = 0;
    while (waited < ms) {
        if (rx_ticks(rx) >= want) return 1;
        usleep(2 * 1000);
        waited += 2;
    }
    return rx_ticks(rx) >= want;
}

// Send until the other endpoint has answered, or ms runs out. The layer creates its socket inside its own
// thread, so an endpoint that has just been created is not necessarily listening yet and the first
// datagram to it can be lost. That is allowed here -- a lost datagram is a lost datagram -- so this is not
// papering over anything; it keeps the section about addressing instead of about a race.
static int send_until(RX *target, int want, FFRDP_LOWLEVEL *fl, uint64_t addr, const char *text, int ms)
{
    int waited = 0;
    while (waited < ms) {
        fl->send(fl, addr, (uint8_t *)text, (int)strlen(text) + 1);
        if (rx_wait(target, want, 100)) return 1;
        waited += 100;
    }
    return rx_count(target) >= want;
}

// a plain socket for the far side: the test must not use the layer it is testing as its own reference
static int peer_open(int port)
{
    struct sockaddr_in a;
    // Short receive timeout on the test's own socket: every reply this test waits for is expected within
    // milliseconds on loopback, so a short timeout turns a stall into a quick, legible failure instead of
    // a long wait -- with a one second timeout a stalled loop elsewhere can add up to minutes.
    struct timeval tv = { 0, 300 * 1000 };
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return -1;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port   = htons((uint16_t)port);
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { close(fd); return -1; }
    return fd;
}

static uint64_t mk_addr(const char *ip, int port)
{
    return ((uint64_t)ntohl(inet_addr(ip)) << 16) | (uint64_t)(uint16_t)port;
}

static int peer_send_to(int fd, int port, const void *buf, int len)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port   = htons((uint16_t)port);
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    return (int)sendto(fd, buf, len, 0, (struct sockaddr *)&a, sizeof(a));
}

// returns the number of bytes, or -1; on success from holds the sender's addr in the same encoding
static int peer_recv(int fd, void *buf, int len, uint64_t *from)
{
    struct sockaddr_in from_sin;
    socklen_t fromlen = sizeof(from_sin);
    int ret = (int)recvfrom(fd, buf, len, 0, (struct sockaddr *)&from_sin, &fromlen);
    if (ret >= 0 && from) {
        *from = ((uint64_t)ntohl(from_sin.sin_addr.s_addr) << 16) | (uint64_t)ntohs(from_sin.sin_port);
    }
    return ret;
}

// Throw away whatever is queued on the test's own socket, and say how much there was. Earlier sections arm
// more replies than they consume on purpose, and a section that retries a send can deliver it more than
// once, so without this a stale datagram gets read by the next section and mistaken for its answer --
// which is exactly how section 9 once received section 8's "client" instead of the tick's frame.
static int peer_drain(int fd, int ms)
{
    int  n = 0, waited = 0;
    char b[2048];
    while (waited <= ms) {
        if (recvfrom(fd, b, sizeof(b), MSG_DONTWAIT, NULL, NULL) <= 0) {
            if (waited >= ms) break;
            usleep(10 * 1000);
            waited += 10;
            continue;
        }
        n++;
    }
    return n;
}

static int open_fds(void)
{
    DIR *d = opendir("/proc/self/fd");
    struct dirent *e;
    int n = -1;
    if (!d) return -1;
    n = 0;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) n++;
    }
    closedir(d);
    return n;
}

static int live_threads(void)
{
    FILE *f = fopen("/proc/self/status", "r");
    char line[256];
    int  n = -1;
    if (!f) return -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "Threads:", 8) == 0) { n = atoi(line + 8); break; }
    }
    fclose(f);
    return n;
}

/* ------------------------------------------------------------------ *
 *  the suite
 * ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    uint16_t base = (uint16_t)(20000 + (getpid() % 20000));
    int port_ep = base, port_peer = base + 1, port_b = base + 2, port_q = base + 3;
    char params[64];
    RX rx;
    int  fd;

    printf("test-lowlevel-udp: ports %d (endpoint), %d (peer), %d (second endpoint)\n",
           port_ep, port_peer, port_b);
    if (argc > 1) printf("(extra argument \"%s\" ignored)\n", argv[1]);

    /* ---------------------------------------------------------------- */
    section("1. params: malformed bind is refused, NULL is refused");

    CHECK(ffrdp_udp_lowlevel_init(NULL,          on_recv, NULL) == NULL);
    CHECK(ffrdp_udp_lowlevel_init("bind:1.2.3",  on_recv, NULL) == NULL);   // no ip:port
    CHECK(ffrdp_udp_lowlevel_init("bind::1234",  on_recv, NULL) == NULL);   // no ip
    CHECK(ffrdp_udp_lowlevel_init("bind:1.2.3.4:99999", on_recv, NULL) == NULL);
    CHECK(ffrdp_udp_lowlevel_init("bind:no.such.address:1234", on_recv, NULL) == NULL);

    rx_init(&rx);
    snprintf(params, sizeof(params), "bind:127.0.0.1:%d", port_ep);
    FFRDP_LOWLEVEL *fl = ffrdp_udp_lowlevel_init(params, on_recv, &rx);
    CHECK(fl != NULL);
    if (!fl) { printf("\ninit failed, nothing else can run\n"); return 1; }
    CHECK(fl->send != NULL);
    CHECK(fl->exit != NULL);
    rx_attach(&rx, fl);

    /* ---------------------------------------------------------------- */
    section("2. receive: message shape, payload, and where it came from");

    fd = peer_open(port_peer);
    CHECK(fd >= 0);
    // nothing has been sent yet: the callback must not invent a frame
    usleep(150 * 1000);
    CHECK_EQ(rx_count(&rx), 0);

    const char *req = "hello-lowlevel";
    CHECK_EQ(peer_send_to(fd, port_ep, req, (int)strlen(req) + 1), (int)strlen(req) + 1);
    CHECK(rx_wait(&rx, 1, 1500));

    pthread_mutex_lock(&rx.lock);
    int      type   = rx.last_type;
    int      msgsz  = rx.last_msgsz;
    int      rlen   = rx.last_len;
    uint64_t addr   = rx.last_addr;
    pthread_t who   = rx.last_thread;
    char     got[64];
    memcpy(got, rx.last, rlen < 63 ? rlen : 63);
    pthread_mutex_unlock(&rx.lock);

    CHECK_EQ(type, FFRDP_LOWLEVEL_TYPE_RECV);
    CHECK_EQ(msgsz, sizeof(FFRDP_LOWLEVEL_MSG));   // the callback gets a message, not a bare payload
    CHECK_EQ(rlen, (int)strlen(req) + 1);
    CHECK(strcmp(got, req) == 0);
    // the addr is the documented "32 bit ip in the high half, 16 bit port in the low half", host order:
    // a wrong byte order here passes every test except a reply, which is exactly what this pins down
    CHECK_EQ((uint32_t)(addr >> 16), 0x7F000001u);
    CHECK_EQ((uint32_t)(addr & 0xFFFF), (uint32_t)port_peer);
    // and the callback runs on the layer's own thread, not on the caller's
    CHECK(!pthread_equal(who, pthread_self()));

    /* ---------------------------------------------------------------- */
    section("3. send: to an addr learnt from a receive, from the callback and from main");

    rx_ask_reply(&rx, 1, "reply-from-callback");
    CHECK_EQ(peer_send_to(fd, port_ep, "r1", 3), 3);
    CHECK(rx_wait(&rx, 2, 1500));
    char    buf[MAXPAYLOAD];
    uint64_t back = 0;
    int     n = peer_recv(fd, buf, sizeof(buf), &back);
    CHECK_EQ(n, (int)strlen("reply-from-callback") + 1);
    CHECK(strcmp(buf, "reply-from-callback") == 0);
    CHECK_EQ(rx_reply_sent(&rx), 1);                     // the reply was sent, exactly once
    CHECK_EQ(rx_reply_fail(&rx), 0);
    CHECK_EQ((uint32_t)(back & 0xFFFF), (uint32_t)port_ep);   // it came from the port we bound

    // the same addr, used from the main thread while the receive thread is running
    uint64_t peer_addr = mk_addr("127.0.0.1", port_peer);
    CHECK_EQ(fl->send(fl, peer_addr, (uint8_t *)"from-main", 10), 0);
    n = peer_recv(fd, buf, sizeof(buf), &back);
    CHECK_EQ(n, 10);
    CHECK(memcmp(buf, "from-main", 10) == 0);

    // send() argument checks
    CHECK_EQ(fl->send(NULL, peer_addr, (uint8_t *)"x", 1), -1);
    CHECK_EQ(fl->send(fl, peer_addr, NULL, 1), -1);
    CHECK_EQ(fl->send(fl, peer_addr, (uint8_t *)"x", 0), -1);

    // a closed port on loopback: sendto succeeds, the icmp error must not take the receive loop down
    CHECK_EQ(fl->send(fl, mk_addr("127.0.0.1", 1), (uint8_t *)"to-nobody", 9), 0);
    usleep(80 * 1000);
    rx_ask_reply(&rx, 1, "still-alive");
    CHECK_EQ(peer_send_to(fd, port_ep, "r2", 3), 3);
    CHECK(rx_wait(&rx, 3, 1500));
    CHECK_EQ(peer_recv(fd, buf, sizeof(buf), &back), (int)strlen("still-alive") + 1);

    /* ---------------------------------------------------------------- */
    section("4. payload sizes, up to the interface frame limit and the truncation above it");

    note("interface frame limit (FFRDP_LOWLEVEL_FRAME_MAX): %d bytes", FFRDP_LOWLEVEL_FRAME_MAX);
    int sizes[] = { 1, 100, 1280, 1472, 1492, FFRDP_LOWLEVEL_FRAME_MAX, FFRDP_LOWLEVEL_FRAME_MAX + 500 };
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        int sz = sizes[i];
        // what arrives is the frame limit, taken from the constant itself: if the layer's buffer and the
        // advertised limit ever drift apart, this is where it shows up
        int want = sz > FFRDP_LOWLEVEL_FRAME_MAX ? FFRDP_LOWLEVEL_FRAME_MAX : sz;
        unsigned char payload[FFRDP_LOWLEVEL_FRAME_MAX + 500];
        for (int k = 0; k < sz; k++) payload[k] = (unsigned char)(k ^ sz);
        int before = rx_count(&rx);
        CHECK_EQ(peer_send_to(fd, port_ep, payload, sz), sz);
        CHECK(rx_wait(&rx, before + 1, 1500));
        pthread_mutex_lock(&rx.lock);
        int got_len = rx.last_len;
        int same    = got_len == want && memcmp(rx.last, payload, want) == 0;
        pthread_mutex_unlock(&rx.lock);
        CHECK_EQ(got_len, want);
        CHECK(same);
    }
    note("a datagram larger than the %d byte frame limit is silently truncated, as documented",
         FFRDP_LOWLEVEL_FRAME_MAX);

    /* ---------------------------------------------------------------- */
    section("5. load: paced traffic must not lose anything, a burst is only reported");

    unsigned int t0 = ffrdp_get_timestamp32_ms();
    int paced_n = 1000, paced_rc = 0;
    int before = rx_count(&rx);   // taken BEFORE the traffic: the receive thread keeps up, so counting
                                  // after the loop would count almost nothing and prove nothing
    for (int i = 0; i < paced_n; i++) {
        unsigned char payload[1280];
        memset(payload, i & 0xFF, sizeof(payload));
        if (peer_send_to(fd, port_ep, payload, sizeof(payload)) == (int)sizeof(payload)) paced_rc++;
        usleep(1000);
    }
    unsigned int ms = ffrdp_get_timestamp32_ms() - t0;
    rx_wait(&rx, before + paced_n, 2000);
    int delivered = rx_count(&rx) - before;
    note("paced: sent %d, delivered %d in %u ms (~%d frames/s)", paced_rc, delivered, ms,
         ms ? (int)((long)delivered * 1000 / ms) : -1);
    CHECK_EQ(paced_rc, paced_n);
    CHECK(delivered >= paced_n * 99 / 100);   // a paced stream on loopback must essentially not drop

    // and the burst: the kernel is allowed to drop, because this layer does not size SO_RCVBUF
    before = rx_count(&rx);
    int burst_n = 2000;
    for (int i = 0; i < burst_n; i++) {
        unsigned char payload[1280];
        memset(payload, 0x5A, sizeof(payload));
        peer_send_to(fd, port_ep, payload, sizeof(payload));
    }
    usleep(500 * 1000);
    rx_wait(&rx, before + burst_n, 1500);
    delivered = rx_count(&rx) - before;
    note("burst: sent %d back to back with no pacing, delivered %d (%d%%), %d dropped by the kernel",
         burst_n, delivered, delivered * 100 / burst_n, burst_n - delivered);
    CHECK(delivered > 0);

    /* ---------------------------------------------------------------- */
    section("6. two endpoints: a second backend instance, and traffic between them");

    RX rxb;
    rx_init(&rxb);
    snprintf(params, sizeof(params), "bind:127.0.0.1:%d", port_b);
    FFRDP_LOWLEVEL *flb = ffrdp_udp_lowlevel_init(params, on_recv, &rxb);
    CHECK(flb != NULL);
    if (flb) {
        rx_attach(&rxb, flb);
        // endpoint -> endpoint, addressing each other through the addr the layer reported. Only b replies:
        // arming a as well made a answer b's reply, so what b held last was no longer the message under
        // test -- the same mistake is why this section used to check a value it had already overwritten.
        rx_ask_reply(&rxb, 1, "b-answers");
        // b was created a moment ago, and the layer opens its socket inside its own thread, so the first
        // datagram aimed at b can be sent before it is listening. send_until retries for that reason; how
        // often it is actually needed is measured in section 7 rather than guessed from one attempt here.
        int a_before_x = rx_count(&rx);
        CHECK(send_until(&rxb, 1, fl, mk_addr("127.0.0.1", port_b), "a-to-b", 2000));   // b got it
        CHECK(rx_wait(&rx, a_before_x + 1, 1500));        // b replied, a got the reply
        pthread_mutex_lock(&rxb.lock);
        uint64_t b_from = rxb.last_addr;
        int      b_len  = rxb.last_len;
        char     b_pay[32];
        memcpy(b_pay, rxb.last, b_len < 31 ? b_len : 31);
        b_pay[b_len < 31 ? b_len : 31] = '\0';
        pthread_mutex_unlock(&rxb.lock);
        note("b received %d bytes \"%s\" from addr 0x%llX", b_len, b_pay, (unsigned long long)b_from);
        CHECK_EQ(b_len, 7);
        CHECK(strcmp(b_pay, "a-to-b") == 0);
        CHECK_EQ((uint32_t)(b_from & 0xFFFF), (uint32_t)port_ep);   // and it says it came from a

        // ... and a holds b's reply, which is the whole round trip through the layer's own addressing
        pthread_mutex_lock(&rx.lock);
        int      a_len  = rx.last_len;
        uint64_t a_from = rx.last_addr;
        char     a_pay[32];
        memcpy(a_pay, rx.last, a_len < 31 ? a_len : 31);
        a_pay[a_len < 31 ? a_len : 31] = '\0';
        pthread_mutex_unlock(&rx.lock);
        note("a received %d bytes \"%s\" from addr 0x%llX", a_len, a_pay, (unsigned long long)a_from);
        CHECK_EQ(a_len, (int)strlen("b-answers") + 1);
        CHECK(strcmp(a_pay, "b-answers") == 0);
        CHECK_EQ((uint32_t)(a_from & 0xFFFF), (uint32_t)port_b);    // and it says it came from b

        // both directions at once: main sends to b while a answers what the plain socket sends it.
        // The replies are armed up front for every ping in the loop, so a reply can never be armed too
        // late for a ping that has already been sent -- that race is what made an earlier version of
        // this section report 199 echoes out of 200.
        rx_ask_reply(&rx, 300, "echo");
        int a_before = rx_count(&rx), b_before = rx_count(&rxb);
        int sends = 0, echoes = 0, misses = 0;
        for (int i = 0; i < 200; i++) {
            if (fl->send(fl, mk_addr("127.0.0.1", port_b), (uint8_t *)"tick", 5) == 0) sends++;
            peer_send_to(fd, port_ep, "ping", 5);
            if (peer_recv(fd, buf, sizeof(buf), &back) == 5 && memcmp(buf, "echo", 4) == 0) {
                echoes++;
                misses = 0;
            } else if (++misses >= 20) {
                // Stop early if the replies have plainly stopped: without this the worst case is one
                // receive timeout per iteration, which is how a stalled run turns into a "hang" that says
                // nothing about what actually went wrong.
                note("stopping after %d iterations: %d consecutive pings got no reply", i + 1, misses);
                break;
            }
            usleep(1000);
        }
        note("concurrent: %d sends from main, %d callbacks on a, %d callbacks on b, %d echoes on the peer",
             sends, rx_count(&rx) - a_before, rx_count(&rxb) - b_before, echoes);
        CHECK_EQ(sends, 200);                       // main thread send, receive threads live
        CHECK(echoes >= 195);                       // a's replies reached the plain socket
        CHECK(rx_count(&rx) - a_before >= 200);     // and a saw every ping
        CHECK(rx_count(&rxb) - b_before == 200);    // b took 200 while a was busy
        note("drained %d stale datagram(s) from the peer socket before the next section",
             peer_drain(fd, 100));
        flb->exit(flb);
    } else {
        CHECK(0);
    }

    /* ---------------------------------------------------------------- */
    section("7. lifecycle: exit before the thread is up, leaks, exit under traffic");

    CHECK(fl->exit != NULL);
    fl->exit(NULL);        // must be a no-op, not a crash

    int fds0 = open_fds(), thr0 = live_threads();
    note("baseline: %d open fds, %d live threads", fds0, thr0);
    for (int i = 0; i < 50; i++) {   // init immediately followed by exit: the thread may never run
        FFRDP_LOWLEVEL *f = ffrdp_udp_lowlevel_init(params, on_recv, &rxb);
        CHECK(f != NULL);
        if (f) f->exit(f);
    }
    for (int i = 0; i < 20; i++) {   // and with enough time for the thread to create its socket
        FFRDP_LOWLEVEL *f = ffrdp_udp_lowlevel_init(params, on_recv, &rxb);
        CHECK(f != NULL);
        if (f) { usleep(20 * 1000); f->exit(f); }
    }
    usleep(50 * 1000);
    int fds1 = open_fds(), thr1 = live_threads();
    note("after 70 init/exit cycles: %d open fds, %d live threads", fds1, thr1);
    CHECK_EQ(fds1, fds0);
    CHECK_EQ(thr1, thr0);

    // Is a just created endpoint listening by the time init() returns? Measured rather than assumed. It
    // does not have to be, and that is fine: this is a datagram layer, a frame that goes missing before the
    // far side is up is a lost frame like any other, and retransmitting it is the protocol's job. The
    // measurement is kept because the number is the useful part -- it says how likely the very first frame
    // is to need that retransmission.
    {
        int tries = 20, lost = 0, created = 0;
        for (int i = 0; i < tries; i++) {
            RX rxq;
            rx_init(&rxq);
            snprintf(params, sizeof(params), "bind:127.0.0.1:%d", (int)port_b);
            FFRDP_LOWLEVEL *flq = ffrdp_udp_lowlevel_init(params, on_recv, &rxq);
            if (!flq) break;
            created++;
            fl->send(fl, mk_addr("127.0.0.1", port_b), (uint8_t *)"q", 2);
            if (!rx_wait(&rxq, 1, 200)) lost++;
            flq->exit(flq);
        }
        note("readiness: %d of %d endpoints had not received the datagram sent right after init "
             "(expected: the socket and its bind happen inside the receive thread)", lost, created);
    }

    // exit while frames are in flight
    RX rxt;
    rx_init(&rxt);
    snprintf(params, sizeof(params), "bind:127.0.0.1:%d", (int)port_b);
    FFRDP_LOWLEVEL *flt = ffrdp_udp_lowlevel_init(params, on_recv, &rxt);
    CHECK(flt != NULL);
    if (flt) {
        for (int i = 0; i < 200; i++) { peer_send_to(fd, port_b, "busy", 5); usleep(500); }
        flt->exit(flt);
        CHECK(1);
        note("exit under traffic returned without hanging");
    }

    /* ---------------------------------------------------------------- */
    section("8. an endpoint with no bind request is still usable as a client");

    RX rxc;
    rx_init(&rxc);
    FFRDP_LOWLEVEL *flc = ffrdp_udp_lowlevel_init("no-bind-here", on_recv, &rxc);
    CHECK(flc != NULL);
    if (flc) {
        rx_attach(&rxc, flc);
        // A client-mode endpoint speaks before anything has been received, so this is the first call that
        // can land on a socket the receive thread has not created yet. It may return -1, and that is
        // allowed: a frame that was not accepted is a lost frame, and the protocol above retransmits. What
        // the test then does is what the layer's own contract says the caller must do -- try again.
        int first = flc->send(flc, mk_addr("127.0.0.1", port_peer), (uint8_t *)"client", 7);
        note("first send() right after init returned %d (%s)", first,
             first == 0 ? "the socket was already up" : "-1: not accepted, retried below");
        uint64_t cfrom = 0;
        int sent = 0;
        for (int i = 0; i < 20 && !sent; i++) {
            if (flc->send(flc, mk_addr("127.0.0.1", port_peer), (uint8_t *)"client", 7) == 0 &&
                peer_recv(fd, buf, sizeof(buf), &cfrom) == 7) {
                sent = 1;
            } else {
                usleep(50 * 1000);
            }
        }
        CHECK(sent);
        CHECK((uint32_t)(cfrom & 0xFFFF) != 0);         // the kernel gave it a source port
        // now the other side replies to that addr: an endpoint that never bound must still receive
        peer_send_to(fd, (int)(cfrom & 0xFFFF), "back", 5);
        CHECK(rx_wait(&rxc, 1, 1500));
        flc->exit(flc);
    }

    /* ---------------------------------------------------------------- */
    section("9. timer: deadlines, being hurried from another thread, and what a tick may do");

    rx_seq_reset(&rx);              // section 9 records its own callback order from here on
    rx_ask_reply(&rx, 0, "");       // and no leftovers: stop answering section 6's pings
    note("drained %d stale datagram(s) from the peer socket before section 9", peer_drain(fd, 100));

    // 9a: with no deadline armed the loop must sleep, not tick
    {
        int before = rx_ticks(&rx);
        usleep(300 * 1000);
        note("no deadline armed: %d ticks in 300ms", rx_ticks(&rx) - before);
        CHECK_EQ(rx_ticks(&rx), before);
    }

    // 9b: a deadline lands when asked. Ten rounds, so one lucky sample cannot carry the claim.
    {
        int worst = 0, best = 100000;
        unsigned long total = 0;
        for (int i = 0; i < 10; i++) {
            int      before = rx_ticks(&rx);
            uint32_t t = ffrdp_get_timestamp32_ms();
            CHECK_EQ(fl->timer(fl, t + 50), 0);
            CHECK(rx_wait_ticks(&rx, before + 1, 500));
            int d = (int)(rx_tick_ms(&rx) - t);
            if (d > worst) worst = d;
            if (d < best)  best = d;
            total += (unsigned long)d;
            usleep(20 * 1000);
        }
        note("deadline = now+50ms: min %d, avg %lu, max %d ms over 10 rounds", best, total / 10, worst);
        CHECK(best >= 45);       // never early
        CHECK(worst <= 90);      // and not so late that the deadline is decorative
    }

    // 9c: a deadline that has already passed means now -- this is how queued work is hurried out
    {
        int      before = rx_ticks(&rx);
        uint32_t t = ffrdp_get_timestamp32_ms();
        CHECK_EQ(fl->timer(fl, t - 1), 0);
        CHECK(rx_wait_ticks(&rx, before + 1, 200));
        int d = (int)(rx_tick_ms(&rx) - t);
        note("deadline in the past: delivered after %d ms", d);
        CHECK(d <= 30);
    }

    // 9d: each call replaces the previous deadline rather than adding to it
    {
        int before = rx_ticks(&rx);
        fl->timer(fl, ffrdp_get_timestamp32_ms() + 250);
        usleep(20 * 1000);
        uint32_t t = ffrdp_get_timestamp32_ms();
        fl->timer(fl, t + 20);
        CHECK(rx_wait_ticks(&rx, before + 1, 300));
        int first = (int)(rx_tick_ms(&rx) - t);
        usleep(300 * 1000);
        note("overwrite: first tick after %d ms, %d tick(s) in total (the 250ms one must never arrive)",
             first, rx_ticks(&rx) - before);
        CHECK(first >= 15 && first <= 60);
        CHECK_EQ(rx_ticks(&rx) - before, 1);
    }

    // 9e: the callback may arm its own next deadline -- a chain, which then stops because nothing re-arms it
    {
        int before = rx_ticks(&rx);
        rx_tick_chain(&rx, 5);           // the callback re-arms itself 5 times
        fl->timer(fl, ffrdp_get_timestamp32_ms() + 10);
        CHECK(rx_wait_ticks(&rx, before + 6, 1000));
        usleep(200 * 1000);
        note("chained from inside the callback: %d ticks (1 armed by the test, then 5 armed by the "
             "callback, then it stops)", rx_ticks(&rx) - before);
        CHECK_EQ(rx_ticks(&rx) - before, 6);
    }

    // 9f: a tick is an ordinary moment to talk -- send() works from inside it
    {
        rx_tick_send(&rx, "from-timer");
        uint64_t from_ep = 0;
        CHECK_EQ(fl->timer(fl, ffrdp_get_timestamp32_ms()), 0);
        CHECK(rx_wait_ticks(&rx, 1, 500));
        int n = peer_recv(fd, buf, sizeof(buf), &from_ep);
        note("a TIMER callback sent a frame with send(): %d bytes \"%s\"", n, n > 0 ? buf : "(nothing)");
        CHECK_EQ(rx_tick_send_rc(&rx), 0);
        CHECK_EQ(n, (int)strlen("from-timer") + 1);
        CHECK(strcmp(buf, "from-timer") == 0);
        CHECK_EQ((uint32_t)(from_ep & 0xFFFF), (uint32_t)port_ep);
    }

    // 9g: the application's thread can hurry the loop -- the whole reason timer() takes a deadline and is
    // callable from any thread. This is what makes a queued frame go out now instead of at the deadline.
    {
        int ok = 0;
        for (int i = 0; i < 20; i++) {
            int      before = rx_ticks(&rx);
            uint32_t t = ffrdp_get_timestamp32_ms();
            CHECK_EQ(fl->timer(fl, t), 0);
            if (rx_wait_ticks(&rx, before + 1, 100) && (int)(rx_tick_ms(&rx) - t) <= 30) ok++;
            usleep(20 * 1000);
        }
        note("hurried from the main thread: %d of 20 ticks arrived within 30ms", ok);
        CHECK_EQ(ok, 20);
    }

    // 9h: a tick armed from inside a receive is delivered after that receive, not during it
    {
        int before = rx_ticks(&rx);
        int mark   = rx_seq_mark(&rx);
        rx_tick_arm_on_recv(&rx);        // the receive callback will arm a tick for now
        CHECK_EQ(peer_send_to(fd, port_ep, "order", 6), 6);
        CHECK(rx_wait_ticks(&rx, before + 1, 500));
        char seq[64];
        rx_seq_since(&rx, mark, seq, sizeof(seq));
        note("callbacks from the armed receive onwards: \"%s\" (R = receive, T = tick)", seq);
        CHECK(strstr(seq, "RT") != NULL);   // the receive that armed it comes first
    }

    // the layer's own invariant, checked across everything above: no endpoint enters its own callback twice
    note("callbacks entered from inside another callback on the same endpoint: %d (over the whole run)", g_nested);
    note("callback pairs overlapping across two endpoints: %d (allowed: one thread each)", g_overlap);
    CHECK_EQ(g_nested, 0);

    // 9i: exit must reach a thread that is asleep with no deadline at all
    {
        RX rxq;
        rx_init(&rxq);
        snprintf(params, sizeof(params), "bind:127.0.0.1:%d", (int)port_q);
        FFRDP_LOWLEVEL *flq = ffrdp_udp_lowlevel_init(params, on_recv, &rxq);
        CHECK(flq != NULL);
        if (flq) {
            usleep(150 * 1000);                 // let it bind and settle into an indefinite wait
            uint32_t t = ffrdp_get_timestamp32_ms();
            flq->exit(flq);
            int d = (int)(ffrdp_get_timestamp32_ms() - t);
            note("exit with no deadline set returned after %d ms", d);
            CHECK(d <= 60);
        }
    }

    /* ---------------------------------------------------------------- */
    section("10. addr_parse: a peer that has not spoken yet can still be addressed");

    {
        char     text[64];
        uint64_t parsed = 0, learnt = 0;
        int      n, before;

        // a configured addr must be indistinguishable from one learnt from a datagram -- that is what lets
        // the layer above send the first frame to a peer that has not said anything yet
        snprintf(text, sizeof(text), "127.0.0.1:%d", port_peer);
        CHECK_EQ(fl->addr_parse(fl, text, &parsed), 0);
        before = rx_count(&rx);
        CHECK_EQ(peer_send_to(fd, port_ep, "whoami", 7), 7);
        CHECK(rx_wait(&rx, before + 1, 1500));
        pthread_mutex_lock(&rx.lock);
        learnt = rx.last_addr;
        pthread_mutex_unlock(&rx.lock);
        note("configured addr 0x%llX, addr learnt from a datagram 0x%llX",
             (unsigned long long)parsed, (unsigned long long)learnt);
        CHECK_EQ(parsed, learnt);

        // and it addresses: send to the configured addr, and the far side receives from our bound port
        uint64_t from_ep = 0;
        CHECK_EQ(fl->send(fl, parsed, (uint8_t *)"configured", 11), 0);
        n = peer_recv(fd, buf, sizeof(buf), &from_ep);
        CHECK_EQ(n, 11);
        CHECK(memcmp(buf, "configured", 10) == 0);
        CHECK_EQ((uint32_t)(from_ep & 0xFFFF), (uint32_t)port_ep);

        // a rejected parse must not touch the output, so a caller cannot mistake a stale value for a new one
        uint64_t keep = parsed;
        CHECK_EQ(fl->addr_parse(fl, NULL, &parsed), -1);
        CHECK_EQ(fl->addr_parse(fl, "", &parsed), -1);
        CHECK_EQ(fl->addr_parse(fl, "127.0.0.1", &parsed), -1);       // no port
        CHECK_EQ(fl->addr_parse(fl, ":8000", &parsed), -1);           // no address
        CHECK_EQ(fl->addr_parse(fl, "127.0.0.1:0", &parsed), -1);     // a peer needs a real port
        CHECK_EQ(fl->addr_parse(fl, "127.0.0.1:99999", &parsed), -1);
        CHECK_EQ(fl->addr_parse(fl, "no.such.host:8000", &parsed), -1);
        CHECK_EQ(fl->addr_parse(fl, "1.2.3.4.5.6.7.8.9.0.1.2.3.4.5.6.7.8.9.0.1.2.3.4.5.6.7.8.9.0.1:80", &parsed), -1);
        CHECK_EQ(fl->addr_parse(fl, "127.0.0.1:8000", NULL), -1);
        CHECK_EQ(parsed, keep);
        note("9 malformed forms rejected, the output addr left untouched");
    }

    /* ---------------------------------------------------------------- */
    close(fd);
    fl->exit(fl);

    printf("\n%s: %d passed, %d failed\n", g_fail ? "FAIL" : "ok", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
