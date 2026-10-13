// t_fuzz: what happens to a context that is fed bytes an attacker chose?
//
// The question this answers (asked for, verbatim): "if ffrdp server binds a port and somebody sends it
// garbage, the process at least must not die". Nothing here is about a *peer's* misbehaviour -- a peer
// that lies is a protocol problem; this is about a stranger who can reach the port and sends whatever it
// likes, including a zero (ttl) length, a truncated frame, or a frame type nobody expects.
//
// How: a fixed corpus of malformed fragments, plus a seeded random phase, each one handed to a
// *separate forked child* that does nothing but create a server context, inject the bytes and update.
// The parent reads the child's wait status, so a crash is not the end of the test: it is one line of
// output naming the exact bytes that did it. A second check floods frames above a permanent hole and
// compares how much memory the library keeps for 2000 and for 4000 of them -- a library the sender is
// credit-limited against must plateau, one that queues whatever arrives grows without bound.
//
// The corpus is a list of things a fragment should never be able to do:
//   * type SHORT/FULL with a length that makes the payload size zero or negative -> the receiver stores
//     a "peer mss" (rmss) computed from that length and later divides by it, and it copies the payload
//     into a ring buffer measuring this many bytes.
//   * a "fec" type (2..FEC32) whose length is shorter than its own 6 byte overhead.
//   * an ACK or QUERY frame of any length, including one byte.
//   * in-order fragments (sequence 0 is in order at the start of a connection), because that is what
//     reaches the deliver path rather than just the reorder list.
//
// Run: (from tools/sim) `TESTS="t_fuzz" bash run.sh` -- or with ASAN=1 to catch reads that stay inside
// the mapping and would otherwise pass silently.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include "simdefs.h"
#include "../../ffrdp.c"

// One step of the protocol. HEAD is driven by the test (ffrdp_update); the working tree is driven by
// its transport, so a step is a step of the fake lowlevel: every packet queued for endpoint 0 is
// handed over as one RECV, then one TIMER.
#ifdef FFRDP_LL_DRIVEN
#define TEST_RUN(c, idx) sim_ll_run(idx, c)
#else
#define TEST_RUN(c, idx) ffrdp_update(c)
#endif

// ffrdp_init 的参数形状变过一次：ip + port 收敛成单个 "ip:port"。两侧都要能编，所以测试走这个入口
// （调用点只关心 ip / port / 角色 / smss）。
#ifdef FFRDP_API_V2
static void *TEST_INIT(const char *ip, int port, int server, int mss)
{
    char params[128];
    snprintf(params, sizeof(params), "%s://%s:%d,mss:%d,fec:0",
             server ? "ffrdps" : "ffrdpc", ip, port, mss);
    return ffrdp_init(params, NULL, NULL);
}
#elif defined(FFRDP_API_HAS_ADDR)
static void *TEST_INIT(const char *ip, int port, int server, int mss)
{
    char addr[64];
    snprintf(addr, sizeof(addr), "%s:%d", ip, port);
    return ffrdp_init(addr, NULL, NULL, server, mss, 0);
}
#else
#define TEST_INIT(ip, port, server, mss) ffrdp_init((char *)(ip), port, NULL, NULL, server, mss, 0)
#endif

// ---- how many cases each phase has ------------------------------------------------------------
#define PHASE_SWEEP   6          // type x length x payload
#define SWEEP_LENS    11         // 0..10
#define SWEEP_PAYS    3
#define PHASE_RANDOM  200
#define FUZZ_CASES    (PHASE_SWEEP * SWEEP_LENS * SWEEP_PAYS + PHASE_RANDOM)

static const unsigned char sweep_types[PHASE_SWEEP] = {
    FFRDP_FRAME_TYPE_FULL, FFRDP_FRAME_TYPE_SHORT, FFRDP_FRAME_TYPE_FEC2,
    FFRDP_FRAME_TYPE_FEC32, FFRDP_FRAME_TYPE_ACK, FFRDP_FRAME_TYPE_QUERY
};
#define SWEEP_MAXLEN  120        // room for the longest fragment the sweep builds

static uint32_t rnd_state;
static uint32_t rnd(void) { rnd_state = rnd_state * 1103515245u + 12345u; return (rnd_state >> 16) & 0x7FFF; }

// one case -> bytes in buf. deterministic: the same index always builds the same fragment, so a
// reported crash is reproducible by index alone.
static int fuzz_case(int idx, unsigned char *buf, int maxlen)
{
    int len, i;
    unsigned char type;
    memset(buf, 0, (size_t)maxlen);
    if (idx < PHASE_SWEEP * SWEEP_LENS * SWEEP_PAYS) {
        int type_i = idx / (SWEEP_LENS * SWEEP_PAYS);
        int len_i  = (idx / SWEEP_PAYS) % SWEEP_LENS;
        int pay_i  = idx % SWEEP_PAYS;
        len  = len_i;
        type = sweep_types[type_i];
        if (len > maxlen) len = maxlen;
        for (i = 0; i < len; i++) {
            if      (pay_i == 0) buf[i] = 0x00;
            else if (pay_i == 1) buf[i] = 0xFF;
            else                 buf[i] = (unsigned char)(0x10 + i);
        }
        if (len >= 1) buf[0] = type;       // type in byte 0
        // bytes 1..3 are the 24 bit sequence: zero means "in order" for a fresh context, so the sweep
        // covers both the reorder path and the deliver path.
        return len;
    }
    // random phase: length and content both arbitrary, but keep the first byte a type so the fragment
    // reaches a parser instead of falling through. every 8th one gets a fully random first byte.
    idx -= PHASE_SWEEP * SWEEP_LENS * SWEEP_PAYS;
    len = (int)(rnd() % 80);
    if (len > maxlen) len = maxlen;
    for (i = 0; i < len; i++) buf[i] = (unsigned char)rnd();
    if (len >= 1 && (idx % 8) != 0) buf[0] = sweep_types[rnd() % PHASE_SWEEP];
    return len;
}

// the child: create a server context, inject, update, exit. nothing is printed here -- whatever it
// prints is lost with it if it dies, so the parent owns reporting.
static void run_case(const unsigned char *buf, int len)
{
    static unsigned char big[64 * 1024];
    FFRDPCONTEXT *c;
    sim_reset();
    sim_config(0, 0, 0, 0);           // no loss, no delay: the fragment itself is the whole test
    c = TEST_INIT("127.0.0.1", 8001, 1, 1280);   // server, the role that faces the port
    if (!c) _exit(0);                 // could not even start: not this fragment's doing
    sim_begin(0);
    if (len > 0) sim_inject(0, (void*)buf, len);
    (void)big;
    TEST_RUN(c, 0);
    TEST_RUN(c, 0);                   // a second pass: some of the damage lands on the next ack
    _exit(0);
}

static const char *sig_name(int sig)
{
    switch (sig) {
    case SIGFPE : return "SIGFPE (divide by zero?)";
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS : return "SIGBUS";
    case SIGABRT: return "SIGABRT";
    default     : return "signal";
    }
}

static void print_hex(const unsigned char *buf, int len)
{
    int i;
    for (i = 0; i < len && i < 16; i++) printf(" %02x", buf[i]);
    if (len > 16) printf(" ...");
}

// how much memory does an out of order flood cost? frames above a hole that never fills must not
// accumulate: the peer is credit limited to the window we advertise, so anything past it is a lie or a
// duplicate and must be dropped, not stored.
static unsigned long flood_live(int count)
{
    static unsigned char buf[64 * 1024];
    FFRDPCONTEXT *c;
    unsigned long before;
    int i, len;
    sim_reset();
    sim_config(0, 0, 0, 0);
    c = TEST_INIT("127.0.0.1", 8001, 1, 1280);
    if (!c) return 0;
    before = sim_alloc() - sim_freed();
    for (i = 1; i <= count; i++) {
        memset(buf, 0, 32);
        buf[0] = FFRDP_FRAME_TYPE_FULL;
        buf[1] = (unsigned char)( i        & 0xFF);
        buf[2] = (unsigned char)((i >>  8) & 0xFF);
        buf[3] = (unsigned char)((i >> 16) & 0xFF);
        len = 4 + 8;                                   // a plausible payload, so nothing is rejected
        sim_begin(0);
        sim_inject(0, buf, len);
        TEST_RUN(c, 0);
    }
    return (sim_alloc() - sim_freed()) - before;
}

int main(void)
{
    static unsigned char buf[64 * 1024];
    int i, len, dead = 0, shown = 0;
    unsigned long live2k, live4k;
    rnd_state = 20261011u;

    printf("  %d fragments, one forked child each (a crash is data, not the end of the run)\n", FUZZ_CASES);
    for (i = 0; i < FUZZ_CASES; i++) {
        pid_t pid;
        int   st;
        len = fuzz_case(i, buf, (int)sizeof(buf));
        pid = fork();
        if (pid < 0) { printf("  fork failed\n"); return 1; }
        if (pid == 0) run_case(buf, len);
        if (waitpid(pid, &st, 0) < 0) { printf("  waitpid failed\n"); return 1; }
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
            dead++;
            if (shown++ < 8) {
                printf("  CRASH case %3d: %s%s  len=%d |", i,
                       WIFSIGNALED(st) ? sig_name(WTERMSIG(st)) : "exit code",
                       WIFSIGNALED(st) ? "" : " != 0", len);
                print_hex(buf, len);
                printf("\n");
            }
        }
    }
    if (dead) printf("  %d of %d fragments killed the context\n", dead, FUZZ_CASES);
    else      printf("  all %d fragments survived\n", FUZZ_CASES);

    // out of order flood: 2000 vs 4000 frames above a hole. if the second costs about twice the first,
    // memory is growing with whatever arrives, which is a remote exhaustion with no crash involved.
    live2k = flood_live(2000);
    live4k = flood_live(4000);
    printf("  flood above a hole: live bytes %lu (2000 frames) vs %lu (4000 frames)", live2k, live4k);
    if (live2k && live4k > live2k + live2k / 2) printf("  -> UNBOUNDED\n");
    else                                        printf("  -> bounded\n");

    return (dead || (live2k && live4k > live2k + live2k / 2)) ? 1 : 0;
}
