#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include "ffrdp.h"
#include "utils.h"

#define TEST_VERSION            "1.1.0" // 与 build.sh 生成的 libffrdp.pc 里的 Version 保持一致
#define TEST_DUMP_INTERVAL_MS   10000   // 每隔这么久打印一次诊断
#define TEST_PEER_SILENCE_MS    2000    // 这么久没收到对端数据就认为它失联（见 peer_is_lost）
#define TEST_SPEED_INTERVAL_MS  1000    // 发送限速：每这么久补一次令牌（见 peer_thread）
#define TEST_STREAM_MAX        (256 * 1024) // 接收侧累积缓冲：ffrdp_recv 是字节流，消息要自己拼回来
#define TEST_RECV_WAIT_MS       10      // 没有数据时 ffrdp_recv 等这么久：应用侧的节拍点

// 每条消息（全小端）：[4B len][4B seq][payload][4B crc32]；len 含头尾，crc 覆盖 len+seq+payload。
// ffrdp_recv 是有序字节流（没有消息边界）⇒ 必须自带定长头；ffrdp_send 是原子的 ⇒ 只有 ret == len 时才 seq+1。
#define TEST_MSG_OVERHEAD       12

// ── A1：真机逐消息延迟量具（notes §26.93）────────────────────────────────────
// 为什么要它 ✗：这一层原来只有"字节数 / 重传率"，而延迟相关的判据（pacing / 截止时间 / 重传等待）
// 全是**分位数** ⇒ 以前只能靠吞吐侧面猜 ✓（仿真那一半已在 notes §26.92 落地 ✓）。
//
// 为什么做成开关 ✗✓：它改的是**消息格式**（尾部多 4 字节发送时刻），而 v1 互通矩阵用的是 dist 里
// 另一份旧格式的 `test_ffrdp_v11_*` ⇒ 默认关 = 与改造前**逐字节相同** ✓；两端都要 `TEST_STAMP=1` ✓。
//
// 时间戳放在 **crc 之后**（`[len][seq][payload][crc][stamp]`）⇒ crc 的覆盖范围与改造前逐字相同
// ⇒ "每次重发前刷新时间戳"不必重算 crc ✓（16 KB 的 crc 在 60 MB/s 下不是免费的 ✗）。
// 分桶用 **100 µs** 一档（不是 1 ms ✗）：lo 上 p50 落在 0~1 ms，毫秒档会把整条分布压成一个 0 ✓
// ⇒ 覆盖 0..1024 ms 共 10240 档（约 40 KB/连接，只在栈上、随线程退出即消失 ✓）。
#define TEST_LAT_UNIT_US 100
#define TEST_LAT_MAX_MS  1024
#define TEST_LAT_BUCKETS ((TEST_LAT_MAX_MS) * (1000 / TEST_LAT_UNIT_US))

static int g_stamp = -1;                 // -1 = 还没读环境变量
static int stamp_on(void)
{
    if (g_stamp < 0) { const char *e = getenv("TEST_STAMP"); g_stamp = (e && *e) ? (atoi(e) != 0) : 0; }
    return g_stamp;
}
static int msg_oh(void) { return TEST_MSG_OVERHEAD + (stamp_on() ? 4 : 0); }   // 一条消息的固定开销

// 退出只有一条路：输入 quit/exit（或 stdin 结束）⇒ 收尾 + verify 汇总 + ffrdp_free；Ctrl+C 直接结束、看不到汇总（接受的代价）。
static volatile int g_exit = 0;
static char server_bind_ip[32]   = "0.0.0.0";
static char client_cnnt_ip[32]   = "127.0.0.1";
static int  server_bind_port     = 8000;
static int  client_cnnt_port     = 8000;

// ①-41 的两个应用侧旋钮（env ✓，默认 = 老行为 ✓）：
//   TEST_APP_SENDRETRY：send 被拒后立刻重试几次（0 = 下一个循环再说，会白等一次 10ms 的 recv ✗）；
//   TEST_APP_SENDWAIT_MS：送进 ffrdp_send 的等待额 —— 队列删掉之后，反压改由协议在 cond 上做 ✓。
static int  app_sendwait_get(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("TEST_APP_SENDWAIT_MS"); v = (e && *e) ? atoi(e) : 0; }
    return v;   // 0 = 老行为（立即被拒）；>0 = 最多等这么久；<0 = 等到能放 ✓
}

static int  app_sendretry_get(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("TEST_APP_SENDRETRY"); v = (e && *e) ? atoi(e) : 0; }
    return v;
}

static int  server_max_send_size = 16 * 1024;
static int  client_max_send_size = 16 * 1024;
static int  send_speed           = 0;   // 本进程的发送限速，单位 KB/s（1024 B）；0 = 不限速
static int  g_idle_sec           = 0;   // FFRDP_IDLE>0：**不发应用数据**，专门验 keepalive 不误杀 ✓
static int  g_do_close           = 0;   // FFRDP_CLOSE=1：退出前用 ffrdp_close() 主动关（验四次挥手）✓
static int  g_use_hs             = 1;   // 本侧是否走握手（FFRDP_HS=0 ⇒ 老路径 ⇒ 服务端句柄就是 0 ✓）
// ①-22：应用侧突发开关（env TEST_APP_BURST>0 才打开 ✓，默认 0 ⇒ 一个字节的行为都不动 ✓）——
// 用来把"应用节拍"与"协议容量"分开（v1.1 每圈搬 ≈65 KB、v2 ≈8.2 KB ⇒ 混在一起指不出等在哪 ✗）。
static int  g_app_burst          = 0;
// ①-32b：应用侧等待点做成旋钮（env TEST_APP_RECVWAIT_MS / TEST_APP_SPIN_US ✓）。默认 10/0 ⇒ 与现状逐字一致 ✓
//（RECVWAIT=0 ⇒ 非阻塞 ✓；没有数据时 SPIN_US 短睡，免得空转吃满 CPU ✗）。
static int  g_recv_wait_ms       = TEST_RECV_WAIT_MS;
static int  g_spin_us            = 0;
static pthread_mutex_t g_mutex;

// ①-23：应用侧的 µs 时钟（只读 ✓）；ms 口径看不出"带超时的 recv"那一段的节奏 ✓。
static uint32_t app_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull);
}

// 一个"对等端"的运行参数。两侧跑的是同一个循环，差别只在下面这几项。
typedef struct {
    const char *name;      // 本端名字（打印用）
    const char *peer;      // 对端名字（打印用）
    const char *msg_up;    // 第一次收到对端数据时打印的那句
    int         is_server; // 传给 ffrdp_init：1 = 服务端，0 = 客户端
    char       *ip;        // 绑定 / 连接地址
    int        *port;      // 端口（指向命令行解析出来的全局变量）
    int        *send_size; // 本端每条消息的最大字节数（含 framing 头尾）
    int        *send_speed;// 发送限速 KB/s（0 = 不限速），指向命令行解析出来的全局变量
} TEST_PEER;

static TEST_PEER g_peers[] = {
    { "server", "client", "client connected !"    , 1, server_bind_ip, &server_bind_port,
      &server_max_send_size, &send_speed },
    { "client", "server", "connect to server ok !", 0, client_cnnt_ip, &client_cnnt_port,
      &client_max_send_size, &send_speed },
};
#define TEST_PEER_SERVER  (&g_peers[0])
#define TEST_PEER_CLIENT  (&g_peers[1])

// ---------------------------------------------------------------- 校验相关的工具

static uint32_t g_crc_table[256];
// slice-by-8 的另 7 张表：一次吃 8 字节（原来 1 字节查一次表 ✓ 实测占整进程 CPU 的 ~29% ✗）
static uint32_t g_crc_t8[8][256];

// 应用句柄队列：事件回调是在**协议锁里**被调用的 ⇒ 这里只入队 ✓（绝不回调 ffrdp_* ✗ 会自锁 ✓）；
// 服务端另起 link 线程取走句柄 ⇒ 一个服务端能同时服务 N 个客户端 ✓。
// ⚠️ 下面写死的 64 必须与 ffrdp.c 的 FFRDP_MAXCONN 一致 —— 那个宏不在头文件里（test.c 是独立编译单元 ✓）。
#define TEST_MAX_LINK   16     // = ffrdp.c 的 FFRDP_MAXCONN
static pthread_mutex_t g_accept_lock = PTHREAD_MUTEX_INITIALIZER;
static int  g_accept_q[TEST_MAX_LINK];
static int  g_accept_qn;

static int accept_pop(void)
{
    int h = -1;
    pthread_mutex_lock(&g_accept_lock);
    if (g_accept_qn > 0) {
        h = g_accept_q[0];
        memmove(g_accept_q, g_accept_q + 1, (size_t)(--g_accept_qn) * sizeof(g_accept_q[0]));
    }
    pthread_mutex_unlock(&g_accept_lock);
    return h;
}

static long on_event(void *cbctx, const FFRDP_EVENT *ev)
{
    (void)cbctx;
    if (ev->type == FFRDP_ACCEPT) {
        pthread_mutex_lock(&g_accept_lock);
        if (g_accept_qn < TEST_MAX_LINK) g_accept_q[g_accept_qn++] = ev->conn;
        pthread_mutex_unlock(&g_accept_lock);
        printf("accept ok: handle %d\n", ev->conn); fflush(stdout);
    } else if (ev->type == FFRDP_DISCONN && (g_do_close || g_idle_sec)) {
        // 只在两个验收模式下打印（普通跑法一字不改 ✓）
        printf("DISCONN: handle %d\n", ev->conn); fflush(stdout);
    }
    return FFRDP_NOT_CONSUMED;
}

static void crc32_init(void) // CRC-32 (IEEE 802.3)，查表，只在 main 里调用一次
{
    uint32_t i, j, c;
    for (i = 0; i < 256; i++) {
        for (c = i, j = 0; j < 8; j++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        g_crc_table[i] = c;
    }
    for (i = 0; i < 256; i++) g_crc_t8[0][i] = g_crc_table[i];
    for (j = 1; j < 8; j++) {
        for (i = 0; i < 256; i++) {
            uint32_t v = g_crc_t8[j - 1][i];
            g_crc_t8[j][i] = (v >> 8) ^ g_crc_table[v & 0xFF];
        }
    }
}

// 小端 32 位读，与 put32/get32 同口径：-O2 在 LE 机器上就是一条 32 位读 ✓（BE 上也不会读错 ✓）
static uint32_t crc_ld32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// slice-by-8：8 字节一拍（前 4 字节与余数异或、后 4 字节直接查表 ✓）。与逐字节版逐位等价，
// 用 /tmp/crc-check 对过标准向量 0xCBF43926 与 0..16384 全长度随机数据 ✓。
static uint32_t crc32(const uint8_t *buf, int len)
{
    uint32_t c = 0xFFFFFFFFu;
    const uint8_t *p = buf;
    while (len >= 8) {
        uint32_t a = crc_ld32(p) ^ c;
        uint32_t b = crc_ld32(p + 4);
        c = g_crc_t8[7][ a        & 0xFF] ^ g_crc_t8[6][(a >>  8) & 0xFF] ^
            g_crc_t8[5][(a >> 16) & 0xFF] ^ g_crc_t8[4][(a >> 24) & 0xFF] ^
            g_crc_t8[3][ b        & 0xFF] ^ g_crc_t8[2][(b >>  8) & 0xFF] ^
            g_crc_t8[1][(b >> 16) & 0xFF] ^ g_crc_t8[0][(b >> 24) & 0xFF];
        p += 8; len -= 8;
    }
    while (len-- > 0) c = g_crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static uint32_t xs32(uint32_t *s) // 小而快的伪随机（rand() 在回环速率下会成为瓶颈）
{
    *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5;
    return *s;
}

static void put32(uint8_t *p, uint32_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static uint32_t get32(const uint8_t *p)   { return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }

// 生成 seq 号消息到 buf，返回整条长度（≤ buf_max）；失败返回 -1
static int msg_build(uint8_t *buf, int buf_max, uint32_t seq, int max_payload, uint32_t *rnd)
{
    int paylen, total, i, oh = msg_oh();
    if (buf_max <= oh || max_payload < 1) return -1;
    if (max_payload > buf_max - oh) max_payload = buf_max - oh;
    paylen = 1 + (int)(xs32(rnd) % (uint32_t)max_payload);
    total  = oh + paylen;
    put32(buf + 0, (uint32_t)total);
    put32(buf + 4, seq);
    for (i = 0; i < paylen; i += 4) {
        uint32_t r = xs32(rnd); int left = paylen - i;
        if (left >= 4) { buf[8+i]=(uint8_t)r; buf[9+i]=(uint8_t)(r>>8); buf[10+i]=(uint8_t)(r>>16); buf[11+i]=(uint8_t)(r>>24); }
        else memcpy(buf + 8 + i, &r, (size_t)left);
    }
    put32(buf + 8 + paylen, crc32(buf, 8 + paylen));   // crc 覆盖 len+seq+payload（与改造前逐字相同 ✓）
    if (stamp_on()) put32(buf + 8 + paylen + 4, app_us());   // 占位；真正发送前由 msg_stamp() 刷新 ✓
    return total;
}

// 每次 ffrdp_send **之前**调一次（含重试）⇒ 延迟从"交给协议"算起，不含应用侧被拒的等待 ✓
static void msg_stamp(uint8_t *buf, int len)
{
    if (!stamp_on() || len < TEST_MSG_OVERHEAD + 4) return;
    put32(buf + len - 4, app_us());
}

// 校验状态（每个线程一份）
typedef struct {
    uint32_t sent_msgs, sent_bytes;   // 已确认交给协议的消息数 / 字节数
    uint32_t recv_msgs, recv_bytes;   // 已校验通过的消息数 / 字节数
    uint32_t seq_base, seq_last;      // 对端 seq 的基准与最近一个
    int      seq_have;
    uint32_t seq_gap, seq_missing;    // 序号缺口次数 / 累计缺失条数
    uint32_t crc_err, len_err, partial_err;
    uint32_t log_seq, log_crc, log_len, log_part; // 每类错误的打印计数（限流用）
    // 应用在闸门上等了多久（只读 ✓）：从第一次被拒到最后被接受的那段 = 应用真实付出的代价 ✓
    //（重试的节拍由那次带超时的 recv 提供 ✓）。
    uint32_t send_retries;      // 被拒的总次数（同一条消息可能被拒多次 ✓）
    uint32_t send_wait_msgs;    // 等过（被拒 >= 1 次）的消息条数 ✓
    uint32_t send_wait_ms;      // 等待时长之和（ms ✓）
    uint32_t send_wait_max_ms;  // 单条最长等待（ms ✓）
    // ── A1：逐消息延迟（TEST_STAMP=1 时才有样本 ✓）──────────────────────────────
    uint32_t lat_n;             // 有效样本数（= 校验通过且时间戳合理 ✓）
    uint32_t lat_bad;           // 时间戳明显不合理 ⇒ 多半是**对端没开** TEST_STAMP ✓（要显式报，别静默 ✗）
    uint32_t lat_over;          // > TEST_LAT_MAX_MS 的样本
    uint32_t lat_max_us;
    uint64_t lat_sum_us;
    uint32_t lat_hist[TEST_LAT_BUCKETS];   // 100 µs 一档（AV 判据在 ms 尺度，但 lo 上要看亚毫秒 ✓）
} TEST_VERIFY;

static void verify_log(const char *name, const char *peer, const char *what,
                       uint32_t a, uint32_t b, uint32_t *slot)
{
    if (*slot < 3) {
        printf("[%s] VERIFY %s: %s (a=%u b=%u)\n", name, peer, what, a, b);
    } else if (*slot == 3) {
        printf("[%s] VERIFY %s: %s ... more than 3 times; further ones of the same kind are not printed one by one (see the exit summary)\n", name, peer, what);
    }
    (*slot)++;
}

// 从流里尽量多地解析出完整消息，返回消耗掉的字节数
static int msg_parse(TEST_VERIFY *v, const char *name, const char *peer, const uint8_t *s, int n)
{
    int off = 0, oh = msg_oh(), st = stamp_on() ? 4 : 0;
    while (n - off >= oh) {
        uint32_t len = get32(s + off), seq = get32(s + off + 4);
        if (len < (uint32_t)oh || len > TEST_STREAM_MAX) {
            // 长度不合法 ⇒ 流已经失步，无法可靠恢复，报一次并停止解析（正常时不该出现）
            verify_log(name, peer, "bad length, stream lost sync", len, (uint32_t)TEST_STREAM_MAX, &v->log_len);
            v->len_err++;
            break;
        }
        if ((uint32_t)(n - off) < len) break;      // 这一条还没收齐，等下一轮
        // crc 的覆盖范围与改造前**逐字相同**（len-4-st 就是"len+seq+payload"，时间戳不在里面 ✓）
        if (crc32(s + off, (int)len - 4 - st) != get32(s + off + len - 4 - st)) {
            // 长度可信 ⇒ 边界还是对齐的，跳过这一条继续（不放弃整条流）
            verify_log(name, peer, "crc mismatch", seq, len, &v->log_crc);
            v->crc_err++;
            off += (int)len;
            continue;
        }
        if (!v->seq_have) { v->seq_have = 1; v->seq_base = v->seq_last = seq; }
        else if (seq != v->seq_last + 1) {
            uint32_t missing = seq - v->seq_last - 1;   // 中间缺了几条（回绕安全）
            v->seq_gap++; v->seq_missing += missing;
            verify_log(name, peer, "seq gap", v->seq_last, seq, &v->log_seq);
            v->seq_last = seq;
        } else v->seq_last = seq;
        if (st) {   // A1：这一条从"交给协议"到"校验通过"用了多久 ✓
            uint32_t d = app_us() - get32(s + off + len - 4);   // 32 位 µs 回绕 ⇒ 减法仍然对（延迟远小于 71 分钟 ✓）
            // 对端没开 TEST_STAMP 时，这 4 字节是载荷 ⇒ 读出来是垃圾（一般极大）⇒ 显式记一笔，
            // 不留"看起来正常的假样本" ✗。
            if (d > 60000000u) v->lat_bad++;
            else {
                uint32_t b = d / TEST_LAT_UNIT_US;
                v->lat_n++; v->lat_sum_us += d;
                if (d > v->lat_max_us) v->lat_max_us = d;
                if (b >= TEST_LAT_BUCKETS) v->lat_over++; else v->lat_hist[b]++;
            }
        }
        v->recv_msgs++; v->recv_bytes += len;
        off += (int)len;
    }
    return off;
}

// 分位数：按累加计数找"第 pct% 个样本落在哪个毫秒桶" ✓（向上取整，样本少时不会返回空桶 ✓）
static uint32_t lat_pct(const TEST_VERIFY *v, uint32_t pct)
{
    uint32_t total = v->lat_n, want, acc = 0, i;
    if (!total) return 0;
    want = (uint32_t)(((uint64_t)total * pct + 99) / 100);
    if (!want) want = 1;
    for (i = 0; i < TEST_LAT_BUCKETS; i++) { acc += v->lat_hist[i]; if (acc >= want) return i; }
    return TEST_LAT_BUCKETS;                     // 落进 over 桶 ✓
}

// 退出时打一次（不动判据：量具先只给人看 ✓；等真机基线出来再决定要不要立判据）
static void lat_print(const TEST_VERIFY *v, const char *name)
{
    if (!stamp_on()) return;
    if (!v->lat_n) {
        printf("[%s] latency: no samples (%s)\n", name,
               v->lat_bad ? "timestamp looked wrong on every message -> did the PEER also set TEST_STAMP=1 ?"
                          : "no message was verified");
        return;
    }
    printf("[%s] latency ms: n=%u p50=%.1f p95=%.1f p99=%.1f max=%.1f mean=%.2f | >%dms %u, bad_stamp %u%s\n",
           name, v->lat_n,
           lat_pct(v, 50) * (double)TEST_LAT_UNIT_US / 1000.0,
           lat_pct(v, 95) * (double)TEST_LAT_UNIT_US / 1000.0,
           lat_pct(v, 99) * (double)TEST_LAT_UNIT_US / 1000.0,
           v->lat_max_us / 1000.0, (double)v->lat_sum_us / (double)v->lat_n / 1000.0,
           TEST_LAT_MAX_MS, v->lat_over, v->lat_bad,
           v->lat_bad ? "  <- peer is probably missing TEST_STAMP=1 (both sides must set it)" : "");
}

// 对端是否已失联（两侧同一判据：协议自己这么判，或我们已经很久没收到它的任何数据 ✓）。
static int peer_is_lost(void *ffrdp, int handle, uint32_t tick_recv)
{
    if (ffrdp_isdead(ffrdp, handle)) return 1;       // 按句柄问（v2：一个上下文 → N 条连接 ✓）
    return (int32_t)get_timestamp32_ms() - (int32_t)tick_recv > TEST_PEER_SILENCE_MS;
}

// 一条连接上的应用循环：ffrdp/handle/peer 由调用方给（服务端一个上下文要同时跑 N 条 ✓）；
// 返回 1 = 这条断了（客户端据此重建 ✓）、返回 0 = 收到 quit；owns_ctx=1 的那条负责 ffrdp_exit ✓。
struct link_arg {
    void           *ffrdp;
    int             handle;
    const TEST_PEER *peer;
    int             is_server;
    int             owns_ctx;
};

static int link_loop(struct link_arg *a)
{
    const TEST_PEER *peer = a->peer;
    const int      handle = a->handle;
    void          *ffrdp  = a->ffrdp;
    const uint32_t quota  = (uint32_t)*peer->send_speed * 1024; // 每秒允许发送的字节数，0 = 不限速
    int      send_max = *peer->send_size;
    uint8_t *sendbuf = NULL, *stream = NULL;
    uint32_t tick_dump, tick_recv = 0, tick_speed, tokens, rnd = 0x12345678u, seq = 1;
    uint32_t tick_msg_first = 0, msg_retries = 0;   // 只读（§26.30 ①-①）：这一条消息的首次尝试时刻 / 被拒次数 ✓
    int      ret, connected = 0, stream_len = 0, msg_len = 0, max_payload, hb = 0, lost = 0;
    // ①-23：应用循环的账（只读 ✓）—— dump 里全是协议侧 ⇒ "谁在等"在应用这一格本来是空的 ✗。
    uint32_t loop_iters = 0, t_loop0 = 0, t_loop1 = 0, t_w;
    uint32_t rd_n = 0, rd_us = 0, re_n = 0, re_us = 0, rf_n = 0, rf_us = 0;
    TEST_VERIFY v;

    memset(&v, 0, sizeof(v));
    const int oh = msg_oh();                                        // A1：TEST_STAMP=1 时 16，否则 12 ✓
    if (send_max < oh + 1) send_max = oh + 1;                       // 至少装得下一条最小消息
    sendbuf = malloc((size_t)send_max);
    stream  = malloc(TEST_STREAM_MAX);
    if (!sendbuf || !stream) {
        printf("%s failed to allocate send or recv buffer !\n", peer->name);
        goto done;
    }

    tick_dump  = get_timestamp32_ms();
    tick_recv  = tick_dump;   // 还没收到过数据 ⇒ 从这一刻起算
    tick_speed = tick_dump;
    tokens     = quota;   // 一开跑就有一个周期的额度，否则头 1 秒发不出去、平均速率会偏低
    while (!g_exit) {
        // 发送限速（令牌桶）：每 TEST_SPEED_INTERVAL_MS 把令牌补满一个周期的额度，
        loop_iters++;                       // ①-23（只读 ✓）
        if (t_loop0 == 0) t_loop0 = app_us();
        t_loop1 = app_us();
        // 桶的上限就是这一个周期的量（不跨周期累积 ⇒ 不会攒出突发）。
        if (quota) {
            if ((int32_t)get_timestamp32_ms() - (int32_t)tick_speed >= TEST_SPEED_INTERVAL_MS) {
                tick_speed += TEST_SPEED_INTERVAL_MS;
                tokens      = quota;
            }
        }

        // ①-22：这个开关要放在"造消息"**之前** ✓（原来放后面 ⇒ 进循环体时 msg_len 已 >0 ⇒ 一次都不跑 ✗，实测踩过 ✓）。
        if (!g_idle_sec && g_app_burst > 1) {
            int k;
            for (k = 1; k < g_app_burst && !g_exit && msg_len == 0; k++) {
                int room = 1, maxp = send_max - oh;
                if (quota) {
                    if (tokens <= (uint32_t)oh) room = 0;
                    else if (tokens - (uint32_t)oh < (uint32_t)maxp) maxp = (int)(tokens - (uint32_t)oh);
                }
                if (room) msg_len = msg_build(sendbuf, send_max, seq, maxp, &rnd);
                if (msg_len <= 0) { msg_len = 0; break; }
                tick_msg_first = get_timestamp32_ms(); msg_retries = 0;
                msg_stamp(sendbuf, msg_len);                    // A1：延迟从"交给协议"算起 ✓
                ret = ffrdp_send(ffrdp, handle, (char*)sendbuf, msg_len, app_sendwait_get());
                {   int t = app_sendretry_get();   // ①-41 实验：被拒立刻重试（0 = 关 ✓）
                    while (ret < 0 && t-- > 0) ret = ffrdp_send(ffrdp, handle, (char*)sendbuf, msg_len, app_sendwait_get());
                }
                if (ret == msg_len) {
                    v.sent_msgs++; v.sent_bytes += (uint32_t)msg_len;
                    if (quota) tokens -= (uint32_t)msg_len;
                    seq++; msg_len = 0;
                } else if (ret > 0) {          // 不该发生（send 是原子的 ✓）：丢掉这条并留下告警
                    v.partial_err++; msg_len = 0; break;
                } else {                       // 被拒 ⇒ 退出突发 ✓（应用不再空转 ✓）
                    v.send_retries++; msg_retries++; break;
                }
            }
        }
        if (!g_idle_sec && msg_len == 0) {   // 上一条已经发出去了，生成下一条（失败重试时必须原样重发，不能重新生成）
            // FFRDP_IDLE>0 ⇒ 压根不生成 ✓：应用层一个字节都不发，专门看协议自己会不会把连接判死 ✓
            int room = 1;
            max_payload = send_max - oh;
            if (quota) {
                if (tokens <= (uint32_t)oh) room = 0; // 本周期额度装不下最小一条 ⇒ 这轮不发
                else if (tokens - (uint32_t)oh < (uint32_t)max_payload) max_payload = (int)(tokens - (uint32_t)oh);
            }
            if (room) msg_len = msg_build(sendbuf, send_max, seq, max_payload, &rnd);
            if (msg_len < 0) msg_len = 0;   // 不该发生；当作"这轮不发"，下一轮重来（绝不能留成负数）
            if (msg_len > 0) { tick_msg_first = get_timestamp32_ms(); msg_retries = 0; }   // 只读（§26.30 ①-①）：新一条的起点 ✓
        }

        if (msg_len > 0) {
            // 传 0（不等待）：这个测试的背压方式是"被拒就下一轮原样重试"，节拍由那次带超时的 recv 提供 ✓。
            msg_stamp(sendbuf, msg_len);                    // A1：每次尝试前刷新 ⇒ 不含"被拒后干等"的那段 ✓
            ret = ffrdp_send(ffrdp, handle, (char*)sendbuf, msg_len, app_sendwait_get());
            {   int t = app_sendretry_get();   // ①-41 实验：被拒立刻重试（0 = 关 ✓）
                while (ret < 0 && t-- > 0) ret = ffrdp_send(ffrdp, handle, (char*)sendbuf, msg_len, app_sendwait_get());
            }
            if (ret == msg_len) {                       // 整条被接受
                // 只读（§26.30 ①-①）：这条被拒过 ⇒ 把"从第一次尝试到被接受"的时长记下来 ✓
                if (msg_retries) {
                    uint32_t d = get_timestamp32_ms() - tick_msg_first;
                    v.send_wait_msgs++; v.send_wait_ms += d;
                    if (d > v.send_wait_max_ms) v.send_wait_max_ms = d;
                    msg_retries = 0;
                }
                v.sent_msgs++; v.sent_bytes += (uint32_t)msg_len;
                if (quota) tokens -= (uint32_t)msg_len;
                seq++; msg_len = 0;
            } else if (ret > 0) {                       // 0 < ret < len：send 是原子的，这不该发生
                verify_log(peer->name, peer->peer, "partial send (ffrdp_send is atomic, this cannot happen)",
                           (uint32_t)ret, (uint32_t)msg_len, &v.log_part);
                v.partial_err++;
                msg_len = 0;                            // 没法安全重发，丢掉这一条（并留下告警）
            }
            // ret == -1 / -2：队列满或连接已断 ⇒ 什么都不动，下一轮原样重试这一条
            else { v.send_retries++; msg_retries++; }   // 只读（§26.30 ①-①）：这一轮被拒了 ✓
        }

        if (stream_len < TEST_STREAM_MAX) {
            // 应用侧唯一的等待点：一有数据立刻返回，没有数据最多等 TEST_RECV_WAIT_MS。
            t_w = app_us();                 // ①-23：应用侧唯一的等待点 ✓
            ret = ffrdp_recv(ffrdp, handle, (char*)stream + stream_len, TEST_STREAM_MAX - stream_len, g_recv_wait_ms);   // ①-32b
            if (ret <= 0 && g_spin_us > 0) usleep((useconds_t)g_spin_us);   // ①-32b：没数据就短睡，别空转 ✓
            {   uint32_t d = app_us() - t_w;   // ①-23："等到数据"与"空转到超时"分开记 ✓
                if (ret > 0) { rd_n++; rd_us += d; } else { re_n++; re_us += d; }
            }
            if (ret > 0) {
                stream_len += ret;
                if (connected == 0) {
                    connected = 1;
                    printf("%s\n", peer->msg_up);
                }
                tick_recv = get_timestamp32_ms();
                if ((int32_t)get_timestamp32_ms() - (int32_t)tick_dump > TEST_DUMP_INTERVAL_MS) {
                    pthread_mutex_lock(&g_mutex);
                    ffrdp_dump(ffrdp, -1, 0)   /* ①-8：打全表 —— 服务端 0 号内嵌槽的计数原来完全不可见 ✗ */;
                    pthread_mutex_unlock(&g_mutex);
                    tick_dump = get_timestamp32_ms();
                }
            }
        } else {
            t_w = app_us();                 // ①-23：流满那条路（注释说"正常情况下到不了"✓，量一下 ✓）
            usleep(10 * 1000);   // 流满了 ⇒ 这一轮没地方收；睡一下别空转（正常情况下到不了这里）
            rf_n++; rf_us += app_us() - t_w;
        }
        if (stream_len > 0) {   // 每轮都试着解析（不足一条时这个循环体不会进入）
            int used = msg_parse(&v, peer->name, peer->peer, stream, stream_len);
            if (used > 0) {
                memmove(stream, stream + used, (size_t)(stream_len - used));
                stream_len -= used;
            }
        }

        // 这里原来有一句 ffrdp_update()：协议现在由 lowlevel 的工作线程驱动 ✓，应用不需要它了。
        t_loop1 = app_us();                 // ①-23（含 msg_parse ✓）
        if (g_idle_sec) {
            uint32_t nowi = get_timestamp32_ms();
            if ((int32_t)nowi - (int32_t)tick_dump > 1000) {
                int      dead   = ffrdp_isdead(ffrdp, handle);
                uint32_t silent = nowi - tick_recv;
                printf("[%s] idle: conn=%d isdead=%d app_silent=%ums%s\n",
                       peer->name, handle, dead, (unsigned)silent,
                       (dead == 0 && (int32_t)silent > TEST_PEER_SILENCE_MS)
                           ? "  <- the application-level silence would have been a false alarm; the protocol side did not declare it dead" : "");
                fflush(stdout);
                tick_dump = nowi;
                if (++hb % 5 == 0) {   // 每 5s 打一次协议状态（连接数 / 握手状态 / 计数器）
                    pthread_mutex_lock(&g_mutex); ffrdp_dump(ffrdp, -1, 0)   /* ①-8：打全表 —— 服务端 0 号内嵌槽的计数原来完全不可见 ✗ */; pthread_mutex_unlock(&g_mutex);
                }
            }
        } else if (connected && peer_is_lost(ffrdp, handle, tick_recv)) {
            printf("%s lost ! (handle %d)\n", peer->peer, handle);
            if (ffrdp) { printf("%s --- dump ---\n", peer->name); fflush(stdout); ffrdp_dump(ffrdp, -1, 0)   /* ①-8：打全表 —— 服务端 0 号内嵌槽的计数原来完全不可见 ✗ */; }
            lost = 1;
            break;                        // 这条连接完了；要不要重建由调用方决定 ✓
        }
    }

done:
    printf("[%s] verify: sent %u msgs / %u B, recv %u msgs / %u B | seq_gap %u (missing %u), crc_err %u, len_err %u, partial_err %u\n",
           peer->name, v.sent_msgs, v.sent_bytes, v.recv_msgs, v.recv_bytes,
           v.seq_gap, v.seq_missing, v.crc_err, v.len_err, v.partial_err);
    // §26.30 ①-①：应用侧在队列闸门上的等待（累计值 ✓；配合 ffrdp 那块的 refuse 分档看"谁挡的"✓）
    printf("[%s] app send gate: refused %u times, waited on %u msgs, %u ms total, %u ms max (retry = next loop, ~%dms)\n",
           peer->name, v.send_retries, v.send_wait_msgs, v.send_wait_ms, v.send_wait_max_ms, TEST_RECV_WAIT_MS);
    if (t_loop0) {   // ①-23：应用循环的账 —— "谁在等"的应用那一格 ✓
        uint32_t tot = t_loop1 - t_loop0;
        printf("[%s] app loop ①-23: %u iters over %u ms | recv-with-data %u (%u ms), recv-EMPTY %u (%u ms), stream-full %u (%u ms) | in recv %u%% of the loop, avg %u us/iter\n",
               peer->name, loop_iters, tot / 1000, rd_n, rd_us / 1000, re_n, re_us / 1000, rf_n, rf_us / 1000,
               tot ? (unsigned)((uint64_t)(rd_us + re_us + rf_us) * 100 / tot) : 0, loop_iters ? tot / loop_iters : 0);
    }
    lat_print(&v, peer->name);   // A1：TEST_STAMP=1 才打（默认关 ⇒ 与改造前逐字相同 ✓）
    if (v.crc_err || v.len_err || v.partial_err || v.seq_gap) {
        printf("[%s] verify: *** CHECK FAILED ***\n", peer->name);
    } else if (v.recv_msgs) {
        printf("[%s] verify: OK -- received %u messages, seq contiguous and crc all correct\n", peer->name, v.recv_msgs);
    }
    // ①-16：**退出时**也打一次全表 —— 对端走了之后就没有任何数据 ✗ ⇒ 收尾那一段在协议账上原本不存在（"账被输出时点挡住" ✓）；
    // 放在 verify 之后、close 之前 ⇒ 走 close 的场景也拿得到 ✓。
    if (ffrdp) {
        printf("[%s] --- exit dump (protocol counters; includes the teardown tail) ---\n", peer->name);
        fflush(stdout);
        pthread_mutex_lock(&g_mutex); ffrdp_dump(ffrdp, -1, 0); pthread_mutex_unlock(&g_mutex);
    }
    if (g_do_close && ffrdp) {
        // 主动关闭（验四次挥手）：ffrdp_close ⇒ 我方 FIN ⇒ 对端 FIN ⇒ 各自 DISCONN ✓；⚠️ hs:0 下没有 FIN（只做本地拆除 ✓）。
        int r = ffrdp_close(ffrdp, handle);
        int t0 = (int)get_timestamp32_ms();
        char tmp[512];
        printf("[%s] close(conn=%d) -> %d, waiting for the peer's FIN (up to 2000ms) ...\n", peer->name, handle, r);
        fflush(stdout);
        while (r == 0 && (int)get_timestamp32_ms() - t0 < 2000) {
            int rd = ffrdp_recv(ffrdp, handle, tmp, sizeof(tmp), 50);
            if (ffrdp_isdead(ffrdp, handle)) {
                printf("[%s] close: peer FIN received, connection closed, %d ms\n",
                       peer->name, (int)get_timestamp32_ms() - t0);
                break;
            }
            if (rd == 0) {
                printf("[%s] close: recv reported CLOSED, connection is gone\n", peer->name);
                break;
            }
        }
        if (r == 0 && !ffrdp_isdead(ffrdp, handle))
            printf("[%s] close: no peer FIN within 2s (peer already exited, or hs:0 => local teardown only)\n", peer->name);
        fflush(stdout);
    }
    free(sendbuf);
    free(stream);
    if (a->owns_ctx) ffrdp_exit(ffrdp);      // 上下文归角色线程 ⇒ 只有它退 ✓
    return lost;
}

// 服务端：一条被 accept 的连接 = 一个 link 线程 ✓（这样服务端天然能同时服务 N 个客户端 ✓）
static void* link_thread(void *param)
{
    struct link_arg *a = (struct link_arg*)param;
    link_loop(a);
    free(a);
    return NULL;
}

// 角色入口：客户端 ⇒ 建上下文 + 跑唯一那条主连接（handle 0 ✓），丢了就重建 ✓；
// 服务端 ⇒ 只做"取句柄、起 link 线程"（主连接不参与收发 ✓），收到 quit 时 join 全部 link 线程 ✓。
static void* peer_thread(void *param)
{
    const TEST_PEER *peer = (const TEST_PEER*)param;
    void *ffrdp = NULL;
    pthread_t links[TEST_MAX_LINK];
    int nlink = 0, i;

    while (!g_exit) {
        if (!ffrdp) {
            char addr[80];
            snprintf(addr, sizeof(addr), "%s:%d", peer->ip, *peer->port);
            {
                // v2 接口：角色进 scheme、smss/sfec 进配置串 ✓；FFRDP_HS（A/B 用）0 ⇒ 老路径、1 ⇒ 强制握手、不设 ⇒ 跟随默认 ✓。
                char params[160];
                const char *hs = getenv("FFRDP_HS");
                snprintf(params, sizeof(params), "%s://%s,mss:1280,fec:0%s",
                         peer->is_server ? "ffrdps" : "ffrdpc", addr,
                         (hs && *hs) ? (atoi(hs) ? ",hs:1" : ",hs:0") : "");
                printf("%s ffrdp_init: %s\n", peer->name, params);   // 排查用：实际配置进日志 ✓
                fflush(stdout);
                ffrdp = ffrdp_init(params, on_event, NULL);
            }
            if (!ffrdp) { usleep(100 * 1000); continue; }
        }

        if (!peer->is_server) {
            // 客户端：只有主连接（handle 0 ✓）⇒ 直接在本线程里跑 ✓
            struct link_arg a;
            a.ffrdp = ffrdp; a.handle = 0; a.peer = peer; a.is_server = 0; a.owns_ctx = 1;
            // ⚠️ owns_ctx=1 ⇒ link_loop 自己已经 ffrdp_exit ⇒ 这里**绝不能再退一次**（再退一次会在进程退出时 SIGSEGV ✗）。
            link_loop(&a);
            ffrdp = NULL;
            if (g_exit) break;
            continue;                               // 断了 ⇒ 重建（原来的行为 ✓）
        }

        // ⚠️ hs:0（老路径）下协议**不发 ACCEPT** ⇒ 服务端的应用句柄就是 0，必须照旧直接在 0 号上收发 ✓。
        if (!g_use_hs) {
            struct link_arg a;
            a.ffrdp = ffrdp; a.handle = 0; a.peer = peer; a.is_server = 1; a.owns_ctx = 1;
            link_loop(&a);                          // 同上：exit 由它负责 ✓
            ffrdp = NULL;
            if (g_exit) break;
            continue;                               // 断了 ⇒ 重建（老路径的老行为 ✓）
        }

        // 服务端：取一个 accept 句柄 ⇒ 起一条 link 线程 ✓
        {   int h = accept_pop();
            if (h < 0) { usleep(5 * 1000); continue; }
            if (nlink < TEST_MAX_LINK) {
                struct link_arg *a = (struct link_arg*)malloc(sizeof(*a));
                if (a) {
                    a->ffrdp = ffrdp; a->handle = h; a->peer = peer; a->is_server = 1; a->owns_ctx = 0;
                    if (pthread_create(&links[nlink], NULL, link_thread, a) == 0) nlink++;
                    else free(a);
                }
            } else {
                printf("%s: too many links, handle %d dropped\n", peer->name, h); fflush(stdout);
            }
        }
    }

    for (i = 0; i < nlink; i++) pthread_join(links[i], NULL);
    if (ffrdp) {
        // ①-16b：上下文拆掉**前一刻**再打一次 —— link_loop 退掉后就再没有 dump 触发 ✗，而低层还在转 ⇒ 账打到这里才闭合 ✓。
        printf("[%s] --- final dump (right before ffrdp_exit) ---\n", peer->name);
        fflush(stdout);
        pthread_mutex_lock(&g_mutex); ffrdp_dump(ffrdp, -1, 0); pthread_mutex_unlock(&g_mutex);
        ffrdp_exit(ffrdp);             // 只有"上下文还开着"的那条路才需要退 ✓
    }
    return NULL;
}

int main(int argc, char *argv[])
{
    int server_en = 0, client_en = 0, i;
    pthread_t hserver = 0, hclient = 0;
    char *str;

    if (argc <= 1) {
        const char *prog = argv[0] ? argv[0] : "test_ffrdp"; // 按调用名打印：重命名、软链、绝对路径都对得上
        printf("ffrdp test program - v%s\n", TEST_VERSION);
        printf("usage: %s [--server[=ip:port]] [--client[=ip:port]]\n", prog);
        printf("%*s[--server_max_send_size=N] [--client_max_send_size=N] [--send_speed=N]\n",
               (int)strlen(prog) + 7, ""); // 7 = strlen("usage: ")，让接续行与上面那个括号对齐
        printf("\n");
        printf("  --server[=ip:port]            run as server    (default 0.0.0.0:8000)\n");
        printf("  --client[=ip:port]            run as client    (default 127.0.0.1:8000)\n");
        printf("  --server_max_send_size=N      server max bytes per message (default 16384)\n");
        printf("  --client_max_send_size=N      client max bytes per message (default 16384)\n");
        printf("  --send_speed=N                limit this process to N KB/s (default 0 = unlimited)\n");
        printf("\n");
        printf("  every message is [len][seq][payload][crc32] and is checked at the receiver:\n");
        printf("  seq continuity + crc + no partial send; a summary is printed at exit.\n");
        printf("  the two roles may be given together (self test on one host) - the send rate\n");
        printf("  limit then applies to both; type 'quit' or 'exit' to stop: only that way prints\n");
        printf("  the verify summary (Ctrl+C ends the process without it); a diagnostic dump is\n");
        printf("  printed every 10 s.\n");
        printf("\n");
        return 0;
    }

    for (i=1; i<argc; i++) {
        if (strstr(argv[i], "--server=") == argv[i]) {
            server_en = 1;
            if (strcmp(argv[i] + 9, "") != 0) {
                // snprintf 而不是 strncpy：源串可能比目标长 ⇒ strncpy 会填满 32 字节而不留结尾的 0（-O2 下 -Wstringop-truncation 拦下 ✓）。
                snprintf(server_bind_ip, sizeof(server_bind_ip), "%s", argv[i] + 9);
            }
        } else if (strstr(argv[i], "--client=") == argv[i]) {
            client_en = 1;
            if (strcmp(argv[i] + 9, "") != 0) {
                snprintf(client_cnnt_ip, sizeof(client_cnnt_ip), "%s", argv[i] + 9);
            }
        } else if (strcmp(argv[i], "--server") == 0) {
            server_en = 1;
        } else if (strcmp(argv[i], "--client") == 0) {
            client_en = 1;
        } else if (strstr(argv[i], "--server_max_send_size=") == argv[i]) {
            server_max_send_size = atoi(argv[i] + 23);
        } else if (strstr(argv[i], "--client_max_send_size=") == argv[i]) {
            client_max_send_size = atoi(argv[i] + 23);
        } else if (strstr(argv[i], "--send_speed=") == argv[i]) {
            send_speed = atoi(argv[i] + 13);
        }
    }

    strtok_r(server_bind_ip, ":", &str); if (str && *str) server_bind_port = atoi(str);
    strtok_r(client_cnnt_ip, ":", &str); if (str && *str) client_cnnt_port = atoi(str);
    {   // 两个验收开关（都不设 ⇒ 与以前完全一样 ✓）
        const char *si = getenv("FFRDP_IDLE");
        const char *sc = getenv("FFRDP_CLOSE");
        if (si && *si) g_idle_sec = atoi(si);
        {   const char *sh = getenv("FFRDP_HS");
            if (sh && *sh) g_use_hs = atoi(sh) ? 1 : 0;   // 与 ffrdp_init 拼串时的判断保持同一口径 ✓
        }
        if (sc && *sc) g_do_close = atoi(sc) ? 1 : 0;
        {   const char *rw = getenv("TEST_APP_RECVWAIT_MS");   // ①-32b
            const char *su = getenv("TEST_APP_SPIN_US");
            if (rw && *rw) { g_recv_wait_ms = atoi(rw); if (g_recv_wait_ms < 0) g_recv_wait_ms = 0; }
            if (su && *su) { g_spin_us = atoi(su); if (g_spin_us < 0) g_spin_us = 0; }
            if (g_recv_wait_ms != TEST_RECV_WAIT_MS || g_spin_us)
                printf("app recv wait       : %d ms, spin %d us when idle (diagnostic; default %d ms / 0)\n",
                       g_recv_wait_ms, g_spin_us, TEST_RECV_WAIT_MS);
        }
        {   const char *sb = getenv("TEST_APP_BURST");   // ①-22：应用侧突发上限（0/不设 = 旧行为 ✓）
            if (sb && *sb) { g_app_burst = atoi(sb); if (g_app_burst < 0) g_app_burst = 0; }
            if (g_app_burst > 1) printf("app burst           : %d messages per recv tick (diagnostic; default 1 = old behaviour)\n", g_app_burst);
        }
        if (g_idle_sec > 0) printf("idle mode           : on (no application data; only checks that keepalive does not misfire)\n");
        if (g_do_close)     printf("close on exit       : on (calls ffrdp_close before exit; checks the four-way handshake)\n");
    }
    printf("send_speed          : %d KB/s%s\n", send_speed, send_speed ? "" : " (unlimited)");
    if (server_en) {
        printf("server bind ip      : %s\n", server_bind_ip      );
        printf("server bind port    : %d\n", server_bind_port    );
        printf("server_max_send_size: %d\n", server_max_send_size);
    }
    if (client_en) {
        printf("client connect ip   : %s\n", client_cnnt_ip      );
        printf("client connect port : %d\n", client_cnnt_port    );
        printf("client_max_send_size: %d\n", client_max_send_size);
    }

    crc32_init();
    pthread_mutex_init(&g_mutex, NULL);
    if (server_en) pthread_create(&hserver, NULL, peer_thread, (void*)TEST_PEER_SERVER);
    if (client_en) pthread_create(&hclient, NULL, peer_thread, (void*)TEST_PEER_CLIENT);

    // 退出只认 quit / exit，或 stdin 结束（Ctrl+D、管道关闭）—— 后者在脚本里跑很有用。
    while (!g_exit) {
        char cmd[256];
        // %255s：宽度必须比缓冲区小一个字节 ⇒ 否则 256 字符的输入会把那个 NUL 写到栈上（ASAN 就是这么报的 ✓）。
        if (scanf("%255s", cmd) != 1) break;
        if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) g_exit = 1;
    }

    if (hserver) pthread_join(hserver, NULL);
    if (hclient) pthread_join(hclient, NULL);
    pthread_mutex_destroy(&g_mutex);
    return 0;
}
