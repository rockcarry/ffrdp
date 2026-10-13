// ffrdp-fuzz -- a hostile peer for a running ffrdp server.
//
// The question it answers: if the server binds a port, and every byte that reaches that port is chosen
// by someone else, does the process at least stay alive? This is that someone else: it speaks no
// protocol, sends one datagram at a time, and watches what happens to the process.
//
// It deliberately does NOT link the library. It must not: it is emulating the far side of the port, and
// half of what it sends is not a frame any implementation would ever produce. The frame layout it knows
// (a type byte in byte 0, a 24 bit sequence in bytes 1..3, an ack's una/mask/window in bytes 0..7) is
// taken from docs/ffrdp-v1.md, and the fragments mirror those of tools/sim/t_fuzz.c -- that one
// feeds the same shapes to the parser in memory, this one puts them on a wire.
//
// Two ways to watch a server:
//   --spawn <command...>  start it here and ask waitpid() after every datagram, so a crash is named by
//                         signal and the datagram that caused it is printed. Exact, and the default.
//   --port <n>            talk to a server that is already running. Liveness can then be no more than
//                         "it still answers": a QUERY frame is sent every so often, and a server that
//                         stops answering is reported together with the datagram before the silence.
//
// After a crash the server is restarted and the run continues, so one pass collects every distinct way
// to kill it rather than just the first.
//
// Build:  cc -O2 -Wall -o ffrdp-fuzz tools/ffrdp-fuzz.c
// Run:    ./ffrdp-fuzz --spawn ./test_ffrdp --server=0.0.0.0:18111
//         ./ffrdp-fuzz --port 18111 --host 192.168.0.213
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

// the frame types this tool pretends to know (docs/ffrdp-v1.md 1.6): 0 full, 1 short, 2..32 fec,
// 33 ack, 34 query.
#define T_FULL   0
#define T_SHORT  1
#define T_FEC2   2
#define T_FEC32  32
#define T_ACK    33
#define T_QUERY  34

#define MAX_FRAG 1600

static uint32_t rnd_state = 1;
static uint32_t rnd(void) { rnd_state = rnd_state * 1103515245u + 12345u; return (rnd_state >> 16) & 0x7FFF; }

// ---- what gets sent ----------------------------------------------------------------------------------
// A fragment is built here, with no regard for whether any implementation would produce it. The first
// block is the one that matters: lengths around a frame's own overhead, because that is where a
// receiver that trusts the length for arithmetic (payload size, buffer offsets) can be made to compute
// a zero or a negative. Then the ordinary parsers, then random bytes.
static const unsigned char sweep_types[] = { T_FULL, T_SHORT, T_FEC2, T_FEC32, T_ACK, T_QUERY };
#define SWEEP_TYPES   (int)(sizeof(sweep_types) / sizeof(sweep_types[0]))
#define SWEEP_LENS    11          // 0..10
#define SWEEP_PAYS    3
#define CORPUS_CASES  (SWEEP_TYPES * SWEEP_LENS * SWEEP_PAYS)

static int build_fragment(int idx, unsigned char *buf)
{
    int len, i;
    memset(buf, 0, MAX_FRAG);
    if (idx < CORPUS_CASES) {
        int t = idx / (SWEEP_LENS * SWEEP_PAYS);
        int l = (idx / SWEEP_PAYS) % SWEEP_LENS;
        int p = idx % SWEEP_PAYS;
        len = l;
        for (i = 0; i < len; i++) buf[i] = (p == 0) ? 0x00 : (p == 1) ? 0xFF : (unsigned char)(0x10 + i);
        if (len >= 1) buf[0] = sweep_types[t];   // bytes 1..3 stay zero: sequence 0 is "in order"
        return len;
    }
    idx -= CORPUS_CASES;
    len = (int)(rnd() % 96);
    for (i = 0; i < len; i++) buf[i] = (unsigned char)rnd();
    if (len >= 1 && (idx % 8) != 0) buf[0] = sweep_types[rnd() % SWEEP_TYPES];
    return len;
}

// ---- liveness ----------------------------------------------------------------------------------------
static pid_t  child = -1;                  // --spawn mode: the server we started
static char   child_cmd[2048];
static const char *log_file = NULL;         // --log FILE: keep the server's own output (debugging)
static int    port = 0;                    // --port mode: the server we were told about
static struct sockaddr_in peer;
static int    fd = -1;

static int start_child(void)
{
    char *argv[64];
    int   argc = 0;
    char *p, *save = NULL;
    char  cmd[sizeof(child_cmd)];           // strtok_r writes into what it tokenises: it must get a copy,
    int   hold[2];                          // or the second crash cannot be reproduced at all
    memcpy(cmd, child_cmd, sizeof(cmd));
    for (p = strtok_r(cmd, " ", &save); p && argc < 63; p = strtok_r(NULL, " ", &save)) argv[argc++] = p;
    argv[argc] = NULL;
    if (argc == 0) return -1;
    // its stdin must stay open and silent: test_ffrdp exits when stdin reaches end of file, and a server
    // that leaves on its own would look exactly like a server this tool killed. an unwritten pipe never
    // ends, so the loop stays where it is.
    if (pipe(hold) != 0) return -1;
    child = fork();
    if (child < 0) { close(hold[0]); close(hold[1]); return -1; }
    if (child == 0) {
        int devnull = open("/dev/null", O_RDWR);
        dup2(hold[0], 0);
        if (log_file) {                                  // --log FILE keeps the server's own output
            int lg = open(log_file, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (lg >= 0) { dup2(lg, 1); dup2(lg, 2); }
        } else if (devnull >= 0) { dup2(devnull, 1); dup2(devnull, 2); }
        execvp(argv[0], argv);
        _exit(127);
    }
    close(hold[0]);                                      // keep hold[1] open for the whole run
    usleep(300 * 1000);                    // let it bind before the first datagram is thrown at it
    return 0;
}

// is the server still there? In spawn mode the answer is exact. Against a server we did not start it can
// only be "it still answers a QUERY", which is what this asks.
static int server_alive(int *sig_out, int *code_out)
{
    int st;
    if (sig_out) *sig_out = 0;
    if (code_out) *code_out = 0;
    if (child > 0) {
        if (waitpid(child, &st, WNOHANG) == child) {
            if (sig_out)  *sig_out  = WIFSIGNALED(st) ? WTERMSIG(st) : 0;
            if (code_out) *code_out = WIFSIGNALED(st) ? 0 : WEXITSTATUS(st);
            return 0;
        }
        return 1;
    }
    {   // ask it something it must answer: an ack is sent back for a query frame
        unsigned char q[4] = { T_QUERY, 0, 0, 0 };
        struct timeval tv = { 0, 200 * 1000 };
        fd_set rs;
        unsigned char junk[64];
        sendto(fd, q, 1, 0, (struct sockaddr*)&peer, sizeof(peer));
        FD_ZERO(&rs); FD_SET(fd, &rs);
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        if (recvfrom(fd, junk, sizeof(junk), 0, NULL, NULL) <= 0) return 0;
        return 1;
    }
}

static void print_hex(const unsigned char *buf, int len)
{
    int i;
    for (i = 0; i < len && i < 16; i++) printf(" %02x", buf[i]);
    if (len > 16) printf(" ... (%d bytes)", len);
}

int main(int argc, char *argv[])
{
    static unsigned char buf[MAX_FRAG];
    int  i, cases, random_n = 200, delay_us = 1000, dead_total = 0;
    int  spawned = 0;
    const char *host = "127.0.0.1";

    for (i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--spawn") && i + 1 < argc) { spawned = 1; snprintf(child_cmd, sizeof(child_cmd), "%s", argv[++i]); }
        else if (!strcmp(argv[i], "--port")  && i + 1 < argc) { port  = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--host")  && i + 1 < argc) { host  = argv[++i]; }
        else if (!strcmp(argv[i], "--random")&& i + 1 < argc) { random_n = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--seed")  && i + 1 < argc) { rnd_state = (uint32_t)strtoul(argv[++i], NULL, 0); }
        else if (!strcmp(argv[i], "--delay") && i + 1 < argc) { delay_us = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--log")   && i + 1 < argc) { log_file = argv[++i]; }
        else {
            fprintf(stderr,
                "usage: %s (--spawn \"<server command>\" | --port <n> [--host <ip>])\n"
                "          [--random <n>] [--seed <n>] [--delay <us>]\n"
                "  --spawn \"../test_ffrdp --server=0.0.0.0:18111\"   start it, watch waitpid (exact)\n"
                "  --port 18111                                     watch a server already running\n", argv[0]);
            return 2;
        }
    }
    if (!spawned && !port) { fprintf(stderr, "need --spawn or --port\n"); return 2; }

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 2; }
    if (spawned && port == 0) {              // pull the port out of the command so we know where to aim
        char *p = strstr(child_cmd, "--server=");
        if (p) { p = strchr(p, ':'); if (p) port = atoi(p + 1); }
        if (!port) {                            // --server=0.0.0.0:18111 was not given: fail loudly
            fprintf(stderr, "could not find a port in the server command (need --server=ip:port)\n");
            return 2;
        }
    }
    memset(&peer, 0, sizeof(peer));
    peer.sin_family      = AF_INET;
    peer.sin_port        = htons((uint16_t)port);
    peer.sin_addr.s_addr = inet_addr(host);

    cases = CORPUS_CASES + random_n;
    printf("fuzz: %d fragments -> %s:%d%s\n", cases, host, port,
           spawned ? "  (server started here, liveness = waitpid)" : "  (server already running, liveness = does it still answer)");
    if (spawned) {
        int code = 0;
        if (start_child() != 0) { fprintf(stderr, "could not start the server\n"); return 2; }
        // A server that never came up is not a server this tool killed. Without this check a wrong path,
        // a wrong working directory or a port already in use reads as "every single fragment crashed it",
        // which is the loudest possible false alarm (measured: 698 out of 698, all with an exit code, none
        // with a signal -- the command simply did not exist where it was run from).
        if (!server_alive(NULL, &code)) {
            fprintf(stderr, "the server did not start (exit code %d): check the command and the directory "
                            "it is run from\n", code);
            return 2;
        }
    }

    for (i = 0; i < cases; i++) {
        int len = build_fragment(i, buf);
        int sig = 0, code = 0;
        if (len > 0) { if (sendto(fd, buf, len, 0, (struct sockaddr*)&peer, sizeof(peer)) < 0 && errno != EAGAIN) { /* server may already be gone */ } }
        if (delay_us > 0) usleep(delay_us);
        // liveness: every datagram in spawn mode; every 16th one when we can only ask
        if (spawned || (i % 16) == 0) {
            if (!server_alive(&sig, &code)) {
                dead_total++;
                printf("  DIED after #%d (len %d |", i, len);
                print_hex(buf, len);
                printf(" ) : %s\n", sig ? strsignal(sig) : "exit code != 0");
                if (spawned) {
                    int rc2 = 0;
                    if (start_child() != 0 || !server_alive(NULL, &rc2)) {
                        fprintf(stderr, "the server could not be restarted (exit code %d); stopping\n", rc2);
                        return 2;
                    }
                }
                else break;                     // nothing to restart: the answer is already reported
            }
        }
    }

    if (child > 0) { kill(child, SIGTERM); waitpid(child, NULL, 0); }
    printf("%s: %d fragments, %d crash%s\n", dead_total ? "FAIL" : "PASS", cases, dead_total,
           dead_total == 1 ? "" : "es");
    return dead_total ? 1 : 0;
}
