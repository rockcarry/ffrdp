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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "simdefs.h"
#ifdef SIMLL_BASE
#include "lowlevel_base.h"
#else
#include "../../lowlevel.h"
#endif

// 接口改动还在飞的时候，某一侧的头文件里可能还留着 addr_parse 槽位（那个约定后来搬去了 utils.h）。
// run.sh 按头文件探测，探测到才传对应的 -DSIMLL_*_HAS_ADDR_PARSE。这一处是唯一给
// HAVE_ADDR_PARSE_SLOT 取值的地方，且在所有使用点之前。
#if defined(SIMLL_BASE_HAS_ADDR_PARSE) || defined(SIMLL_NEW_HAS_ADDR_PARSE)
#define HAVE_ADDR_PARSE_SLOT 1
#endif

// In the sim an addr is opaque: sim_send() ignores it. It must still be non zero, because a server
// stores the addr of the first frame it receives as its peer (ffrdp_lowlevel_cb), and 0 is "no peer".
#define SIMLL_ADDR 0x7F00000100005001ULL

#define SIMLL_MAX 8 // the two endpoint tests, plus room for however many a later test wants

#define SIMLL_ROUTES 8   // 一个假 lowlevel 能登记几个对端（v2 的多连接测试要按地址分发）

typedef struct {
    FFRDP_LOWLEVEL    ll;
    FFRDP_LOWLEVEL_CB cb;
    void             *cbctx;
    int               used;
    uint32_t          due;    // 协议 arm 的绝对毫秒期限（0 = 无）；忠实模型靠它决定何时给 TIMER
    uint64_t          own;    // 本实例自己的地址：回给已登记对端时，包里带的源地址
    int               nrt;    // 已登记的"对端地址 ⇒ 目的 slot"条数（0 = 走旧路径）
    struct { uint64_t addr; int slot; } rt[SIMLL_ROUTES];
} SIMLL;

static SIMLL g_ll[SIMLL_MAX];
static unsigned long g_guard_hits; // 忠实模型防自旋的计数（非 0 时 exit 会提示）

static SIMLL *simll_find_ctx(void *cbctx)
{
    int i;
    for (i = 0; i < SIMLL_MAX; i++) if (g_ll[i].used && g_ll[i].cbctx == cbctx) return &g_ll[i];
    return NULL;
}

static int simll_send(void *fl, uint64_t addr, uint8_t *buf, int len)
{
    SIMLL *s = (SIMLL*)fl;   // ll 是第一个成员，所以句柄就是实例（与 simll_exit 同一招）
    int    i;
    // 登记过的对端：按地址路由到它所在的 slot。这就是 v2 多连接要用的那条路。
    if (s) for (i = 0; i < s->nrt; i++) {
        if (s->rt[i].addr == addr) {
            return sim_send_from(s->rt[i].slot, s->own ? s->own : SIMLL_ADDR, buf, len) > 0 ? 0 : -1;
        }
    }
    // 未登记：与以前逐字相同（忽略 addr，发到"对面"那个 slot）
    // sim_send returns the length it took, always > 0 here: the lowlevel contract is 0 = accepted.
    return sim_send(buf, len) > 0 ? 0 : -1;
}

static int simll_timer(void *fl, uint32_t deadline_ms)
{
    // 不在 timer() 里触发（见文件头注释：会在调用方持锁时重入协议）。但期限现在要记下来 --
    // sim_ll_run() 用它区分"真有一次 tick 到期"和"只是步进计数器又走了一格"，这正是真实传输
    // 与"每毫秒叫醒一次"的差别。
    SIMLL *s = (SIMLL*)fl;
    if (s) s->due = deadline_ms;
    return 0;
}

static void simll_exit(void *fl)
{
    SIMLL *s = (SIMLL*)fl; // ll is the first member, so the handle is the instance
    if (g_guard_hits) fprintf(stderr, "[simll] deadline mode guard hit %lu time(s)\n", g_guard_hits);
    if (s) s->used = 0;
}

#ifdef HAVE_ADDR_PARSE_SLOT
// 老接口：把 "ip:port" 解析成 addr。仿真里 addr 是不透明的 —— sim_send() 根本不看它 —— 只要非 0
// 就够；服务端会把收到的第一帧的 addr 当作对端。
static int simll_addr_parse(void *fl, const char *text, uint64_t *addr)
{
    (void)fl; (void)text;
    if (!addr) return -1;
    *addr = SIMLL_ADDR;
    return 0;
}
#endif

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
#ifdef HAVE_ADDR_PARSE_SLOT
            s->ll.addr_parse = simll_addr_parse;   // 只有那一侧的头文件里还有这个槽位时才补
#endif
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
static void simll_deliver(SIMLL *s, unsigned char *tmp, int n)
{
    FFRDP_LOWLEVEL_MSG msg;
    uint64_t src = sim_recv_src();
    // 旧路径（slot 未登记地址）的源地址是 0 ⇒ 仍报那个老常量，与以前逐字相同；
    // 登记过就报真实来源地址 ⇒ 上层才能按地址分发（v2 的 conn 查表就靠它）。
    msg.addr = src ? src : SIMLL_ADDR;
    msg.buf  = tmp;
    msg.len  = n;
    s->cb(s->cbctx, FFRDP_LOWLEVEL_TYPE_RECV, &msg, sizeof(msg));
}

void sim_ll_run(int idx, void *ctxt)
{
    SIMLL        *s;
    unsigned char tmp[2048];
    int           n, guard;
    sim_begin(idx);
    s = simll_find_ctx(ctxt);
    if (!s) return;

    if (getenv("SIM_LL_DEADLINE")) {
        // 忠实模型：真实传输只在"有帧到达"或"期限到了"时唤醒工作线程，不是每毫秒一次。
        // 每次唤醒 = 一次回调；回调里协议会重新 arm 期限，所以循环到"没有帧、也没有到期"为止。
        for (guard = 0; guard < 100000; guard++) {
            if (sim_queued(idx) > 0 && (n = sim_recv(tmp, (int)sizeof(tmp))) > 0) { simll_deliver(s, tmp, n); continue; }
            if (s->due != 0 && (int32_t)(s->due - (int32_t)sim_tick()) <= 0) {
                s->due = 0;
                s->cb(s->cbctx, FFRDP_LOWLEVEL_TYPE_TIMER, NULL, 0);
                continue;
            }
            break;
        }
        if (guard == 100000) g_guard_hits++;
        return;
    }

    while (sim_queued(idx) > 0) { // 旧模型（默认）：每步无条件一次 TIMER
        n = sim_recv(tmp, (int)sizeof(tmp));
        if (n <= 0) break; // queued but not yet due: leave it for a later step
        simll_deliver(s, tmp, n);
    }
    s->cb(s->cbctx, FFRDP_LOWLEVEL_TYPE_TIMER, NULL, 0);
}

// ── 多对端路由（P0 新增）──────────────────────────────────────────────────────
// 登记一条"对端地址 ⇒ 目的 slot"。没有登记过的对端仍走旧路径（发到 1-cursid 那个 slot），
// 所以老的两端测试完全不受影响：它们一条都不登记。
int sim_ll_route(void *fl, uint64_t addr, int slot)
{
    SIMLL *s = (SIMLL*)fl;
    int    i;
    if (!s || slot < 0 || slot >= SIMNET_SLOTS) return -1;
    for (i = 0; i < s->nrt; i++) if (s->rt[i].addr == addr) { s->rt[i].slot = slot; return 0; }
    if (s->nrt >= SIMLL_ROUTES) return -1;
    s->rt[s->nrt].addr = addr;
    s->rt[s->nrt].slot = slot;
    s->nrt++;
    return 0;
}

void sim_ll_own(void *fl, uint64_t addr)
{
    SIMLL *s = (SIMLL*)fl;
    if (s) s->own = addr;
}
