#ifndef _UTILS_H_
#define _UTILS_H_

#include <stdint.h>

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

#ifdef WIN32
#include <windows.h>
#define get_timestamp32_ms GetTickCount64
#else
#define get_timestamp32_ms ffrdp_get_timestamp32_ms
#endif

#define module_thread_get_state ffrdp_module_thread_get_state
#define module_thread_set_state ffrdp_module_thread_set_state
#define module_thread_dump      ffrdp_module_thread_dump
#define module_thread_check_exit(mt)  ((mt)->flags & MOD_THREAD_FLAG_STOP)
#define module_thread_check_pause(mt) (((mt)->flags & MOD_THREAD_FLAG_PAUSE_REQ) ? ((mt)->flags |= MOD_THREAD_FLAG_PAUSE_OK, 1) : 0)
#define thread_setname ffrdp_thread_setname
#define thread_getname ffrdp_thread_getname

typedef struct {
    #define MOD_THREAD_FLAG_STOP      (1 << 0)
    #define MOD_THREAD_FLAG_PAUSE_REQ (1 << 1)
    #define MOD_THREAD_FLAG_PAUSE_OK  (1 << 2)
    uint32_t    flags;
    void     *hthread;
    void*      (*proc)(void *arg);
    void         *arg;
} MOD_THREAD;

uint32_t ffrdp_get_timestamp32_ms(void);

/* ── 32 位毫秒时间戳的比较工具：时间判断一律走这里，别再手写 `(int32_t)a - (int32_t)b` ──
 *
 * 时间戳来自 CLOCK_MONOTONIC 的 32 位毫秒（Windows 上是 GetTickCount64 截断），含义是"开机以来的毫秒"：
 *   · 每 2^32 ms（≈49.7 天）回绕一次；
 *   · **开机超过 2^31 ms（≈24.85 天）之后，now 的最高位就是 1**（当成 int32 读就是负数）。
 *
 * 由此两条铁律（libffp2p 的 common/ffp2p-time.h 是同一套，改一边记得改另一边）：
 *   ① 比较两个**真实时刻**用无符号相减、按有符号解释：`(int32_t)(a - b)`。只要相隔 < 2^31 ms，
 *      跨回绕也能得到正确先后关系 —— `(int32_t)a - (int32_t)b` 形式虽然结果一样，但 a/b 单独
 *      转 int32 会读出负数，写起来仍然容易踩坑 ⇒ 统一用下面的函数。
 *   ② **不要把 0 当"很久以前 / 还没到点"的哨兵再去相减**：high bit 为 1 时 `(int32_t)(now - 0)`
 *      是负数 ⇒ "已经过了 D 毫秒"恒假、"已经到点"恒真，判断会**静默失效**（不崩、不打日志，
 *      只是那条逻辑永远不走）。要表达"没设置 / 立刻到点"用 ffrdp_ts_due()；字段初值 0 且语义是
 *      "从没发生过"的，用 ffrdp_ts_passed_or_zero()（它把 0 当开机时刻算，仿真 t=0 也不失真）。
 *
 * 本仓库实测踩过的坑（都是 uptime 超过 24.85 天的机器上才复现）：
 *   · test.c 的 g_dump_tick 初值 0 ⇒ 整轮联调一条 `[server] status:` 都没有（发了 11 MB 也不打）；
 *   · tick_win 初值 0   ⇒ 证据窗口永不开启（RTO 地板 / hole_wait 那套证据不更新）；
 *   · rate_tick 初值 0  ⇒ 每秒速率采样永不执行（rate_now / probe_waste / cap 自适应全静默）；
 *   · probe_next 初值 0 ⇒ 探测永不启动；tick_rto_backoff 初值 0 ⇒ RTO 永不退避；
 *   · tick_cap_rise/revert、last_giveback 初值 0 ⇒ 升档 / giveback 永不发生。
 * 现象统一是"短 uptime 的机器正常、长 uptime 的机器那几条功能像没写"。
 * ------------------------------------------------------------------------------------ */

/* now - then（ms）：>0 已经过了这么久，<0 then 还在未来。两个参数都必须是真实时刻。 */
static inline int32_t ffrdp_ts_diff(uint32_t now, uint32_t then)
{
    return (int32_t)(now - then);
}

/* now 是否已到 when。when == 0 表示"没设置" ⇒ 按"立刻到点"处理（别拿 0 去相减）。 */
static inline int ffrdp_ts_due(uint32_t now, uint32_t when)
{
    return !when || (int32_t)(now - when) >= 0;
}

/* 距 since 是否已过 ms 毫秒。since 必须是**真实时刻**（调用前已赋过值）。 */
static inline int ffrdp_ts_passed(uint32_t now, uint32_t since, int32_t ms)
{
    return (int32_t)(now - since) >= ms;
}

/* 同上，但允许 since == 0（"从没发生过"）—— 用于初值为 0 的 tick 字段：
 * 把 0 当成"开机那一刻"来算已经过了多久（= now 本身），于是
 *   · 真实机器上（进程远晚于开机启动）⇒ 早就过了（返回 1），与短 uptime 下 0 的表现一致；
 *   · 虚拟时钟仿真（tools/sim，t 从 0 起算）⇒ now < ms 时返回 0，与旧代码逐字一致，不会造出假 diff。
 * 直接写 `(int32_t)(now - 0) >= ms` 在 uptime > 24.85 天时是负数 ⇒ 恒假（本仓库踩过的坑）。 */
static inline int ffrdp_ts_passed_or_zero(uint32_t now, uint32_t since, int32_t ms)
{
    if (ms <= 0) return 1;
    if (!since)  return now >= (uint32_t)ms;
    return (int32_t)(now - since) >= ms;
}

/* a 是否早于 b（两者都必须是真实时刻，用于"取最早期限"那种比较）。 */
static inline int ffrdp_ts_before(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) < 0;
}

/* since 起到 now 过了多少毫秒。since == 0（从没发生过）⇒ 按"从开机那一刻起"算（= now）。
 * 结果放在 64 位里，永远是 0..2^32-1 的非负数 ⇒ 可以放心用 > / >= 和任意阈值比，
 * 也不会像 (int32_t)now 那样在 uptime > 24.85 天时变成负数。 */
static inline int64_t ffrdp_ts_elapsed(uint32_t now, uint32_t since)
{
    return (int64_t)(since ? (uint32_t)(now - since) : now);
}

/* now 之后 ms 毫秒对应的时刻（ms 可以为负）。 */
static inline uint32_t ffrdp_ts_after(uint32_t now, int32_t ms)
{
    return now + (uint32_t)ms;
}

int  ffrdp_module_thread_get_state(MOD_THREAD *mt);
void ffrdp_module_thread_set_state(MOD_THREAD *mt, int state);
void ffrdp_module_thread_dump(MOD_THREAD *mt, char *str, int len);
int  ffrdp_thread_setname(char *name);
int  ffrdp_thread_getname(char *name, int len);

// ---- 地址约定（协议层与传输层共用，就这一份）----
// 一个 uint64_t 表示一个对端：IPv4 地址在高 32 位、端口在低 16 位，**都是主机序**
// （所以 127.0.0.1 打印出来就是 0x7F000001，而不是小端机上 s_addr 那个字节序颠倒的数）。
// 这个布局以前只有 lowlevel-udp.c 知道；现在放在这里 —— 它不是某个后端的实现细节，
// 而是两条层共同的约定（协议层要能给还没开口的对端起名，传输层要能把它交给 send()）。
#define FFRDP_ADDR_DEFAULT_PORT 8000

uint64_t ffrdp_addr_make(uint32_t ip, uint16_t port);
void     ffrdp_addr_split(uint64_t addr, uint32_t *ip, uint16_t *port);
// 解析 "ip[:port]"：省略端口时用 default_port（**< 0 表示端口必须显式给**）。
// 成功时 *ip / *port 是主机序；端口只做 0..65535 的范围检查 —— "对端必须给真端口"这类
// 策略由调用方决定（协议层拒绝 0，bind 允许 0 = 由内核选）。
// 地址部分要求严格的四段点分十进制（不引 socket 头，所以协议层也能用）。
int ffrdp_addr_parse_text(char *text, uint32_t *ip, int *port, int default_port);

#if defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif
