#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include "ffrdp.h"

#ifdef WIN32
#include <windows.h>
#define usleep(t) Sleep((t) / 1000)
#define get_tick_count GetTickCount
#define strtok_r strtok_s
#else
static uint32_t get_tick_count()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
#endif

#define TEST_VERSION            "1.1.0" // 与 build.sh 生成的 libffrdp.pc 里的 Version 保持一致
#define TEST_DUMP_INTERVAL_MS   10000   // 每隔这么久打印一次诊断
#define TEST_PEER_SILENCE_MS     2000   // 这么久没收到对端数据就认为它失联（见 peer_is_lost）

static int  g_exit = 0;
static char server_bind_ip[32]   = "0.0.0.0";
static char client_cnnt_ip[32]   = "127.0.0.1";
static int  server_bind_port     = 8000;
static int  client_cnnt_port     = 8000;
static int  server_max_send_size = 16 * 1024;
static int  client_max_send_size = 16 * 1024;
static pthread_mutex_t g_mutex;

// 一个"对等端"的运行参数。两侧跑的是同一个循环，差别只在下面这几项。
typedef struct {
    const char *name;      // 本端名字（打印用）
    const char *peer;      // 对端名字（打印用）
    const char *msg_up;    // 第一次收到对端数据时打印的那句
    int         is_server; // 传给 ffrdp_init：1 = 服务端，0 = 客户端
    char       *ip;        // 绑定 / 连接地址
    int        *port;      // 端口（指向命令行解析出来的全局变量）
    int        *send_size; // 本端每次发送的最大字节数
    int        *recv_size; // 本端接收缓冲大小（= 对端每次发送的上限）
} TEST_PEER;

static TEST_PEER g_peers[] = {
    { "server", "client", "client connected !"    , 1, server_bind_ip, &server_bind_port,
      &server_max_send_size, &client_max_send_size },
    { "client", "server", "connect to server ok !", 0, client_cnnt_ip, &client_cnnt_port,
      &client_max_send_size, &server_max_send_size },
};
#define TEST_PEER_SERVER  (&g_peers[0])
#define TEST_PEER_CLIENT  (&g_peers[1])

// 对端是否已失联：协议自己这么判定，或者我们已经很久没收到它的任何数据。
// 两侧用同一个判据（历史上只有客户端有"看多久没收到数据"这一半）。
static int peer_is_lost(void *ffrdp, uint32_t tick_recv)
{
    if (ffrdp_isdead(ffrdp)) return 1;
    return (int32_t)get_tick_count() - (int32_t)tick_recv > TEST_PEER_SILENCE_MS;
}

// 服务端与客户端共用的主循环：不停地发随机长度的数据、收对端数据、定期打出诊断。
static void* peer_thread(void *param)
{
    const TEST_PEER *peer = (const TEST_PEER*)param;
    const int send_max = *peer->send_size;
    const int recv_max = *peer->recv_size;
    uint8_t *sendbuf = malloc(send_max);
    uint8_t *recvbuf = malloc(recv_max);
    void    *ffrdp   = NULL;
    uint32_t tick_dump, tick_recv = 0;
    int      size, ret, connected = 0;

    if (!sendbuf || !recvbuf) {
        printf("%s failed to allocate send or recv buffer !\n", peer->name);
        goto done;
    }

    tick_dump = get_tick_count();
    while (!g_exit) {
        if (!ffrdp) {
            ffrdp = ffrdp_init(peer->ip, *peer->port, NULL, NULL, peer->is_server, 1280, 0);
            if (!ffrdp) { usleep(100 * 1000); continue; }
        }

        size = 1 + rand() % send_max;
        // 发送失败只可能是应用侧队列满（被背压），属正常现象，不打印
        (void)ffrdp_send(ffrdp, (char*)sendbuf, size);

        ret = ffrdp_recv(ffrdp, (char*)recvbuf, recv_max);
        if (ret > 0) {
            if (connected == 0) {
                connected = 1;
                printf("%s\n", peer->msg_up);
            }
            tick_recv = get_tick_count();
            if ((int32_t)get_tick_count() - (int32_t)tick_dump > TEST_DUMP_INTERVAL_MS) {
                pthread_mutex_lock(&g_mutex);
                ffrdp_dump(ffrdp, 0);
                pthread_mutex_unlock(&g_mutex);
                tick_dump = get_tick_count();
            }
        }

        ffrdp_update(ffrdp);
        if (connected && peer_is_lost(ffrdp, tick_recv)) {
            printf("%s lost !\n", peer->peer);
            ffrdp_free(ffrdp); ffrdp = NULL;
            connected = 0;
        }
    }

done:
    free(sendbuf);
    free(recvbuf);
    ffrdp_free(ffrdp);
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
        printf("%*s[--server_max_send_size=N] [--client_max_send_size=N]\n",
               (int)strlen(prog) + 7, ""); // 7 = strlen("usage: ")，让接续行与上面那个括号对齐
        printf("\n");
        printf("  --server[=ip:port]            run as server    (default 0.0.0.0:8000)\n");
        printf("  --client[=ip:port]            run as client    (default 127.0.0.1:8000)\n");
        printf("  --server_max_send_size=N      server max bytes per send (default 16384)\n");
        printf("  --client_max_send_size=N      client max bytes per send (default 16384)\n");
        printf("\n");
        printf("  the two roles may be given together (self test on one host);\n");
        printf("  type 'quit' or 'exit' to stop; a diagnostic dump is printed every 10 s.\n");
        printf("\n");
        return 0;
    }

    for (i=1; i<argc; i++) {
        if (strstr(argv[i], "--server=") == argv[i]) {
            server_en = 1;
            if (strcmp(argv[i] + 9, "") != 0) {
                strncpy(server_bind_ip, argv[i] + 9, sizeof(server_bind_ip));
            }
        } else if (strstr(argv[i], "--client=") == argv[i]) {
            client_en = 1;
            if (strcmp(argv[i] + 9, "") != 0) {
                strncpy(client_cnnt_ip, argv[i] + 9, sizeof(client_cnnt_ip));
            }
        } else if (strcmp(argv[i], "--server") == 0) {
            server_en = 1;
        } else if (strcmp(argv[i], "--client") == 0) {
            client_en = 1;
        } else if (strstr(argv[i], "--server_max_send_size=") == argv[i]) {
            server_max_send_size = atoi(argv[i] + 23);
        } else if (strstr(argv[i], "--client_max_send_size=") == argv[i]) {
            client_max_send_size = atoi(argv[i] + 23);
        }
    }

    strtok_r(server_bind_ip, ":", &str); if (str && *str) server_bind_port = atoi(str);
    strtok_r(client_cnnt_ip, ":", &str); if (str && *str) client_cnnt_port = atoi(str);
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

    pthread_mutex_init(&g_mutex, NULL);
    if (server_en) pthread_create(&hserver, NULL, peer_thread, (void*)TEST_PEER_SERVER);
    if (client_en) pthread_create(&hclient, NULL, peer_thread, (void*)TEST_PEER_CLIENT);

    while (!g_exit) {
        char cmd[256];
        scanf("%256s", cmd);
        if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
            g_exit = 1;
        }
    }

    if (hserver) pthread_join(hserver, NULL);
    if (hclient) pthread_join(hclient, NULL);
    pthread_mutex_destroy(&g_mutex);
    return 0;
}
