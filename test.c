#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include "ffrdp.h"
#include "utils.h"

#define TEST_VERSION            "1.1.0" // 与 build.sh 生成的 libffrdp.pc 里的 Version 保持一致
#define TEST_DUMP_INTERVAL_MS   10000   // 每隔这么久打印一次诊断
#define TEST_PEER_SILENCE_MS    2000    // 这么久没收到对端数据就认为它失联（见 peer_is_lost）
#define TEST_SPEED_INTERVAL_MS  1000    // 发送限速：每这么久补一次令牌（见 peer_thread）
#define TEST_STREAM_MAX        (256 * 1024) // 接收侧累积缓冲：ffrdp_recv 是字节流，消息要自己拼回来
#define TEST_RECV_WAIT_MS       10      // 没有数据时 ffrdp_recv 等这么久：应用侧的节拍点

// 每条消息的格式（全部小端）：
//   [4B 总长 len][4B seq][payload...][4B crc32]      len = 整条消息字节数（含头尾）
//   crc32 覆盖 len + seq + payload（不含 crc 自身）
// ffrdp_recv 是"有序可靠字节流"（没有消息边界），所以必须自带定长头才能解析。
// ffrdp_send 是原子的（要么整条接受、要么返回 -1），所以 seq 只在 ret == len 时才 +1。
#define TEST_MSG_OVERHEAD        12

// 退出只有一条路：输入 quit / exit（或 stdin 结束）。两个对等端线程看到它之后收尾，
// 打印 verify 汇总并 ffrdp_free。Ctrl+C 不做任何特殊处理 —— 进程会直接结束（看不到汇总），
// 这是接受的代价：测试程序不值得为信号路径维护额外逻辑（见 docs/notes.md 5.21）。
static volatile int g_exit = 0;
static char server_bind_ip[32]   = "0.0.0.0";
static char client_cnnt_ip[32]   = "127.0.0.1";
static int  server_bind_port     = 8000;
static int  client_cnnt_port     = 8000;
static int  server_max_send_size = 16 * 1024;
static int  client_max_send_size = 16 * 1024;
static int  send_speed           = 0;   // 本进程的发送限速，单位 KB/s（1024 B）；0 = 不限速
static pthread_mutex_t g_mutex;

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

// 应用句柄与事件回调（v2）：持着协议锁被调用 ⇒ 只记句柄，绝不回调 ffrdp_* ✓
static int g_app_conn;
static long on_event(void *cbctx, const FFRDP_EVENT *ev)
{
    (void)cbctx;
    if (ev->type == FFRDP_ACCEPT) {
        g_app_conn = ev->conn;
        printf("accept ok: handle %d\n", ev->conn); fflush(stdout);
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
}

static uint32_t crc32(const uint8_t *buf, int len)
{
    uint32_t c = 0xFFFFFFFFu;
    int i;
    for (i = 0; i < len; i++) c = g_crc_table[(c ^ buf[i]) & 0xFF] ^ (c >> 8);
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
    int paylen, total, i;
    if (buf_max <= TEST_MSG_OVERHEAD || max_payload < 1) return -1;
    if (max_payload > buf_max - TEST_MSG_OVERHEAD) max_payload = buf_max - TEST_MSG_OVERHEAD;
    paylen = 1 + (int)(xs32(rnd) % (uint32_t)max_payload);
    total  = TEST_MSG_OVERHEAD + paylen;
    put32(buf + 0, (uint32_t)total);
    put32(buf + 4, seq);
    for (i = 0; i < paylen; i += 4) {
        uint32_t r = xs32(rnd); int left = paylen - i;
        if (left >= 4) { buf[8+i]=(uint8_t)r; buf[9+i]=(uint8_t)(r>>8); buf[10+i]=(uint8_t)(r>>16); buf[11+i]=(uint8_t)(r>>24); }
        else memcpy(buf + 8 + i, &r, (size_t)left);
    }
    put32(buf + 8 + paylen, crc32(buf, 8 + paylen));
    return total;
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
} TEST_VERIFY;

static void verify_log(const char *name, const char *peer, const char *what,
                       uint32_t a, uint32_t b, uint32_t *slot)
{
    if (*slot < 3) {
        printf("[%s] VERIFY %s: %s (a=%u b=%u)\n", name, peer, what, a, b);
    } else if (*slot == 3) {
        printf("[%s] VERIFY %s: %s ... > 3 次，后续同类不再逐条打印（见退出汇总）\n", name, peer, what);
    }
    (*slot)++;
}

// 从流里尽量多地解析出完整消息，返回消耗掉的字节数
static int msg_parse(TEST_VERIFY *v, const char *name, const char *peer, const uint8_t *s, int n)
{
    int off = 0;
    while (n - off >= TEST_MSG_OVERHEAD) {
        uint32_t len = get32(s + off), seq = get32(s + off + 4);
        if (len < TEST_MSG_OVERHEAD || len > TEST_STREAM_MAX) {
            // 长度不合法 ⇒ 流已经失步，无法可靠恢复，报一次并停止解析（正常时不该出现）
            verify_log(name, peer, "bad length, stream lost sync", len, (uint32_t)TEST_STREAM_MAX, &v->log_len);
            v->len_err++;
            break;
        }
        if ((uint32_t)(n - off) < len) break;      // 这一条还没收齐，等下一轮
        if (crc32(s + off, (int)len - 4) != get32(s + off + len - 4)) {
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
        v->recv_msgs++; v->recv_bytes += len;
        off += (int)len;
    }
    return off;
}

// 对端是否已失联：协议自己这么判定，或者我们已经很久没收到它的任何数据。
// 两侧用同一个判据（历史上只有客户端有"看多久没收到数据"这一半）。
static int peer_is_lost(void *ffrdp, uint32_t tick_recv)
{
    if (ffrdp_isdead(ffrdp, g_app_conn)) return 1;   // 老程序只有一条连接 ⇒ 句柄 0（= 主连接 ✓）
    return (int32_t)get_timestamp32_ms() - (int32_t)tick_recv > TEST_PEER_SILENCE_MS;
}

// 服务端与客户端共用的主循环：不停地发随机长度的消息、收对端消息并逐条校验、定期打诊断。
static void* peer_thread(void *param)
{
    const TEST_PEER *peer = (const TEST_PEER*)param;
    const uint32_t quota = (uint32_t)*peer->send_speed * 1024; // 每秒允许发送的字节数，0 = 不限速
    int      send_max = *peer->send_size;
    uint8_t *sendbuf = NULL, *stream = NULL;
    void    *ffrdp   = NULL;
    uint32_t tick_dump, tick_recv = 0, tick_speed, tokens, rnd = 0x12345678u, seq = 1;
    int      ret, connected = 0, stream_len = 0, msg_len = 0, max_payload;
    TEST_VERIFY v;

    memset(&v, 0, sizeof(v));
    if (send_max < TEST_MSG_OVERHEAD + 1) send_max = TEST_MSG_OVERHEAD + 1; // 至少装得下一条最小消息
    sendbuf = malloc((size_t)send_max);
    stream  = malloc(TEST_STREAM_MAX);
    if (!sendbuf || !stream) {
        printf("%s failed to allocate send or recv buffer !\n", peer->name);
        goto done;
    }

    tick_dump  = get_timestamp32_ms();
    // 应用句柄：客户端就是 0 号主连接 ✓；服务端要等 FFRDP_ACCEPT 报出那条新连接的句柄 ✓
    // 才有地方收发 ✓（写死 0 的话，服务端在握手模式下收发全落空 ✗ —— 仿真里 e2e 踩过同一个坑 ✓）。
    g_app_conn = 0;
    tick_speed = tick_dump;
    tokens     = quota;   // 一开跑就有一个周期的额度，否则头 1 秒发不出去、平均速率会偏低
    while (!g_exit) {
        if (!ffrdp) {
            char addr[80];
            snprintf(addr, sizeof(addr), "%s:%d", peer->ip, *peer->port);
            {
                // v2 接口：角色进 scheme，smss/sfec 进配置串（v1 是位置参数）。
                // FFRDP_HS（真机排查/A-B 用）：0 ⇒ 强制老路径 hs:0；1 ⇒ 强制握手；
                // 不设 ⇒ 跟随协议默认（v2 起默认就是握手）。远端的 v1 二进制不认这个键 ⇒ 只影响本侧 ✓。
                char params[128];
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

        // 发送限速（令牌桶）：每 TEST_SPEED_INTERVAL_MS 把令牌补满一个周期的额度，
        // 桶的上限就是这一个周期的量（不跨周期累积 ⇒ 不会攒出突发）。
        // 有了消息 framing 之后不能再"把一包裁到剩余额度"（那会把一条消息截断），
        // 所以改成"按剩余额度**选**这一条消息的大小"：额度不够装最小一条就先不发。
        if (quota) {
            if ((int32_t)get_timestamp32_ms() - (int32_t)tick_speed >= TEST_SPEED_INTERVAL_MS) {
                tick_speed += TEST_SPEED_INTERVAL_MS;
                tokens      = quota;
            }
        }

        if (msg_len == 0) {   // 上一条已经发出去了，生成下一条（失败重试时必须原样重发，不能重新生成）
            int room = 1;
            max_payload = send_max - TEST_MSG_OVERHEAD;
            if (quota) {
                if (tokens <= TEST_MSG_OVERHEAD) room = 0; // 本周期额度装不下最小一条 ⇒ 这轮不发
                else if (tokens - TEST_MSG_OVERHEAD < (uint32_t)max_payload) max_payload = (int)(tokens - TEST_MSG_OVERHEAD);
            }
            if (room) msg_len = msg_build(sendbuf, send_max, seq, max_payload, &rnd);
            if (msg_len < 0) msg_len = 0;   // 不该发生；当作"这轮不发"，下一轮重来（绝不能留成负数）
        }

        if (msg_len > 0) {
            // 传 0（不等待）：这个测试的背压方式就是"队列满就下一轮原样重试这一条"，重试的节拍由下面
            // 那次带超时的 ffrdp_recv 提供。协议跑在自己的线程上之后，等待不再等于"让协议停一会儿"，
            // 所以这里的 0 只是这个测试自己的选择，不再是必须。
            ret = ffrdp_send(ffrdp, g_app_conn, (char*)sendbuf, msg_len, 0);
            if (ret == msg_len) {                       // 整条被接受
                v.sent_msgs++; v.sent_bytes += (uint32_t)msg_len;
                if (quota) tokens -= (uint32_t)msg_len;
                seq++; msg_len = 0;
            } else if (ret > 0) {                       // 0 < ret < len：send 是原子的，这不该发生
                verify_log(peer->name, peer->peer, "partial send (ffrdp_send is atomic, this cannot happen)",
                           (uint32_t)ret, (uint32_t)msg_len, &v.log_part);
                v.partial_err++;
                msg_len = 0;                            // 没法安全重发，丢掉这一条（并留下告警）
            }
            // ret == -1：队列满（正常背压）⇒ 什么都不动，下一轮原样重试这一条
        }
        // tokens 不够装下最小一条消息时 msg_len 会是 0 ⇒ 这一轮只收不发，协议照常由循环末尾的 update 驱动

        if (stream_len < TEST_STREAM_MAX) {
            // 这里就是应用侧唯一的等待点：一有数据立刻返回，没有数据最多等 TEST_RECV_WAIT_MS。协议由
            // lowlevel 的工作线程驱动，所以这个等待是真的在"等数据"，不再顺便决定协议的节拍 —— 这正是
            // 旧版传 >0 会掉吞吐（55 -> 13 MB/s）的原因，那时"等"同时是协议的 nap。
            ret = ffrdp_recv(ffrdp, g_app_conn, (char*)stream + stream_len, TEST_STREAM_MAX - stream_len, TEST_RECV_WAIT_MS);
            if (ret > 0) {
                stream_len += ret;
                if (connected == 0) {
                    connected = 1;
                    printf("%s\n", peer->msg_up);
                }
                tick_recv = get_timestamp32_ms();
                if ((int32_t)get_timestamp32_ms() - (int32_t)tick_dump > TEST_DUMP_INTERVAL_MS) {
                    pthread_mutex_lock(&g_mutex);
                    ffrdp_dump(ffrdp, 0, 0);
                    pthread_mutex_unlock(&g_mutex);
                    tick_dump = get_timestamp32_ms();
                }
            }
        } else {
            usleep(10 * 1000);   // 流满了 ⇒ 这一轮没地方收；睡一下别空转（正常情况下到不了这里）
        }
        if (stream_len > 0) {   // 每轮都试着解析（不足一条时这个循环体不会进入）
            int used = msg_parse(&v, peer->name, peer->peer, stream, stream_len);
            if (used > 0) {
                memmove(stream, stream + used, (size_t)(stream_len - used));
                stream_len -= used;
            }
        }

        // 这里原来有一句 ffrdp_update()：应用按自己的节奏驱动协议。协议现在由 lowlevel 的工作线程驱动
        // （收包走 RECV 回调、到期走 TIMER 回调），应用不再需要、也没有这个函数可调了。
        if (connected && peer_is_lost(ffrdp, tick_recv)) {
            printf("%s lost !\n", peer->peer);
            // 真机排查用：退出前把协议状态打一遍（连接数 / 握手状态 / 各计数器 ✓）。
            if (ffrdp) { printf("%s --- dump ---\n", peer->name); fflush(stdout); ffrdp_dump(ffrdp, -1, 0); }
            ffrdp_exit(ffrdp); ffrdp = NULL;
            connected = 0;
            stream_len = 0; v.seq_have = 0;   // 重新建连 ⇒ 对端从新的流开始，基准要重新取
        }
    }

done:
    printf("[%s] verify: sent %u msgs / %u B, recv %u msgs / %u B | seq_gap %u (missing %u), crc_err %u, len_err %u, partial_err %u\n",
           peer->name, v.sent_msgs, v.sent_bytes, v.recv_msgs, v.recv_bytes,
           v.seq_gap, v.seq_missing, v.crc_err, v.len_err, v.partial_err);
    if (v.crc_err || v.len_err || v.partial_err || v.seq_gap) {
        printf("[%s] verify: *** 校验未通过 *** ✗\n", peer->name);
    } else if (v.recv_msgs) {
        printf("[%s] verify: OK —— 收到的 %u 条消息 seq 连续、crc 全部正确 ✓\n", peer->name, v.recv_msgs);
    }
    free(sendbuf);
    free(stream);
    ffrdp_exit(ffrdp);
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
                // snprintf 而不是 strncpy：源串可能比目标长，strncpy 会把 32 字节填满而不留
                // 结尾的 0（-O2 下 gcc 的 -Wstringop-truncation 就会拦下它）。
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
        // %255s：宽度必须比缓冲区小一个字节，否则一个 256 字符的输入就会把那一个 NUL 写到栈上（ASAN 报
        // 的就是这个：WRITE of size 257）。这个 bug 与协议无关，是在这次验证里被撞出来的。
        if (scanf("%255s", cmd) != 1) break;
        if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) g_exit = 1;
    }

    if (hserver) pthread_join(hserver, NULL);
    if (hclient) pthread_join(hclient, NULL);
    pthread_mutex_destroy(&g_mutex);
    return 0;
}
