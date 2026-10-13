// 参数解析（v2）—— 把 `ffrdp_init` 的**全部校验分支**与几个键边界钉住。
//
// 为什么单独一个测试：其余测试（e2e / 各单测 / 真机 test.c）**只用合法参数** ✗ ⇒
// 负面用例一条都没有 ✓，而「坏参数」恰恰是最容易被改坏、又最难在真机上发现的部分 ✓
//（真机上只会打一句 log 然后返回 NULL ✓，没人会去逐个试 ✗）。
//
// 覆盖（对应 ffrdp.c 里那四条打印 + 那条范围检查 ✓）：
//   params 为 NULL / scheme 不认识 / 键缺冒号 / 未知键 / maxconn 越界 ✓
// 另外钉住几个「没有校验、但后果很大」的键 ✓：只断言「能建起来、不崩」 ✓，
// 语义交给别的测试（比如 `idle_ms:0` 会在 t_keepalive 那个方向上立刻判死 ✓）。
//
// ⚠️ v2-only ✓（v1 的 ffrdp_init 是另一套签名 ✓）。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../ffrdp.h"
#include "simdefs.h"
#include "../../ffrdp.c"

static int g_fail;
static void check(int ok, const char *what) { printf("  %s %s\n", ok ? "✓" : "✗", what); if (!ok) g_fail++; }

// 事件回调：只有参数过关的上下文才会真的建起来并回调（这里不看事件 ✓）
static long on_event(void *cbctx, const FFRDP_EVENT *ev) { (void)cbctx; (void)ev; return FFRDP_NOT_CONSUMED; }

// 应当**被拒**（返回 NULL ✓）
static void must_reject(const char *tag, const char *params)
{
    void *p = ffrdp_init((void*)params, on_event, NULL);
    check(p == NULL, tag);
    if (p) ffrdp_exit(p);          // 万一没拒，也别把它漏在这儿
}

// 应当**被接受**：返回上下文（调用方负责 exit ✓）
static void *must_accept(const char *tag, const char *params)
{
    void *p = ffrdp_init((void*)params, on_event, NULL);
    check(p != NULL, tag);
    return p;
}

int main(void)
{
    char params[192];
    void *p;

    printf("── 1 应当被拒的坏参数（每条对应 ffrdp.c 的一个分支 ✓）\n");
    must_reject("params == NULL ⇒ NULL（不能空指针解引用 ✗）", NULL);
    must_reject("空串 ⇒ NULL", "");
    must_reject("scheme 不认识（tcp://）⇒ NULL", "tcp://127.0.0.1:8001");
    must_reject("缺 scheme 前缀 ⇒ NULL", "127.0.0.1:8001,mss:1024");
    must_reject("键缺冒号（mss）⇒ NULL", "ffrdps://127.0.0.1:8001,mss");
    must_reject("未知键（foo:1）⇒ NULL", "ffrdps://127.0.0.1:8001,foo:1");
    must_reject("maxconn:0 ⇒ NULL（下越界 ✓）", "ffrdps://127.0.0.1:8001,maxconn:0");
    must_reject("maxconn 非数字（abc ⇒ 0）⇒ NULL", "ffrdps://127.0.0.1:8001,maxconn:abc");
    snprintf(params, sizeof(params), "ffrdps://127.0.0.1:8001,maxconn:%d", FFRDP_MAXCONN + 1);
    must_reject("maxconn = FFRDP_MAXCONN + 1 ⇒ NULL（上越界 ✓）", params);

    printf("── 2 应当被接受：正常写法与边界值\n");
    p = must_accept("最小串（只有 scheme + 地址 ✓）", "ffrdpc://127.0.0.1:8001");
    if (p) ffrdp_exit(p);

    p = must_accept("全部键写一遍 ⇒ 接受 ✓",
                    "ffrdps://127.0.0.1:8001,mss:1024,fec:0,hs:1,maxconn:4,handshake_ms:1000,idle_ms:300");
    if (p) ffrdp_exit(p);

    snprintf(params, sizeof(params), "ffrdps://127.0.0.1:8001,maxconn:%d", FFRDP_MAXCONN);
    p = must_accept("maxconn = FFRDP_MAXCONN（上边界 ✓）", params);
    if (p) ffrdp_exit(p);

    p = must_accept("maxconn:1（下边界 ✓ —— 除主连接外不接任何连接）", "ffrdps://127.0.0.1:8001,maxconn:1");
    if (p) ffrdp_exit(p);

    printf("── 3 没有范围校验的键：只要求能建起来、不崩 ✓（后果由别的测试盖 ✓）\n");
    p = must_accept("mss:8（极小，但 4 对齐 ✓）", "ffrdps://127.0.0.1:8001,mss:8,hs:0");
    if (p) ffrdp_exit(p);
    p = must_accept("handshake_ms:0（握手立刻到期 ✓）", "ffrdps://127.0.0.1:8001,handshake_ms:0,hs:1");
    if (p) ffrdp_exit(p);
    p = must_accept("idle_ms:0（⚠️ 会立刻判死 —— 配置地雷，这里只钉住它能建起来 ✓）",
                    "ffrdps://127.0.0.1:8001,idle_ms:0,hs:1");
    if (p) ffrdp_exit(p);
    p = must_accept("hs:0（老路径 ✓）", "ffrdps://127.0.0.1:8001,hs:0");
    if (p) ffrdp_exit(p);

    printf("── 4 fec 的钳制（0..32 ✓）：超出范围不报错，被折进区间 ✓\n");
    p = must_accept("fec:40（> 32 ⇒ 折到 32 ✓）", "ffrdps://127.0.0.1:8001,fec:40,hs:0");
    if (p) {
        check(((FFRDPCONTEXT*)p)->conn.fec_txredundancy == 32, "fec:40 ⇒ fec_txredundancy = 32 ✓");
        ffrdp_exit(p);
    }
    p = must_accept("fec:-5（< 0 ⇒ 折到 0 ✓）", "ffrdps://127.0.0.1:8001,fec:-5,hs:0");
    if (p) {
        check(((FFRDPCONTEXT*)p)->conn.fec_txredundancy == 0, "fec:-5 ⇒ fec_txredundancy = 0 ✓");
        ffrdp_exit(p);
    }
    p = must_accept("fec:3（区间内 ✓）", "ffrdps://127.0.0.1:8001,fec:3,hs:0");
    if (p) {
        check(((FFRDPCONTEXT*)p)->conn.fec_txredundancy == 3, "fec:3 ⇒ fec_txredundancy = 3 ✓");
        ffrdp_exit(p);
    }

    printf("── 5 同一进程里建多个上下文（各自独立 ✓）\n");
    {   void *a = must_accept("第二个上下文也建得起来 ✓", "ffrdps://127.0.0.1:8001,hs:0");
        void *b = must_accept("第三个（客户端）✓", "ffrdpc://127.0.0.1:8001,hs:0");
        if (a != NULL && b != NULL) check(a != b, "两个上下文是不同的对象 ✓");
        if (a) ffrdp_exit(a);
        if (b) ffrdp_exit(b);
    }

    printf("       verdict -> %s\n", g_fail ? "FAIL" : "OK");
    return g_fail ? 1 : 0;
}
