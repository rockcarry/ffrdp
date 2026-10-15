// `ffrdp_dump` 的边界（v2）—— 诊断函数也不能在坏参数上崩 ✓。
//
// 覆盖：conn < 0（总状态 ✓）/ 0（主连接 ✓）/ 已 accept 的（✓）/ 空槽（✓）/ 越界（✓）/
//       已放掉的槽（✓）/ clearhistory（✓）/ ctxt == NULL（✓ —— 这条以前会空指针解引用 ✗）。
//
// 手法：搭一对真上下文（服务端 + 客户端 ✓，照 t_keepalive 的登记法 ✓）并把它握上手；
//       先把「还活着」的各种 conn 值都 dump 一遍 ✓，再让连接被 keepalive 判死 ✓，
//       对**已放掉**的那个槽再 dump 一遍 ✓（这是最容易踩的一条 ✓）。
//
// ⚠️ v2-only ✓。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>   // dup/dup2：把 dump 的输出抓下来数块数 ✓
#include "../../ffrdp.h"
#include "simdefs.h"
#include "../../ffrdp.c"

// 块头前缀：ffrdp.c 打的是四个 ASCII 连字符加 " conn "（2026-10-15 起 dump 全改 ASCII ✓）。
// 改到这里要同步 ✓：它按 dump 的**文本**数块 ⇒ 前缀一变、计数立刻归零 ⇒ 三条断言假红 ✗
// （2026-10-14 已经踩过一次同类坑：当时只写了两个 ─ ⇒ 计数恒为 0 ✓）。别写死字节数，用 strstr ✓。
#define DUMP_HDR "---- conn "

#define MKADDR(port) ((uint64_t)(0x7F000001ULL << 16) | (uint64_t)(port))
#define A_SRV MKADDR(8001)
#define A_C1  MKADDR(40001)
#define IDLE_MS 300

static int g_fail;
static void check(int ok, const char *what) { printf("  %s %s\n", ok ? "✓" : "✗", what); if (!ok) g_fail++; }

struct app { int accept_n; int conn; int disconn_n; };
static long on_event(void *cbctx, const FFRDP_EVENT *ev)
{
    struct app *a = (struct app*)cbctx;
    if (!a) return FFRDP_NOT_CONSUMED;
    if (ev->type == FFRDP_ACCEPT)       { a->accept_n++; a->conn = ev->conn; }
    else if (ev->type == FFRDP_DISCONN) a->disconn_n++;
    return FFRDP_NOT_CONSUMED;
}

static int g_step;

// ── 数一份 dump 打了几块 ──────────────────────────────────────────────────────
// 为什么要数：`conn` 参数以前被忽略 ✗（整个表都打一遍），服务端于是每次 dump 都多出
// 一大段"0 号槽"的零 ✗ —— 看起来就像同一台机器的另一个时刻（2026-10-14 实测误导过 ✓）。
// 这里把 stdout 临时接到一个临时文件上，数"块头"（以 "──── conn " 开头 ✓）的行数 ✓。
static void   *g_dump_ctx;
static int     g_dump_conn, g_dump_clear;
static void    do_dump(void) { ffrdp_dump(g_dump_ctx, g_dump_conn, g_dump_clear); }

static int count_blocks(void *ctx, int conn, int clearhistory)
{
    FILE *f;
    int   save, n = 0;
    char  line[512];
    g_dump_ctx = ctx; g_dump_conn = conn; g_dump_clear = clearhistory;
    fflush(stdout);
    save = dup(fileno(stdout));
    f    = tmpfile();
    if (save < 0 || !f) return -1;
    dup2(fileno(f), fileno(stdout));
    do_dump();
    fflush(stdout);
    dup2(save, fileno(stdout));
    close(save);
    rewind(f);
    while (fgets(line, sizeof(line), f)) if (strstr(line, DUMP_HDR) != NULL) n++;
    fclose(f);
    return n;
}
static void drive(int steps, void *p_c1, void *p_srv)
{
    int i;
    for (i = 0; i < steps; i++) {
        sim_setstep(g_step++);
        sim_begin(1); sim_ll_run(1, p_c1);
        sim_begin(0); sim_ll_run(0, p_srv);
    }
}

int main(void)
{
    void *p_srv, *p_c1;
    FFRDPCONTEXT *srv;
    struct app a_srv, a_c1;
    int   n;

    sim_reset(); sim_config(0.0, 0, 4, 0);
    memset(&a_srv, 0, sizeof(a_srv)); memset(&a_c1, 0, sizeof(a_c1));
    a_srv.conn = a_c1.conn = -1;
    p_srv = ffrdp_init("ffrdps://127.0.0.1:8001,mss:1024,fec:0,idle_ms:300", on_event, &a_srv);
    p_c1  = ffrdp_init("ffrdpc://127.0.0.1:8001,mss:1024,fec:0,idle_ms:300", on_event, &a_c1);
    check(p_srv && p_c1, "两个上下文都建起来了");
    if (!p_srv || !p_c1) { printf("       verdict -> FAIL\n"); return 1; }
    srv = (FFRDPCONTEXT*)p_srv;
    sim_ll_own(srv->lowlevel, A_SRV); sim_ll_own(((FFRDPCONTEXT*)p_c1)->lowlevel, A_C1);
    sim_ll_route(srv->lowlevel, A_C1, 1); sim_ll_route(((FFRDPCONTEXT*)p_c1)->lowlevel, A_SRV, 0);

    drive(IDLE_MS * 2, p_c1, p_srv);
    check(a_srv.accept_n == 1 && a_srv.conn >= 1, "先握手成功（拿到 accept 句柄 ✓）");

    printf("── 1 还活着的各种 conn 值\n");
    ffrdp_dump(p_srv, -1, 0);                    check(1, "dump(ctxt, -1) ⇒ 总状态（走完不崩 ✓）");
    ffrdp_dump(p_srv, 0, 0);                     check(1, "dump(ctxt, 0) ⇒ 主连接 ✓");
    ffrdp_dump(p_srv, a_srv.conn, 0);            check(1, "dump(ctxt, accept 句柄) ⇒ 那一条 ✓");
    ffrdp_dump(p_srv, a_srv.conn, 1);            check(1, "dump(..., clearhistory=1) ✓");

    printf("── 2 空槽 / 越界 / 空 ctxt（都不该崩 ✓）\n");
    ffrdp_dump(p_srv, 3, 0);                     check(1, "dump(ctxt, 3) ⇒ 空槽 ✓");
    ffrdp_dump(p_srv, 999, 0);                   check(1, "dump(ctxt, 999) ⇒ 越界 ✓");
    ffrdp_dump(p_srv, FFRDP_MAXCONN, 0);         check(1, "dump(ctxt, FFRDP_MAXCONN) ⇒ 越界边界 ✓");
    ffrdp_dump(NULL, 0, 0);                      check(1, "dump(NULL, 0) ⇒ 空 ctxt（以前会空指针解引用 ✗）");
    ffrdp_dump(NULL, -1, 0);                     check(1, "dump(NULL, -1) ⇒ 空 ctxt + 总状态 ✓");

    printf("── 3 conn 参数要真的选连接（不是『整个表都打一遍』✗）\n");
    n = count_blocks(p_srv, -1, 0);              check(n == 2, "dump(ctxt, -1) 打出 2 块（0 号内嵌主连接 + accept 那条 ✓）");
    n = count_blocks(p_srv, 0, 0);               check(n == 1, "dump(ctxt, 0) 只打 1 块（0 号槽 ✓）");
    n = count_blocks(p_srv, a_srv.conn, 0);      check(n == 1, "dump(ctxt, accept 句柄) 只打 1 块 ✓");
    n = count_blocks(p_srv, 3, 0);               check(n == 0, "dump(ctxt, 空槽) 一块都不打（提前 return ✓）");

    printf("── 4 连接被放掉之后，对那个槽再 dump（最容易踩的一条 ✓）\n");
    sim_config(1.0, 0, 4, 0);                    // 之后谁都听不到谁
    drive(IDLE_MS * 3, p_c1, p_srv);
    check(a_srv.disconn_n == 1 && srv->table[a_srv.conn] == NULL, "连接已被 keepalive 判死并收摊 ✓");
    ffrdp_dump(p_srv, a_srv.conn, 0);            check(1, "dump(ctxt, 已放掉的槽) ⇒ 不崩 ✓");
    ffrdp_dump(p_srv, -1, 0);                    check(1, "dump(ctxt, -1) ⇒ 再打一次总状态 ✓");

    ffrdp_exit(p_c1); ffrdp_exit(p_srv);

    printf("       verdict -> %s\n", g_fail ? "FAIL" : "OK");
    return g_fail ? 1 : 0;
}
