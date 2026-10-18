# ffrdp

> **English** ✓ | [中文](readme.zh.md)
>
> **Language** ✓: this is the **default (English)** readme ✓ — the Chinese version is
> [`readme.zh.md`](readme.zh.md) ✓. Everything under `docs/` is **Chinese only** ✗ — if you need another
> language, please **translate it with an AI** ✓ (hand the document to any LLM; ask it to keep macro names,
> commands, code blocks and the `✓ / ✗` markers verbatim ✓).

ffrdp is a fast, reliable transport protocol built on **UDP + ARQ + FEC**. Its goals are **low latency** and
**stable bandwidth on lossy networks**.

Single-file implementation (`ffrdp.c`) plus one test program (`test.c`); **no third-party dependencies** other
than `pthread`, no kernel changes, and no change to UDP semantics.

---

## 1. Protocol overview (how it differs from TCP)

| Feature | Notes |
|---|---|
| Full ARQ in userspace | Acknowledgement, timeouts, retransmission, windowing and flow control all live in userspace, so the strategy can follow the link's shape instead of being bound to a fixed kernel policy |
| Selective repeat + no delayed ACK | **UNA + a MACK bitmap** states precisely *which* frame is missing (rather than "everything past sequence N is lost"); data is ACKed as soon as it arrives, instead of wasting a round trip on delayed ACKs |
| Fast retransmit (with a waiting discipline) | Most holes the peer reports are actually **reordering**, not loss ⇒ how long to wait is decided by **peer evidence** (since v1.1.0), not by an unconditional immediate retransmit |
| Forward error correction (FEC) | Implemented with XOR: one FEC frame is attached every N frames, with N carried in the frame type (application-selectable, off by default) |
| Latency control for "bufferbloated" links | The core of v1.1.0: treat the **number of frames in flight** as the real brake, and determine what it should be with **a single reproducible experiment** |
| Peer-evidence driven | The RTO floor and the fast-retransmit wait are both driven by "the peer keeps reporting the same thing" — the only signal on the link that this end's own counters cannot pollute |
| Diagnostics friendly | `ffrdp_dump()` prints the three latency components, in-flight amount, retransmissions, wasted fraction, etc.; a `version & build` line self-identifies the binary so you never "run the wrong one" (**since v1.1.0** it is merged with `version` into one line ✓; only v1.0.0 prints a standalone `version` line ✓) |
| **Handshake & connection management** (v2.0.0) | A three-way handshake to establish (SYN / SYN-ACK / ACK) and a **four-way FIN teardown**; the handshake and FIN use a **dedicated frame type** (35) ⇒ they do not consume data-ARQ sequence space ✓ |
| **Multi-client server** (v2.0.0) | The server keeps a **connection table** keyed by peer address (`maxconn`, default 16 ✓), one **handle** per connection; anything not yet handshaken is dropped ✓ |
| **Encryption left to the application** (v2.0.0) | The protocol hands the payload **as-is** to the `ENCRYPT` / `DECRYPT` callbacks (in place, same length ✓) ⇒ algorithms and keys live entirely on the application side ✓, and the library **no longer needs openssl at all** ✓ |
| **Idle is not disconnected** (v2.0.0) | If the peer has been silent for half the interval, a keep-alive `QUERY` is sent to confirm liveness ✓; only when the peer is really mute is it declared dead within `idle_ms` (default **5 s**) and `DISCONN` reported ✓ |

Almost everything added over v1.0.0 is an optimisation aimed at bufferbloated links (concurrency control and
latency probing); **the protocol itself did not get more complicated** (the frame format gained exactly one
optional byte, see below). **Size reference**: v1.0.0's `ffrdp.c` was **670 lines** ✓, v1.1.0 **1316 lines** ✓;
from v1.2.0 the transport layer is a separate file; from v2.0.0 the connection model and interface were
reworked — today `ffrdp.c` is **2240 lines** ✓, `lowlevel-udp.c` **550 lines** ✓, `socket.c` **85 lines** ✓,
`utils.c` **156 lines** ✓, `test.c` **667 lines** ✓ (**3698 lines** in total ✓, all standard C + `pthread` ✓).

---

## 2. Protocol versions

| Version | **Branch** | Frame format | Contents |
|---|---|---|---|
| **v1.0.0** | `v1.0.x` | 4-byte frame header; 8-byte ACK | Basic ARQ (selective repeat / fast retransmit / UNA+MACK / rwnd) + FEC |
| **v1.1.0** | `v1.1.x` | **unchanged** ✓ | **Logical** protocol optimisations: wasted-retransmission ACK counter (the only new optional byte), peer-evidence driven RTO floor, fast-retransmit waiting discipline, in-flight gate + latency probing |
| **v1.2.0** | `v1.2.x` | **unchanged** ✓ | **Transport-layer rework** (`lowlevel` interface + worker-thread driven) and the protocol fixes that followed (wakeup criterion, ACK in the same round, window scaling); parameters and spec in [`docs/ffrdp-v1.md`](docs/ffrdp-v1.md) §3.1 |
| **v2.0.0** (current) | `master` | v1 frame format **unchanged** ✓ + new **handshake frame type 35** (never enters data ARQ ✓) | **Interface and connection-model rework**: handle-based `send`/`recv` + event callbacks, three-way handshake and four-way teardown, multi-client `accept`, `keepalive`, encryption callbacks — see [`docs/ffrdp-v2.md`](docs/ffrdp-v2.md) |

**How to tell which version you are running** ✓: look at the `version & build : … compiled <date> <time>` line in
the dump (**since v1.1.0** `version` and `build` are merged into one line ✓; only **v1.0.0** still prints a
standalone `version : …` line ✓), then match it against the commit it was compiled from ✓.
(`FFRDP_VERSION` has existed since v1.0.0 ✓ — it is present in the `ffrdp.h` of all three v1 branches and of v2 ✓;
only **binaries built earlier** may lack it ✗, and then the `build` line plus the commit is all you have ✓.
The v2 dump also adds a **per-connection** layer ✓ — `ffrdp_dump(ctxt, conn, …)`: `conn < 0` prints the overview
(one block per connection ✓), `conn >= 0` prints **only that one** ✓; each block's **header** states the handle,
the peer address and the handshake state ✓; a slot that never communicated occupies a single line ✓.)

**Frame-format compatibility conclusion**: v1.1.0 **introduced no new frame type and changed neither the width
nor the meaning of any existing field** ✓; the only change is one **optional** byte at the tail of the ACK frame
(`dups`, sent only when there is something to report ✓). **v1.2.0 likewise changed neither the frame format nor
ACK semantics** ✓, but **three `ffrdp.h` API signatures changed** ✗ (see §3.1.6 ✓): `ffrdp_init()` now takes an
address string, `send` / `recv` each gained a `timeout_ms`, and `ffrdp_update()` was removed.
**v2.0.0's data plane is still isomorphic to v1** ✓ (frame header, ACK and FEC are byte-for-byte unchanged ✓);
**all that is new is the handshake frame**: a new frame type **35** ✓, while v1's parser only accepts ≤34 ⇒ an old
implementation simply **drops it silently** ✓ (details in [`docs/ffrdp-v2.md`](docs/ffrdp-v2.md)). ⇒ Interoperability
is therefore **decided by the handshake switch** ✓: `hs:0` (the old path) works in both directions ✓; **the default
`hs:1` does not interoperate with v1** ✓ (deliberately: v2's connection model is not the same thing as v1's ✓).

⇒ **There is an executable test for this** ✓ (no more "reading the code and thinking they look the same" ✗):
`V1_REF=<a v1.2.x binary> ./test.sh <a v2 binary> x` (run locally, no ssh ✓) —
① v1 server ← v2 client (both sides `hs:0`) **must work** ✓, ② v2 server ← v1 client (both sides `hs:0`) **must
work** ✓, ③ v2 server with defaults (`hs:1`) ← v1 client **must not work** ✓. Step 5 of `tools/check.sh` is exactly
this ✓.

---

## 3. Building

You need `gcc` (or a cross toolchain) + `pthread`. The build script is `build.sh`:

```bash
TOOLCHAIN_NAME=gcc ./build.sh                                  # native build
TOOLCHAIN_NAME=arm-linux CROSS_COMPILE=arm-linux- ./build.sh   # cross build
./build.sh clean                                               # remove artefacts
```

Artefacts (`BUILD_PACKAGE_OUTDIR` overrides the location, default `./out`):

```
out/$TOOLCHAIN_NAME/lib/libffrdp.a                      static library
out/$TOOLCHAIN_NAME/include/ffrdp.h                     header
out/$TOOLCHAIN_NAME/lib/pkgconfig/libffrdp.pc           pkg-config description
./test_ffrdp                                            test program (left in the source directory)
```

`build.sh` compiles with `-Wall -Werror -Wextra -Wunused-macros` (**warnings are errors**: extra arguments,
implicit fall-through and unused macros are all failures ✓), and by default passes the two feature macros of the
**dev / criteria profile**: `-DFFRDP_DIAG=1` (keep full diagnostics — `ffrdp_dump` and the counters only it uses ✓)
and `-DFFRDP_FEC_ENABLE=1` (full FEC ✓); **a release build passes neither macro** ✗ ⇒ the dump subsystem, the
diagnostic counters and the FEC buffers are all compiled out ✓. Linking adds `-lpthread` (Windows/MinGW targets
additionally add `-lws2_32`). `-DLINUX` is added automatically by `build.sh` on non-mingw targets ✓ — without it
the Linux implementations of `socket.c` / `utils.c` / `lowlevel-udp.c` are hidden behind `#ifdef LINUX` ⇒ you get
a pile of empty functions ✓.

The minimal manual compile if you would rather not use the script:

```bash
gcc -Wall -Werror -DLINUX -c ffrdp.c        -o ffrdp.o
gcc -Wall -Werror -DLINUX -c lowlevel-udp.c -o lowlevel-udp.o
gcc -Wall -Werror -DLINUX -c socket.c       -o socket.o
gcc -Wall -Werror -DLINUX -c utils.c        -o utils.o
ar  rcs libffrdp.a ffrdp.o lowlevel-udp.o socket.o utils.o
gcc -Wall -Werror -DLINUX test.c libffrdp.a -lpthread -o test_ffrdp
```

> ⚠️ The command above is the **minimal / release-shaped** build ✓ — without `-DFFRDP_DIAG=1` /
> `-DFFRDP_FEC_ENABLE=1` both diagnostics and FEC are compiled out ✓ (which is **another legitimate profile** ✓).
> For a byte-for-byte match with `build.sh`'s output ✓, just use `build.sh`, or add those two feature macros and the
> stricter warning switches yourself ✓.

---

## 4. The test program `test_ffrdp`

`test_ffrdp` runs one ffrdp instance in each of two threads (server and client share the same parameterised
peer loop ✓) and the two sides **continuously exchange random-length data**, calling `ffrdp_dump()` every 10
seconds to observe latency, retransmissions, in-flight amount and other metrics. **The server accepts multiple
clients through the `FFRDP_ACCEPT` event** ✓ (one handle per connection ✓), and each client's byte stream is
verified independently ✓.

```bash
./test_ffrdp --server=0.0.0.0:8000            # one end acts as the server
./test_ffrdp --client=192.168.1.10:8000       # the other end acts as the client
./test_ffrdp --server --client                # local self-test (both ends in one process)
```

| Option | Meaning | Default |
|---|---|---|
| `--server[=ip:port]` | act as the server and bind to that address | `0.0.0.0:8000` |
| `--client[=ip:port]` | act as the client and connect to that address | `127.0.0.1:8000` |
| `--server_max_send_size=N` | maximum bytes per **message** on the server (**including** the 12 B framing header+trailer) | 16384 |
| `--client_max_send_size=N` | maximum bytes per **message** on the client (**including** the 12 B framing header+trailer) | 16384 |
| `--send_speed=N` | **send rate limit**: cap this process's send rate at N KB/s | `0` (unlimited) |

### Message verification (done on every run)

`ffrdp_recv()` is an **ordered, reliable byte stream** (there are no message boundaries), so the test program adds
its own framing to every message:

```
[4B total len][4B seq][payload...][4B crc32]      crc32 covers len + seq + payload
```

The receiver reassembles the byte stream into messages and verifies each one: **seq continuity** (was anything
lost or reordered ✓), **crc32** (was the content corrupted ✓) and **length validity** (did the stream lose sync ✓).
The sender only advances `seq` when `ffrdp_send()` returns the full length ✓ (`ffrdp_send()` is **atomic**: either
the whole message is accepted or it returns `-1` having accepted nothing ✓).

Failures print `VERIFY ...` lines (at most 3 verbose lines per error class, the rest are only counted ✓), and a
summary is printed on exit:

```
[client] verify: sent 285 msgs / 1638311 B, recv 285 msgs / 1638311 B | seq_gap 0 (missing 0), crc_err 0, len_err 0, partial_err 0
[client] verify: OK -- received 285 messages, seq contiguous and crc all correct
```

Notes:

- `--server` and `--client` **may be given together** (in-process self-test); `--send_speed` then applies to both ends;
- **`--send_speed` controls the send rate the way iperf3 does**: with `--send_speed=100`, roughly 100KB per second is
  handed to `ffrdp_send()` (measured **100.37KB/s** ✓; 1KB/s, 20KB/s and 500KB/s track just as closely ✓).
  It is a token bucket: the full one-second allowance is replenished every 1 s, and the size of the next message is
  **chosen** from the remaining allowance (messages are atomic and cannot be trimmed ✓); pass `0` or nothing to disable ✓;
- **Without a rate limit the speed is decided by the test program itself** (framing + crc32 + pseudo-random fill cost
  CPU ✓) and is far above any real link ✓;
- With no arguments it prints usage and exits;
- **Exit with `quit` / `exit`** ✓ — only that path **prints the `verify:` summary above before exiting** ✓ (and with
  `FFRDP_CLOSE=1` it first runs through `ffrdp_close()`'s **four-way teardown** ✓);
- Internally it uses the fixed strings `"ffrdps://<ip:port>,mss:1280,fec:0"` (server) /
  `"ffrdpc://<ip:port>,mss:1280,fec:0"` (client) ⇒ **MSS 1280 bytes, FEC redundancy 0 (off)** ✓; change `test.c` for
  other combinations ✓;
- **Three environment switches** (**none set ⇒ exactly as before** ✓): `FFRDP_HS=0` disables the handshake and takes
  the old v1 path ✓; `FFRDP_IDLE=<seconds>` makes the application send **not a single byte** ⇒ specifically to
  verify "idle is not killed by mistake" ✓; `FFRDP_CLOSE=1` calls `ffrdp_close()` before exiting ⇒ verifies the
  **four-way teardown** ✓.

### lowlevel conformance test (`tools/test-lowlevel-udp.sh`)

Since v1.2.0 the protocol layer is decoupled from the socket: the foundation is the transport endpoint defined by
`lowlevel.h` (send + receive + timer, with its own worker thread). That layer has its own contract test, using only
127.0.0.1 and ports derived from the pid — no network and no configuration needed:

```bash
bash tools/test-lowlevel-udp.sh            # plain build (-Wall -Wextra -Werror)
ASAN=1 bash tools/test-lowlevel-udp.sh     # address sanitizer (with leak detection)
```

It covers, section by section: argument parsing ✓, received messages and source addresses ✓, sending back
(including `send` from inside a callback and from the main thread) ✓, payload boundaries (including truncation at
1500) ✓, throttling and burst load ✓, two endpoints and concurrency ✓, lifecycle (70 init/exit cycles with zero
leaked fds/threads ✓, prompt return from `exit` when there is no deadline ✓) and timers (deadline precision /
already past fires immediately / overwrite semantics / reset from inside a callback / cross-thread wakeup /
`RECV` and `TIMER` never interleave / callbacks never nest) ✓.
The design trade-offs and measured numbers are in section 13 of [`docs/notes.md`](docs/notes.md).

### Real-machine A/B comparison (`test.sh`)

Compresses one real-machine comparison into a single command ✓: **upload the binary → start the server remotely →
start the client locally → fetch the logs → produce a summary** ✓.

```bash
./test.sh <local-binary> <user@host> [password] [seconds]   # arg 1: relative / absolute / bare name all work ✓
export SESSION=z5-run1        # share one identifier across a scan, for grouped comparison (same as STAMP)
./test.sh dist/test_ffrdp_w5a4 chenk@192.168.0.213 abc 120
column -s, -t logs/summary.csv | tail -10     # the summary table
```

- When `sshpass` is not installed it opens an **SSH multiplexed connection** ⇒ you type the password once per run ✓
  (`KEEP_MUX=1` keeps it alive across runs ✓);
- **Two different versions** compared against each other:
  `REMOTE_BIN=<peer binary> ./test.sh <local binary> <user@host> …` ✓

**One command that runs every criterion** (run it after touching the protocol ✓, especially for performance work ✓):

```bash
bash tools/check.sh              # build + simulation (14 tests / 3 scenario groups) + ASAN + local two-process four paths + v1 interop
QUICK=1 bash tools/check.sh      # skip ASAN and e2e (about 1 minute) ✓
```
- Six steps: real-machine-profile `build.sh` ✓, simulation with the default model (HEAD and the working tree compared
  line by line ✓), simulation under **ASAN** ✓, local two-process runs (default / `FFRDP_HS=0` old path /
  `FFRDP_IDLE=1 FFRDP_CLOSE=1` idle+teardown / multi-client ✓) and **v1 interop** (`hs:0` must work in both
  directions, the default `hs:1` must not ✓);
- All green ⇒ exit 0, any red ⇒ exit 1 ⇒ it can be wired straight into `pre-push` or a pipeline ✓; logs land in
  `logs/check-<timestamp>/` ✓. (The local side still uses argument 1, the remote side `REMOTE_BIN` ✓ — so you can tell
  whether "one direction being cut" is a local send problem or a peer acknowledgement problem ✓);
- Other switches: `DUR` / `PORT` / `LOGDIR` ✓, `CLIENTS=N` (start N clients locally against one server ⇒ exercises
  multi-client ✓; the ceiling is the server's `maxconn` − 1 ✓), `CPUWATCH=1` (sample the client's CPU ✓),
  `PINGWATCH=1` (ping the peer once a second for the whole run ⇒ measure the link itself as min/avg/max + loss ✓, to
  tell "code or link" apart ✓), `CLEAN=1` (clean up the remote side afterwards ✓), `DRY=1` (print the plan only, touch
  no network ✓), `FFRDP_IDLE=1` / `FFRDP_CLOSE=1` (idle and teardown acceptance ✓), `V1_REF=<v1 binary>` (run the
  v2 ↔ v1.2.x interop matrix locally ✓, see §2 compatibility);
- Each run appends to **`$LOGDIR/summary.csv`** ✓ (`stamp,tag,role,resend_pct,waste_pct,rtts_ms,send_recv_MBps` ✓) ⇒
  after scanning a set of binaries, just read the table ✓;
- Read the criteria against the protocol document: `rttm/rtts`, `counter_resend_ratio`, `confirmed rate`,
  `inflight, cap`, `ack latency` — see [`docs/ffrdp-v1.md`](docs/ffrdp-v1.md) §3.2.4 ✓. ⚠️ **Dump field names have
  changed across versions** ✗ (for example the `lap period avgmax` of the v1.2.0 era was removed during the
  code-slimming ✗) ⇒ before trusting a field, compare it against the actual dump output of the build you have ✓.

---

## 5. Directory layout

```
.
├── ffrdp.c                 protocol implementation (single file: ARQ + FEC + flow/concurrency control + diagnostics)
├── ffrdp.h                 public API (v2.0.0: **8 functions**, see below — handle-based I/O + event callbacks)
├── lowlevel.h              lowlevel interface (tx + rx + timer, with its own worker thread) — since v1.2.0
├── lowlevel-udp.c          the UDP implementation of lowlevel (bind + send/recv + timer + wakeup socket)
├── socket.c / socket.h     thin socket wrapper (init, options, timeouts, non-blocking)
├── utils.c / utils.h       timestamps, thread setname/getname, module thread state machine
├── test.c                  test program (server/client share one parameterised peer loop + dump every 10 s)
├── test.sh                 **real-machine A/B comparison** (upload → remote server → local client → logs → summary; see §4)
├── build.sh                build script (static library + test program + pkg-config)
├── readme.md               this file, the **English version** (shown by default on GitHub ✓; kept in sync with the Chinese one ✓)
├── readme.zh.md            the Chinese version: overview, versions, build, tests, layout
├── LICENSE
├── docs/
│   ├── ffrdp-v1.md   protocol description: v1.0.0 / v1.1.0 / v1.2.0 in three parts (frame format, parameters, changes and rationale)
│   ├── ffrdp-v2.md    the v2.0.0 description: interface (params / handles / events), handshake and teardown, multi-client, encryption callbacks, keepalive; §9 covers the v2.x improvements over v1.1.x
│   ├── sim-test.md         how to use the differential simulation harness (tools/sim)
│   ├── ffrdp-latency.md audio/video latency budget and tuning: the formula, numbers for slow/fast APs, the cost of loss, MTU and fragmentation, how to measure
│   └── notes.md            development and debugging record (chronological, including the raw logs of every run)
└── tools/                  helper tools (not listed one by one ✓: **one command for every criterion** `tools/check.sh`, **compile-time memory account** `tools/size.sh`, differential simulation `tools/sim/`, hostile input, in-flight sweep, lowlevel contract test, log extraction — the directory contents are authoritative ✓)
```

### Public API (`ffrdp.h`) — **v2.0.0** ✓

```c
void* ffrdp_init (void *params, FFRDP_CALLBACK callback, void *cbctx);   // params below (role + configuration are in the string)
void  ffrdp_exit (void *ctxt);                                          // close every connection and free (joins the transport thread inside)
int   ffrdp_send (void *ctxt, int conn, char *buf, int len, int timeout_ms);  // atomic: len = whole message accepted; 0 = closed; -2 = queue full and no space within the timeout
int   ffrdp_recv (void *ctxt, int conn, char *buf, int len, int timeout_ms);  // ordered, reliable **byte stream** (no message boundaries), returns as soon as there is data
void  ffrdp_flush(void *ctxt, int conn);                                // send the not-yet-full frame immediately (conn < 0 = all connections)
void  ffrdp_dump (void *ctxt, int conn, int clearhistory);              // conn < 0 = overall state; conn >= 0 = detailed state of that connection
int   ffrdp_isdead(void *ctxt, int conn);                               // has **this connection** been declared lost
int   ffrdp_close(void *ctxt, int conn);                                // close actively: send FIN, report DISCONN only after the peer's FIN (new in v2 ✓)
```

```text
params (string configuration; a parse failure returns NULL and prints the reason ✓):
  "ffrdps://0.0.0.0:8000"                                   server (bind to that address)
  "ffrdpc://192.168.0.213:8000"                             client (connect to that address)
  "ffrdps://0.0.0.0:8000,maxconn:10"                        maximum number of connections (default 16, ceiling FFRDP_MAXCONN)
  "ffrdps://0.0.0.0:8000,handshake_ms:10000,idle_ms:5000"   handshake timeout / idle (keepalive) timeout
  ",mss:1280,fec:0"                                         this end's MSS and FEC redundancy (v1's smss / sfec)
  ",hs:0"                                                   disable the handshake ⇒ take the old v1 path (use this to interoperate with v1.x)
⚠️ Time keys **carry their unit** (`_ms` ✓) — the same style as `timeout_ms`, leaving no "config in seconds, internals in milliseconds" conversion point ✗.
```

- **Handle-based** ✓: `conn` is a connection handle (the server obtains it from the `FFRDP_ACCEPT` event ✓);
  **handles are never reused** ✓ ⇒ a stale handle can only refer to an "already closed" connection ✓ and yields
  `FFRDP_CONN_INVALID` ✓.
- **Event callbacks** ✓: `FFRDP_ACCEPT` / `CONNECT` / `DISCONN` / `ENCRYPT` / `DECRYPT` / `RECVDATA` — semantics,
  callback return values (`FFRDP_CONSUMED` / `NOT_CONSUMED`) and the encryption contract are in
  [`docs/ffrdp-v2.md`](docs/ffrdp-v2.md) ✓.
- **Return codes** ✓: `> 0` = byte count ✓, `FFRDP_CLOSED(0)` = peer closed ✓, `FFRDP_EAGAIN(-2)` = nothing for now /
  no space within the timeout ✓, `FFRDP_CONN_INVALID(-1)` = invalid handle (including **already closed** ones ✓).
- **`timeout_ms`** ✓: `0` = do not wait (old behaviour ✓), `> 0` = wait at most that many milliseconds ✓, `< 0` = wait
  until something can be received / sent ✓.
- **Handshake is on by default** ✓ (`hs:0` turns it off ⇒ v1 behaviour ✓); **idle keep-alive** is governed by `idle_ms` ✓
  (default 5000 ms ✓).

> **Legacy interface on record (the `v1.2.x` branch)** ✓ — the following is the current interface only on the v1.2.x
> branch ✓; it is gone from v2's `ffrdp.h` ✗.
> The three signature differences (`ffrdp_init` takes an address string, `send` / `recv` gained a `timeout_ms`,
> `ffrdp_update` removed) are in [`docs/ffrdp-v1.md`](docs/ffrdp-v1.md) §3.1.6 ✓.

```c
void* ffrdp_init  (char *addr, char *txkey, char *rxkey, int server, int smss, int sfec);   // addr = "ip:port" / "ip" (port defaults to 8000)
void  ffrdp_free  (void *ctxt);
int   ffrdp_send  (void *ctxt, char *buf, int len, int timeout_ms);  // atomic: len = whole message accepted; -1 = not a single byte accepted
int   ffrdp_recv  (void *ctxt, char *buf, int len, int timeout_ms);  // fetch application data (ordered, reliable **byte stream**, no message boundaries)
int   ffrdp_isdead(void *ctxt);                       // has the peer been lost
void  ffrdp_flush (void *ctxt);                       // send the not-yet-full frame immediately (do not hoard it)
void  ffrdp_dump  (void *ctxt, int clearhistory);     // print diagnostics
```

---

## 6. Documentation index

| To learn about | Read |
|---|---|
| Frame format, mack, FEC, RTO computation, communication flow | part 1 (v1.0.0) of [`docs/ffrdp-v1.md`](docs/ffrdp-v1.md) (Chinese) |
| The motivation for each change, the routes that were rejected, the raw logs | [`docs/notes.md`](docs/notes.md) (Chinese) |
| How to prove "no regression" without a real machine | [`docs/sim-test.md`](docs/sim-test.md) (Chinese) |
| **One command that runs every criterion** (build / simulation / ASAN / local two-process four paths / v1 interop) | `tools/check.sh` (usage in §4) |
| Contract test for the transport layer (packets / timers / wakeup) | `tools/test-lowlevel-udp.sh` (usage in §4) |
| The v2.0.0 interface (params / handles / events / encryption callbacks / handshake and teardown / keepalive) | [`docs/ffrdp-v2.md`](docs/ffrdp-v2.md) (Chinese) |
| v2.0.0's design trade-offs and boundary conditions (connection table / handles not reused / half-open ceiling / encryption contract) | [`docs/ffrdp-v2.md`](docs/ffrdp-v2.md) (Chinese) |
| **A/V (H.265 / VBR) latency budget and tuning**: the formula, numbers for slow/fast APs, the cost of a loss, MTU and IP fragmentation, how to measure | [`docs/ffrdp-latency.md`](docs/ffrdp-latency.md) (Chinese) |
| **v2.x improvements over v1.1.x** (with acceptance criteria, known limits, rejected routes and still-open directions) | [`docs/ffrdp-v2.md`](docs/ffrdp-v2.md) §9 (Chinese) |

> Everything under `docs/` is written in Chinese ✗. If you need English (or any other language) versions, please
> **translate them with an AI** ✓ — see the note at the top of this file.

---

## 7. Author

rockcarry@163.com
