#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include "ffrdp.h"

#pragma warning(disable:4996) // disable warnings
#define usleep(t) Sleep((t) / 1000)
#define get_tick_count GetTickCount

static int  g_exit = 0;
static char server_bind_ip[32]   = "0.0.0.0";
static char client_cnnt_ip[32]   = "127.0.0.1";
static int  server_bind_port     = 8000;
static int  client_cnnt_port     = 8000;
static int  server_max_send_size = 16 * 1024;
static int  client_max_send_size = 16 * 1024;
static pthread_mutex_t g_mutex;

static void* server_thread(void *param)
{
    uint8_t *sendbuf= malloc(server_max_send_size);
    uint8_t *recvbuf= malloc(client_max_send_size);
    void    *ffrdp  = NULL;
    uint32_t tick_start, total_bytes;
    int      size, ret, client_connected = 0;

    (void)param;
    if (!sendbuf || !recvbuf) {
        printf("server failed to allocate send or recv buffer !\n");
        goto done;
    }

    tick_start  = get_tick_count();
    total_bytes = 0;
    while (!g_exit) {
        if (!ffrdp) {
            ffrdp = ffrdp_init(server_bind_ip, server_bind_port, 1, 1024, 10);
            if (!ffrdp) { usleep(100 * 1000); continue; }
        }
        size = 1 + rand() % server_max_send_size;

        ret = ffrdp_send(ffrdp, (char*)sendbuf, size);
        if (ret != size) {
//          printf("server send data failed: %d\n", size);
        }

        ret = ffrdp_recv(ffrdp, (char*)recvbuf, client_max_send_size);
        if (ret > 0) {
            if (client_connected == 0) {
                client_connected = 1;
                printf("client connected !\n");
            }
            total_bytes += ret;
            if ((int32_t)get_tick_count() - (int32_t)tick_start > 10 * 1000) {
                pthread_mutex_lock(&g_mutex);
                ffrdp_dump(ffrdp, 0);
                pthread_mutex_unlock(&g_mutex);
                tick_start = get_tick_count();
                total_bytes= 0;
            }
        }

        ffrdp_update(ffrdp);
        if (client_connected && ffrdp_isdead(ffrdp)) {
            printf("client lost !\n");
            ffrdp_free(ffrdp); ffrdp = NULL;
            client_connected = 0;
        }
    }

done:
    free(sendbuf);
    free(recvbuf);
    ffrdp_free(ffrdp);
    return NULL;
}

static void* client_thread(void *param)
{
    uint8_t *sendbuf= malloc(client_max_send_size);
    uint8_t *recvbuf= malloc(server_max_send_size);
    void    *ffrdp  = NULL;
    uint32_t tick_start, tick_recv, total_bytes;
    int      size, ret, connect_ok = 0;

    (void)param;
    if (!sendbuf || !recvbuf) {
        printf("client failed to allocate send or recv buffer !\n");
        goto done;
    }

    tick_start  = get_tick_count();
    total_bytes = 0;
    while (!g_exit) {
        if (!ffrdp) {
            ffrdp = ffrdp_init(client_cnnt_ip, client_cnnt_port, 0, 1280, 0);
            if (!ffrdp) { usleep(100 * 1000); continue; }
        }
        size = 1 + rand() % client_max_send_size;

        ret = ffrdp_send(ffrdp, (char*)sendbuf, size);
        if (ret != size) {
//          printf("client send data failed: %d\n", size);
        }

        ret = ffrdp_recv(ffrdp, (char*)recvbuf, server_max_send_size);
        if (ret > 0) {
            if (connect_ok == 0) {
                connect_ok = 1;
                printf("connect to server ok !\n");
            }
            tick_recv    = get_tick_count();
            total_bytes += ret;
            if ((int32_t)get_tick_count() - (int32_t)tick_start > 10 * 1000) {
                pthread_mutex_lock(&g_mutex);
                ffrdp_dump(ffrdp, 0);
                pthread_mutex_unlock(&g_mutex);
                tick_start = get_tick_count();
                total_bytes= 0;
            }
        }

        ffrdp_update(ffrdp);
        if (connect_ok && get_tick_count() - tick_recv > 2000) {
            printf("server lost !\n");
            ffrdp_free(ffrdp); ffrdp = NULL;
            connect_ok = 0;
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
        printf("ffrdp test program - v1.0.0\n");
        printf("usage: ffrdp_test --server=ip:port --client=ip:port\n\n");
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

    strtok_s(server_bind_ip, ":", &str); if (str && *str) server_bind_port = atoi(str);
    strtok_s(client_cnnt_ip, ":", &str); if (str && *str) client_cnnt_port = atoi(str);
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
    if (server_en) pthread_create(&hserver, NULL, server_thread, NULL);
    if (client_en) pthread_create(&hclient, NULL, client_thread, NULL);

    while (!g_exit) {
        char cmd[256];
        scanf("%256s", cmd);
        if (stricmp(cmd, "quit") == 0 || stricmp(cmd, "exit") == 0) {
            g_exit = 1;
        }
    }

    if (hserver) pthread_join(hserver, NULL);
    if (hclient) pthread_join(hclient, NULL);
    pthread_mutex_destroy(&g_mutex);
    return 0;
}

