// A fake lowlevel for the sim harness: the transport the protocol talks to when a test drives it
// directly, one frame at a time, instead of a real socket with its own worker thread.
//
// It keeps the same network model the socket macros always used (sim_send / sim_recv, in simnet.c)
// and adds only what lowlevel.h gives the protocol that a bare socket did not: a per frame receive
// callback, and a timer request. timer() is recorded and never fired from here -- the test's own
// sim_ll_run() is what delivers a TIMER, once per step. Firing it from inside timer() would run a
// tick the test did not ask for, and it would re-enter the protocol while the application thread
// still holds the context lock (ffrdp_send calls timer() from under that lock). So the stepping
// model of the tests is unchanged: one step, one TIMER.
#include <stdint.h>
#include <string.h>
#include "simdefs.h"
#include "../../lowlevel.h"

// In the sim an addr is opaque: sim_send() ignores it. It must still be non zero, because a server
// stores the addr of the first frame it receives as its peer (ffrdp_lowlevel_cb), and 0 is "no peer".
#define SIMLL_ADDR 0x7F00000100005001ULL

#define SIMLL_MAX 8 // the two endpoint tests, plus room for however many a later test wants

typedef struct {
    FFRDP_LOWLEVEL    ll;
    FFRDP_LOWLEVEL_CB cb;
    void             *cbctx;
    int               used;
} SIMLL;

static SIMLL g_ll[SIMLL_MAX];

static SIMLL *simll_find_ctx(void *cbctx)
{
    int i;
    for (i = 0; i < SIMLL_MAX; i++) if (g_ll[i].used && g_ll[i].cbctx == cbctx) return &g_ll[i];
    return NULL;
}

static int simll_send(void *fl, uint64_t addr, uint8_t *buf, int len)
{
    (void)fl; (void)addr;
    // sim_send returns the length it took, always > 0 here: the lowlevel contract is 0 = accepted.
    return sim_send(buf, len) > 0 ? 0 : -1;
}

static int simll_timer(void *fl, uint32_t deadline_ms)
{
    // Recorded, never fired: see the file comment. The deadline is not needed either -- sim_ll_run()
    // delivers exactly one TIMER per step, which is the granularity the tests are written against.
    (void)fl; (void)deadline_ms;
    return 0;
}

static void simll_exit(void *fl)
{
    SIMLL *s = (SIMLL*)fl; // ll is the first member, so the handle is the instance
    if (s) s->used = 0;
}

static int simll_addr_parse(void *fl, const char *text, uint64_t *addr)
{
    (void)fl; (void)text;
    if (!addr) return -1;
    *addr = SIMLL_ADDR; // opaque in the sim; send() ignores it
    return 0;
}

void *sim_ll_init(void *params, FFRDP_LOWLEVEL_CB cb, void *cbctx)
{
    int i;
    (void)params; // "bind:..." / "none": there is nothing to bind in the sim
    for (i = 0; i < SIMLL_MAX; i++) {
        if (!g_ll[i].used) {
            SIMLL *s = &g_ll[i];
            memset(s, 0, sizeof(*s));
            s->used         = 1;
            s->cb           = cb;
            s->cbctx        = cbctx;
            s->ll.send      = simll_send;
            s->ll.timer     = simll_timer;
            s->ll.exit      = simll_exit;
            s->ll.addr_parse= simll_addr_parse;
            return &s->ll;
        }
    }
    return NULL;
}

// Drive one endpoint for one step -- the replacement for the old ffrdp_update(ctxt):
//   sim_begin(idx) picks the current endpoint, which is what sim_send/sim_recv key on;
//   every packet already queued for idx is handed over as one RECV callback;
//   then one TIMER callback runs what is due now (retransmits, probes, acks).
// The RECV/TIMER split and the one-frame-per-receive granularity both come straight from the real
// transport (lowlevel-udp.c), so a step here has the same shape as a lap there.
void sim_ll_run(int idx, void *ctxt)
{
    SIMLL        *s;
    unsigned char tmp[2048];
    sim_begin(idx);
    s = simll_find_ctx(ctxt);
    if (!s) return;
    while (sim_queued(idx) > 0) {
        int n = sim_recv(tmp, (int)sizeof(tmp));
        FFRDP_LOWLEVEL_MSG msg;
        if (n <= 0) break; // queued but not yet due: leave it for a later step
        msg.addr = SIMLL_ADDR;
        msg.buf  = tmp;
        msg.len  = n;
        s->cb(s->cbctx, FFRDP_LOWLEVEL_TYPE_RECV, &msg, sizeof(msg));
    }
    s->cb(s->cbctx, FFRDP_LOWLEVEL_TYPE_TIMER, NULL, 0);
}
