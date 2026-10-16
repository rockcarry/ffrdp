#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include "utils.h"
#include "lowlevel.h"
#include "ffrdp.h"

// 帧的固定开销：4 字节头（类型/长度）+ FEC 帧再加 2 字节 fec seq。见 frame_node_new / frame_payload_size。
#define FFRDP_FRAME_HEAD      4
#define FFRDP_FRAME_FEC_TAIL  2
#define FFRDP_FRAME_OVERHEAD  (FFRDP_FRAME_HEAD + FFRDP_FRAME_FEC_TAIL)
// 单帧载荷上限：由传输层的一帧上限派生（**不再写死 1500**）——减掉最大帧开销，再向下对齐到 4 字节。
// 1500 - 6 = 1494 ⇒ &~3 = 1492，与旧值 (1500 - 8) 相同 ⇒ 这次只是把来源写清、行为不变。
#define FFRDP_MAX_MSS       ((FFRDP_LOWLEVEL_FRAME_MAX - FFRDP_FRAME_OVERHEAD) & ~3)
#define FFRDP_MIN_RTO        16   // RTO 硬下限（v1.1.0 原值）。
                                  // 2026-10-12 真机教训：改成的不是这个常数，而是"让 RTO 自己去够到延迟尾部"。
                                  // 真机上原有的 33~55 次/秒伪超时（resend_waste 99.7~100%）不是 RTO 太小，
                                  // 而是**估计器看不见尾部**：RTT 采样把"被重传过的帧"整批排除，而迟到 85~92 ms
                                  // 的恰恰全是这批 ⇒ 任何 rtts/rttd 公式都学不到它（临时把下限抬到 100 ms 能治好，
                                  // 已验证，但那是常数、在真丢包链路上要付恢复变慢的代价）。
                                  // 更好的做法见 rtt_tail：只采"对端其实早就有"的那种超时，把它学进 RTO。
#define FFRDP_MAX_RTO        2000
// ①-41：在途帧数上限（判在 ffrdp_send 的受理判据里）。它原来还是应用队列容量的乘数 ✓，队列已删 ✓。
// 必须 >= FFRDP_INFLIGHT_MAX（以及 FFRDP_MAX_CWND_SIZE）：比它小就成了比协议闸门更紧的刹车，在途
// 永远填不满窗口，高 BDP 链路上会掉速 —— 而回环 RTT 极小，测不出来。默认取值与此二者相等。
#define FFRDP_MAX_WAITSND    256
#define FFRDP_QUERY_CYCLE    500
#define FFRDP_FLUSH_TIMEOUT  500
#define FFRDP_DEAD_TIMEOUT   5000
#define FFRDP_MIN_CWND_SIZE  1
#define FFRDP_DEF_CWND_SIZE  32
#define FFRDP_MAX_CWND_SIZE  128
// 连接表容量（v2：maxconn，默认 16 ✓）。句柄 = 下标 ✓、索引永不复用 ✓（见 ffrdp.h 的设计说明）。
#define FFRDP_MAXCONN        16
// ── 握手（v2）────────────────────────────────────────────────────────────
// 只吃一个帧类型号，子类型放载荷第 1 字节（将来加字段只扩载荷，不再占类型号）。
// 构造方式照抄 QUERY：小栈缓冲 + lowlevel->send() 直接发 ⇒ 绕开 frame_node 的 FEC 尾巴。
// 帧类型 35 是安全的：ffrdp_on_frame 只认 ≤34，其余一律静默忽略（v1 收到会直接丢掉 ✓）。
#define FFRDP_FRAME_TYPE_HANDSHAKE 35
#define FFRDP_HS_SYN      1
#define FFRDP_HS_SYN_ACK  2
#define FFRDP_HS_ACK      3
#define FFRDP_HS_FIN      4
#define FFRDP_HS_RETRY_MS 500   // 重传间隔（总时限用配置的 handshake_ms ✓）
// 握手状态（每条连接）；0 = 无握手（calloc 清零后的初值，不需要单独的常量 ✓）
#define HS_SYN_SENT 1   // 客户端发了 SYN，等 SYN-ACK
#define HS_SYN_RCVD 2   // 服务端发了 SYN-ACK（半开），等 ACK
#define HS_UP       3   // 已建立
#define HS_FIN_WAIT 4   // 我方已发 FIN，等对端的 FIN（v2 主动关 ✓）
#define HS_FAILED   5   // 握手超时（客户端）：此后 send/recv 一律按"已断"处理
                                  // 2026-10-12 真机 A/B：这个上限与 FFRDP_INFLIGHT_MAX 一起从 64 抬到
                                  // 128 后，两侧吞吐同时 ×3（客户端 2.5->7.1 MB/s，服务端 3.8->11.4），
                                  // 服务端 counter_send_failed 9120->254；再抬到 256 基本不再涨（拐点在
                                  // 128）。注意它同时是 ffrdp_drain_tx 的早退判据，所以不要只改在途闸门。

// step29: the real brake -- the number of frames allowed on the wire at once. The peer's advertised window
// cannot serve as one: it is handed out again on every ack, so a limit placed on it never binds (measured:
// 63 frames in flight while the cap read 22).
// 2026-10-14 真机扫描（tools/inflight-sweep.sh，同批多档、各 180s、两端同版本 ✓；见 notes §26.15/§26.16）：
//   快 AP  cap 5 ⇒ 0.87 MB/s / rtts 21ms ｜ cap 64 ⇒ 2.66~3.20 / 6~18ms（×3 ✓）｜ cap128 ⇒ 3.14~3.80 / 24~97ms ✗
//   慢 AP  cap 5 ⇒ 0.34 / **14ms** / 重传 0.12% ✓ ｜ cap 64 ⇒ 0.32 / **227ms** ✗ / 2.86% ✗
//   lo     cap 64 比 cap 5 慢 ~1.5 倍 ✗（结构性代价，与闸门取值无关 ✓）
// ⇒ 同一个常数救不了两条链路 ✗（快 AP 要 64 ✓、慢 AP 要 5 ✓）⇒ 这里取"对谁都无害"的 64 ✓，
//   自适应交给探测（FFRDP_PROBE_ENABLE ✓）—— 实测"探测开 + 地板 32"：快 AP 4.57~8.0 MB/s ✓（还快过静态 64 ✓）、
//   慢 AP rtts 104~116ms / 重传 0.29~1.25% ✓，无一处变差 ✓。
// 三道闸同步核对：MAX_WAITSND 256 ✓ 与 MAX_CWND_SIZE 128 ✓ 本来就 >= 64 ✓（见上面那条坑 ✓）。
// 别混 ✓：waste% 随窗口涨而重传率 ≤0.25% ⇒ 那是 RTO 尾部那条独立线索（§26.5/§26.12）✓，不是闸门的判据 ✗。
#define FFRDP_INFLIGHT_MAX   64
                                  // It is a source constant, not a build switch: change it here and rebuild.
// 2026-10-14（第二轮）：地板 32 → **4**。2026-10-15 真机结果 ✓：慢 AP 两侧分别稳在 4 / 8 帧，
// rtts **12 / 20ms**（原 116 / 224ms ✓）、吞吐 0.32/0.64 与 0.61/0.30 不降反升 ✓、重传 0.46/0.24% ✓。
// 推导：慢 AP 的延迟就是"在途字节 ÷ 该 AP 速率"✓——
// 32 帧（41KB）÷ 0.34MB/s = 120ms（实测 116ms ✓）、5 帧（6.4KB）= 19ms（实测 14ms ✓）
// ⇒ 要拿到 ~15-20ms，就必须允许探测走到 ~4-6 帧 ✓（§26.16 里"32"只是当时判据会抖的权宜 ✓；
// 现在 keep 有了第二条证据（重传强度 ✓），下探才稳得住 ✓）。
// 命名原因：闸门 / 探测 / 预期窗口三处曾各写 4 或 8 而漂开 ⇒ 现在共用一个常量 ✓。
// ══ ①-27（本轮实验 ✓）：三道闸门的**运行时**旋钮（默认全部 = 源码常量 ⇒ 零行为变化 ✓）══════
// 为什么需要：①-26 查明**默认节拍是 app-limited**（96.6% 的圈因表空而停 ✓）⇒ 那时调闸门是空转 ✗，
// 之前 `capN` 那批实验（cap 5/8/16/32/64/128 ✓）就是这么得出"加大没用"的 ✗；而让应用能灌（burst ✓）
// 之后**在途闸门就是工作点**（占用 89.6% ✓、88% 的圈被它停 ✓）⇒ 现在才量得出它值多少 ✓。
// ⚠️ 三个必须一起动 ✓：`FFRDP_MAX_CWND_SIZE` **同时**是 `ffrdp_drain_tx` 的早退判据（源码注释里
//    写着"不要只改在途闸门" ✗），而 `FFRDP_MAX_WAITSND` 是应用侧的帧数闸 ✓。
// 实验做完，选定的值应该写回上面那几个源码常量 ✓（注释里那句 "a source constant, not a build switch" ✓）。
static uint32_t g_inflight_max = FFRDP_INFLIGHT_MAX;
// ①-39（2026-10-16）：探测器的**内存视角**开关 ✓（env `FFRDP_MEMPROBE=0` 可关 ✓）。
// 为什么需要第三种触发：原来那两条问的都是"链路在排队吗"（rtts > 3×rtt_min ✓）与
// "重传强度上来了吗"（waste ≥ 5 pm ✓）—— 这两条在**健康快链路**上永远不成立 ✗
// （真机实测：50 MB/s 回环、waste 0 pm、rtts/rtt_min = 0/0 ⇒ probes 0 次 ✗，
// 见 §26.59 那份 dump ✓），可每在途帧 ≈1.3 KB ⇒ 上限 64 帧就是 ~84 KB 的**峰值**内存 ✓。
// 对内存敏感的场景（嵌入式），"上限远远用不满"本身就是值得下探的理由 ✓。
// 安全网不变 ✓：keep 只认"确认速率没掉"或"重传强度低 20% 以上"（§26.18 起就不再要求 RTT 下降 ✓），
// revert 会把这个下探还回去、并把下次间隔翻倍（10s→20s→40s→60s ✓）。
static uint32_t g_probe_mem = 1;
// 没到这么深的在途就不值得再问（否则会一路问到地板、把来回测量变成抖动 ✓）
#define FFRDP_MEMPROBE_MIN_OUT 8u
static uint32_t g_max_cwnd     = FFRDP_MAX_CWND_SIZE;
static uint32_t g_waitsnd      = FFRDP_MAX_WAITSND;
// ①-41（实验）：**未发出帧数**预算 ✓ —— 应用队列被删掉之后，接管它"突发整形"那一半职责 ✓。
// 为什么必须是这个量：实测（lo ✓）删掉队列后吞吐 35.6 → 3.3 MB/s ✗、重传 0.16% → 5.55% ✗，
// 根因是应用按自己的速度直接攒帧 ⇒ 一次可以攒 100+ 帧（≈180 KB ✗）一把灌进内核缓冲 ⇒ 内核丢包
// ⇒ 拥塞崩溃 ✓。而"未发出"（= 已入发送表但还没上过线）正是能压住的量 ✓；
// **在途**（已发出、未确认）不受这一条限制 ✓ ⇒ 高 RTT 链路该需要的深度照旧（由 g_inflight_max 管 ✓）。
// 0 = 关闭（退化成"只有帧数闸"✓，也就是 lo 上崩掉的那一版 ✓）。env: FFRDP_UNSENT
static uint32_t g_unsent        = 24;   // 24 帧 ≈ 32 KB ≈ 原应用队列的整形量级 ✓
// ①-28 B：伪超时（`CEVENT_ACK_TIMEOUT_MISSED` —— 等的时候 ack 到过 ✓，是估计器没看见 ✓）
// **不再减半**。为什么：①-27 实测零丢包的 lo 上 **100% 的超时都是这一类** ✓（no-ack-arrived 0 /
// acks-arrived-but-missed 1350 ✓），而每来一个就把 cwnd 减半 ⇒ 窗口被钉在地板 ⇒ 吞吐塌 ✓。
// 默认 0 = 保持原行为（与快速重传同一档：乘性减半 ✓）⇒ 零行为变化 ✓。
static int      g_pseudo_nocut = 0;
// ①-29（**①-30 起转正：默认 ON** ✓）：`ffrdp_apply_acks` 里"洞等够久"那条分支，
// **同一帧只许减半一次** ✓。为什么：那条分支在**每次 ack** 都会重新走一遍发送表 ✓，而 CC 调用
// 没有"已经减过"的守卫 ✗ ⇒ 同一个帧每当 hole_wait 到点就被再判一次 ✓。
// 实测（§26.49 ✓ lo ✓ burst=8）：守卫关 ⇒ 770 次分支里 **729 次是重复减半** ✗、单帧最多 **62** 次 ✗
// （窗口抬到 256 时 **16 707/17 409** ✗、单帧最多 **238** 次 ✗✗）；开了之后真调 CC 770→**104** ✓、
// `cwnd-back` 829→**212** ✓，而同档吞吐 31.6 → **34.8/36.2 MB/s** ✓（两次复测同向 ✓，仍在单跑噪声带边缘 ✓）。
// 保留 env：`FFRDP_HOLE_CUT_ONCE=0` 可关回原行为（复现对照用 ✓）。
static int      g_hole_cut_once = 1;
// ①-35b/c：`ffrdp_send` 每收一条就 `lowlevel->timer(now)` 叫醒工作线程一次 ✗ —— 32 条的突发就是
// 32 次唤醒（§26.54：v2 ≈120 µs/消息 vs v1.1 ≈66 µs/消息，而 v1.1 的 send 只是 memcpy、零系统调用 ✓）。
// `FFRDP_SENDWAKE_COALESCE=1`（默认 0 = 原行为 ✓）⇒ **队列由空变非空时才叫醒** ✓：之后工作线程
// 自己会把队列抽干（`ffrdp_drain_tx` ✓，与 send 同一把锁 ✓ ⇒ 不会漏醒 ✓）并在抽空时把标志清掉 ✓。
static int      g_sendwake_coalesce = 0;
static int      g_knobs_read   = 0;
static void ffrdp_read_knobs(void)
{
    const char *e;
    if (g_knobs_read) return;
    g_knobs_read = 1;
    if ((e = getenv("FFRDP_INFLIGHT")) && *e) { unsigned v = (unsigned)atoi(e); if (v >= 1 && v <= 4096) g_inflight_max = v; }
    if ((e = getenv("FFRDP_MEMPROBE")) && *e) g_probe_mem = (uint32_t)atoi(e) ? 1u : 0u;   // ①-39
    if ((e = getenv("FFRDP_MAXCWND" )) && *e) { unsigned v = (unsigned)atoi(e); if (v >= 1 && v <= 4096) g_max_cwnd     = v; }
    if ((e = getenv("FFRDP_WAITSND"  )) && *e) { unsigned v = (unsigned)atoi(e); if (v >= 1 && v <= 8192) g_waitsnd      = v; }
    if ((e = getenv("FFRDP_UNSENT"   )) && *e) { unsigned v = (unsigned)atoi(e); if (v <= 8192) g_unsent = v; }  // ①-41：0 = 关
    if ((e = getenv("FFRDP_PSEUDO_NOCUT")) && *e) g_pseudo_nocut = atoi(e) ? 1 : 0;   // ①-28 B
    if ((e = getenv("FFRDP_HOLE_CUT_ONCE")) && *e) g_hole_cut_once = atoi(e) ? 1 : 0; // ①-29
    if ((e = getenv("FFRDP_SENDWAKE_COALESCE")) && *e) g_sendwake_coalesce = atoi(e) ? 1 : 0; // ①-35c
}

#define FFRDP_INFLIGHT_MIN   4

// the delay probe: take half the in-flight away for a moment and watch whether the round trip follows.
#define FFRDP_PROBE_HOLD_MS       500 // held for 2 round trips plus this much, long enough to drain a queue
#define FFRDP_PROBE_RTT_DROP_MS   2   // the typical round trip must fall by at least this many ms. Absolute, and
                                      // that is the whole point of the change: the rule used to be relative --
                                      // below 85% of the value in front of it -- and the value in front of it
                                      // was c->rttm, the LATEST sample rather than the typical one, so the
                                      // same experiment passed or failed depending on which single sample
                                      // happened to precede it. On the measured path the winning step is rtts
                                      // 18 -> 14ms, i.e. 4ms: 85% of 18 asks for 2.7ms and got it only when
                                      // the sample in front was high enough. A relative rule also cannot see
                                      // a queue that refuses to drain: 15% of a 5ms floor is far under the
                                      // jitter. What the experiment can actually answer is "did taking half
                                      // the frames away visibly lower the latency", which is a difference in
                                      // milliseconds -- so it is compared as one. The rate guard below is what
                                      // keeps an over-eager keep harmless, and the giveback undoes a stale one.
#define FFRDP_PROBE_RATE_KEEP_PCT 85  // ... while the rate the peer confirms stays above this percentage
// 2026-10-14 重开 ✓。当年关它的理由（"判据与地板是在**唤醒坏掉**的时期调出来的"）已经不存在：
// 唤醒那两条 bug 已修（send/recv 句柄竞态、cond 醒后重查），而今天的真机数据正好证明**探测的前提成立** ✓
// —— 不同链路的拐点天差地别（慢 AP 3~6 帧、快 AP 64 ✓），一个"自己走下来找拐点"的机制就是答案 ✓
// （三张网实测：probe32 无一处变差、两处明显变好 ✓，见 §26.16）。
// 已知不足，留着但它有安全网 ✓：keep 判据用的是 rtts 滑窗，在**突发型**链路上会踩空 ✗
// （2026-10-14 慢 AP 日志：probe rtt ref/best = 79/79 ✗ 而实际 227ms↔79ms 在跳）；
// 但速率守卫（FFRDP_PROBE_RATE_KEEP_PCT ✓）+ 按秒 giveback + 地板 32 三道网把它兜住了 ✓
// —— 实测没有一次误 keep 造成损失 ✓。判据口径的重审排在后面（把"RTT 降没降"换成"resend_perMB 降没降"）。
#define FFRDP_PROBE_GIVEBACK_IDLE_S 5 // giveback（把证明过的窗口还回去）要先"连续健康"这么多秒 ✓
                                      // 为什么：还的速度不能快过证明的速度 ✗ —— 探测每 PROBE_GAP(10s) 才问一次，
                                      // 而原实现每秒就能还 25% ⇒ 必然振荡。2026-10-15 慢 AP 实测：
                                      // cap 在 16~64 之间漂、rtts 77~116ms ✗；把 giveback 关掉当对照则稳在
                                      // 4 帧、rtts 9ms ✓✓（算术也对得上：4 帧⇒9ms、16⇒60、32⇒120 ✓）。
                                      // 见 notes §26.18。
#define FFRDP_PROBE_ENABLE          1
                                      // —— 把在途砍半，若 rtts 明显下降且确认速率没掉，就保留更小的上限（同吞吐、低延迟）。
                                      // 真机上它曾走过一次"单向棘轮"✗：参考取紧邻的前一秒，第一刀之后参考自己也掉 ⇒
                                      // 每一刀都通过 ⇒ 上限被一路 keep 到地板。现在三处安全网就位：
                                      //   1) 参考改成"探测前一秒"与"见过的最好一秒"里的较大者（rate_best，每秒衰减 1/16）
                                      //      ⇒ 一个 keep 必须让速率不低于历史最好值的 85%，砍过头会被拒；
                                      //   2) 按秒 giveback：被 keep 的上限把速率拖到最好值的 85% 以下 ⇒ 每秒翻倍还回去；
                                      //   3) 地板 INFLIGHT_MIN = 32（= INFLIGHT_MAX 的一半）⇒ 一次误 keep 也压不死管子。
                                      // 注意它是"延迟 ↔ 吞吐"的取向选择：2026-10-12 坏 AP 那次（RTT 0.3~2 s）它把上限
                                      // keep 到 32~64，吞吐因此更低，但 RTT 确实掉回 109ms —— 那是设计行为（§1.1 优先级②），
                                      // 不是故障。要"吞吐优先"就把这一行改成 0 重编（dist 里有 test_ffrdp_noprobe）。
                                      // lo 上它不触发（rtts ≈ rtt_min）⇒ 只有真机或 tools/sim 能验它；见 notes 26.9/26.14/26.15。
                                      // on by default: it is a net win wherever a queue is being held, and
                                      // where there is none the rate guard reverts it and nothing is lost.
                                      // it is also how a new path gets asked "is the delay ours?", so it is
                                      // left on rather than built specially for the question.
// the queue test: how far the typical round trip sits above the path's own floor is what decides whether an
// experiment is worth running. at 4x the probe stops asking while the excess is merely smaller, not gone:
// on the target path it converged to rtts 12~19ms over a 4ms floor, i.e. 3~4.75x, and the 4x gate then left
// it idle although a queue it could still have halved was there. 3x asks the question one step closer to
// the floor. being curious here stays safe because the answer must still pass the same two guards (the round
// trip must fall, the confirmed rate must hold) and a failed question doubles the gap -- so the cost is
// bounded by one half window per back off, never by a permanently smaller pipe.
#define FFRDP_PROBE_QUEUE_RATIO   3
#define FFRDP_PROBE_WASTE_PM      5   // retransmits per thousand first transmissions, over the last second
                                      // (the gate that used to sit here, "at least 8 frames on the wire", is gone:
                                      //  what earns an experiment is not a big pipe but a ceiling that can still
                                      //  be lowered, so the test is now "the halved ceiling is strictly under the
                                      //  one in force" against FFRDP_INFLIGHT_MIN's floor -- see below.)
                                      // two older gates -- "rttm >= 50ms" and "enough retransmits in the window" --
                                      // were replaced by the queue test and are gone.
#define FFRDP_PROBE_GAP_MS        10000
#define FFRDP_PROBE_GAP_MAX_MS    60000
#define FFRDP_PROBE_HEALTHY_PCT   1   // below this much waste the kept window is given back a little

#define FFRDP_RTO_FLOOR_MAX  1000 // ms: a longer repair delay than this is never worth paying
#define FFRDP_RTO_WIN        128  // retransmits per evidence window, so the loop reacts in about a second
#define FFRDP_WASTE_MIN      16   // retransmits a window must contain before it is allowed to decide anything
#define FFRDP_WASTE_HI       80   // % of our retransmits the peer already had => the rto is sitting too short
#define FFRDP_WASTE_LO       40   // % below which the repairs are real and the floor is given back

// ①-36（2026-10-15）：**128 → 96 帧**（186 KB → 140 KB）。它是**接收窗口的缓冲**
// （recv_wnd = (sizeof(recv_buff) - recv_size) / rmss ⇒ 就是我们在 ack 里通告给对端的窗口）。
// 判据：它必须**大于我方在途上限**（FFRDP_INFLIGHT_MAX = 64）并留余量，否则窗口反成瓶颈。
// 实测（lo，各两轮，应用队列 32 KB）：sizeof(conn) 260576 → **180064 B（-31%）**，
// 吞吐 40.9/37.9 → **38.2/39.0 MB/s**（同带），通告窗口 101~110 帧。
// 再往下的 80 帧（156 KB）吞吐也同带，但实测有一轮窗口掉到 66 帧（贴住 64）⇒ 不取。
#define FFRDP_RECVBUF_SIZE  (96 * (FFRDP_MAX_MSS + 0))
// 应用发送队列容量：编译期常量、内联在结构体里 ⇒ 没有 calloc，也没有"分配失败"这条路 ✓。
// 它只决定两件事：① 能发的**单条消息**上限 = 这个数 − 4（4 字节长度前缀，超过就返回 -1，不空等 ✓）；
// ② 背压何时出现（配小 ⇒ 更早出现，调用方按契约重试 ✓）。
// ①-41（2026-10-16，实验）：**应用队列（FFRDP_APP_TX_SIZE / app_tx_buff）已删除** ✓ ——
//   回到 v1.0/v1.1 的形状：应用线程在 ffrdp_send 里**直接成帧** ✓。省 32 KB/**连接** ✓。
//   它的"整形"职责由 `g_unsent`（未发出帧数预算 ✓）接管 ⇒ 见那里的注释 ✓。
                                  // （v1.1.0 里是 WAITSND×(MSS+4) = 374 KiB ✓）。WAITSND 现在只是"在途/受理"的帧数闸门，
                                  // 与这块纯应用侧缓冲无关；旧的乘法把两者捆在一起 ⇒ "省内存"必然"掉吞吐"，是个假取舍 ✗
                                  // （真机实测：WAITSND 64→256 ⇒ 两侧吞吐 ×3，而队列容量本可以不动 ✓）。对吞吐中性 ✓。
// how far ahead of the hole a received frame may sit before it is dropped instead of stored. The window
// we advertise travels in a one byte field of the ack, so it is never more than 255 frames, and a peer
// that respects its credit cannot be further ahead than that. Anything beyond it is a lie or a
// duplicate, and storing it is how one stranger - with no session, no handshake and one datagram per
// frame - grows this process's memory without limit (measured before this bound existed: 4000
// datagrams, 4000 frames kept).
#define FFRDP_RECV_QUEUE_MAX 256
// socket 的缓冲区大小、等待方式（select/usleep）都随 socket 一起搬进了 lowlevel：协议这一层不再有
// "等多久"这个东西，它只声明"我下次要在什么时刻被叫"（lowlevel.h 的 timer）。

// 应用侧等待时，即使调用方没给期限也要周期性醒来一次：这样 close 之类的状态变化不会被漏掉，也让
// 调用方看到一个"还在等"的循环，而不是一次永久睡眠。
#define FFRDP_COND_WAIT_MS   200

#define GET_FRAME_SEQ(f)        (*(uint32_t*)(f)->data >> 8)
#define SET_FRAME_SEQ(f, seq)   do { *(uint32_t*)(f)->data = ((f)->data[0]) | (((seq) & 0xFFFFFF) << 8); } while (0)

enum {
    FFRDP_FRAME_TYPE_FULL,       // full  frame
    FFRDP_FRAME_TYPE_SHORT,      // short frame
    FFRDP_FRAME_TYPE_FEC2,       // fec2  frame
    FFRDP_FRAME_TYPE_FEC32 = 32, // fec32 frame
    FFRDP_FRAME_TYPE_ACK   = 33, // ack   frame
    FFRDP_FRAME_TYPE_QUERY = 34, // query frame
};

typedef struct tagFFRDP_FRAME_NODE {
    struct tagFFRDP_FRAME_NODE *next;
    struct tagFFRDP_FRAME_NODE *prev;
    uint16_t size; // frame size
    uint8_t *data; // frame data
    #define FLAG_FIRST_SEND     (1 << 0) // after frame first send, this flag will be set
    #define FLAG_TIMEOUT_RESEND (1 << 1) // data frame wait ack timeout and be resend
    #define FLAG_FAST_RESEND    (1 << 2) // data frame need fast resend when next update
    uint32_t flags;        // frame flags
    uint32_t tick_1sts;    // frame first time send tick
    uint32_t tick_send;    // frame send tick
    uint32_t tick_timeout; // frame ack timeout tick
    uint32_t ticket;       // 首发时的 counter_ack_recv 快照：超时时用来分辨"等待期间 ack 到没到"。
                           // 不是临时调试：它是 timeout diag 与 rtt_tail 采样条件的依据，保留。
    // 2026-10-15（①-29）：这一帧把 cwnd 判减半了几次 ✓（`frame_node_new` 用 calloc ⇒ 天然从 0 起 ✓）
    uint8_t cc_cuts;
} FFRDP_FRAME_NODE;

// ── 标志位 ───────────────────────────────────────────────────────────────
    #define FLAG_CONNECTED (1 << 1)
    #define FLAG_FLUSH     (1 << 2)
    // (1 << 3) / (1 << 4)：v1 内置 AES 的 tx / rx 标志位，已随那套机制一起删除（位号不复用 ✓）
    #define FLAG_ACK_DUE   (1 << 5) // 收到过需要应答的帧、还没回；由 tick 在这一段结束时统一回一个

// ── 每连接状态（P1：一个进程内只有一条）────────────────────────────────────
// 这是 v1 那条单连接的全部协议状态。v2 起它就是 conn_list 里的 conn_ctx ✓
// （连接句柄 = 表下标 ✓）。字段顺序保持原样 ✓ —— ffrdp_dump 的清零跨度
// memset(&counter_send_bytes, 0, &reserved - &counter_send_bytes) 依赖它 ✓。
typedef struct {
    // ①-38（路线 B）：这两块**大缓冲**从"内嵌数组"改成"**按需分配**"（指针 + 容量）——
    // 为什么：服务端那条**内嵌主连接从不用于数据**（受理进来的连接从槽 1 起 ✓）⇒ 它的
    // recv_buff(96×1492≈140 KB) + app_tx_buff(32 KB，①-41 已删 ✓) 曾白占 ✗；改成按需之后
    // 服务端进程省下它，而**容量、窗口、队列行为一字不变** ✓（仍由下面那两个源码常量定 ✓）。
    uint8_t *recv_buff;   uint32_t recv_cap;   // 接收窗口缓冲（容量 = FFRDP_RECVBUF_SIZE）
    // 应用交给协议的数据（4 字节长度前缀 + 数据），协议在 tick 里取出并成帧。
    // 为什么要过这一道：成帧要碰的全是协议状态（cur_new_node / send_seq / 加密密钥），让应用线程直接
    // 去写它，就等于把协议的一半搬到应用线程上。放在这里之后，协议侧只被一个执行者碰；下一步协议搬
    // 到 lowlevel 的工作线程上时，这个队列就是两条线程之间唯一的界面（那时只需要给它加一把锁）。
    // ①-41（实验）：这里原来是一条 32 KB 的**应用队列** —— 已删除 ✓，省 32 KB/连接 ✓。
    // 应用线程现在直接成帧（ffrdp_send → ffrdp_frame_message ✓）⇒ "还没成帧的数据"这个状态不存在了 ✓。
    int32_t  recv_size, recv_head, recv_tail;
    uint32_t flags;
    uint64_t          peer_addr;     // 对端地址：客户端建连时从参数解出，服务端从第一个收到的包学会
    uint32_t          pub_wait_snd;  // 协议发布的在途帧数：应用的受理判据用它，而不是自己去看协议

    FFRDP_FRAME_NODE *send_list_head;
    FFRDP_FRAME_NODE *send_list_tail;
    FFRDP_FRAME_NODE *recv_list_head;
    FFRDP_FRAME_NODE *recv_list_tail;
    FFRDP_FRAME_NODE *cur_new_node;
    uint32_t          cur_new_size;
    uint32_t          cur_new_tick;
    uint32_t send_seq; // send seq
    uint32_t recv_seq; // send seq
    uint32_t wait_snd; // data frame number wait to send
    uint32_t tx_unsent; // ①-41：已成帧、但还没上过线（FLAG_FIRST_SEND 未置）的帧数 ✓
    uint32_t rttm, rtts, rttd, rto;
    uint32_t rmss, smss, swnd, cwnd, ssthresh;
    uint32_t peer_wnd_raw;        // 对端最近一次广告的窗口（夹之前的原值）：用来区分"我们夹的"与"对端给的"
    uint32_t counter_swnd0;       // 有帧等着首发、却没有信用的 tick 数：发送被饿死的直接证据
    uint32_t counter_capblock;    // 有帧等着首发、却被在途闸门挡住的 tick 数
    uint32_t rto_floor;           // persistent lower bound on the rto, moved only by the peer's duplicate reports
    uint32_t rtt_tail;            // 学到的"确认延迟尾部"：由"被重传过的帧最后被确认时的等待"喂入，每次增长限速
                                  // （首次 ≤64ms，之后 ≤ +50%+25ms），每秒衰减 1/16；让 RTO 覆盖 rtts/rttd 看不见的那一档。
    uint32_t win_need, win_dup;   // retransmits attempted in this evidence window, and how many the peer called pointless
    uint32_t tick_win;            // when the current evidence window was opened
    uint32_t hole_wait;           // ms a frame must be outstanding before a reported hole is believed
    uint32_t tick_recv_ack;
    uint32_t tick_send_query;
    uint32_t tick_rto_backoff; // tick of last rto backoff, rto backs off once per round

    uint8_t  fec_txbuf[4 + FFRDP_MAX_MSS + 2];
    uint8_t  fec_rxbuf[4 + FFRDP_MAX_MSS + 2];
    uint8_t  fec_txredundancy, fec_rxredundancy;
    uint16_t fec_txseq;
    uint16_t fec_rxseq;
    uint16_t fec_rxcnt;
    uint32_t fec_rxmask;
    // every sendto that did not take the frame, counted over the dump interval, whichever path it came
    // from: a data frame, an ack, a query, an fec frame. The ack/query/fec paths do not check their sendto
    // at all, so until this counter existed, an ack that could not be sent was invisible everywhere.
    // (A consecutive-failure count sat beside it and was dropped: resetting on the next success made a
    // socket that refuses one frame in ten read as zero at almost every sampling instant.)
    uint32_t counter_send_err;
    // 注意：这两个是【字节】计数，必须是 64 位。32 位版本累计到 4 GiB 就回绕，而 averg_* 拿
    // 回绕后的值去除以"已经过去的秒数"，打印出来就是突然掉到地板上的假速率 —— 实测 140s 处
    // total_send 4031MB、150s 处 207MB，而同一刻瞬时 confirmed rate 一直是 2.2 万帧/s。
    uint64_t counter_send_bytes;   // payload of the frames the peer acked: bytes DELIVERED, not bytes put on the wire
    uint64_t counter_recv_bytes;   // ... of the frames we acked
    uint32_t counter_send_1sttime; // first transmissions: the denominator of counter_resend_ratio
    uint32_t counter_send_failed;  // ffrdp_send refused the call (app queue full, or an allocation failed)
    uint32_t counter_send_query;
    // 2026-10-13 诊断：收帧→回 ack 的耗时（微秒）。服务端（对端）看到的 RTT 里含这一段。
    uint32_t ack_lat_n, ack_lat_avg_us, ack_lat_max_us;
    uint64_t ack_lat_sum_us;
    uint32_t counter_resend_fast;
    uint32_t counter_resend_rto;
    uint32_t counter_reach_maxrto;
    uint32_t counter_txfull , counter_rxfull ;
    uint32_t counter_recv_dup;   // data frames that arrived although we already had them
    uint32_t counter_ack_dup;    // ... as reported back to us by the peer: retransmits of ours that were pointless
    uint32_t dup_seen;           // duplicates seen since our last ack, the value that ack carries
    uint32_t counter_txshort, counter_rxshort;
    // fragments that were thrown away instead of being acted on, counted separately because they mean
    // different things: rx_bad is a fragment whose length cannot describe anything (it is malformed,
    // whoever sent it), recv_drop is a well formed frame that sat further ahead of the hole than the
    // window we advertise, so storing it would only spend our memory on someone else's lie.
    uint32_t counter_rx_bad;
    uint32_t counter_recv_drop;
    uint32_t counter_fec_tx;
    uint32_t counter_fec_rx;
    uint32_t counter_fec_ok;
    uint32_t counter_fec_failed;
    uint32_t counter_ack_recv   ; // ack frames received
    uint32_t counter_ack_mack   ; // ack frames carrying out of order (mack) info
    uint32_t counter_ack_hole   ; // ack frames reporting a gap below the highest acked frame
    uint32_t counter_recv_ooo   ; // data frames that arrived out of order: there is a hole below them
    uint32_t counter_recv_far   ; // ... and they are more than 23 ahead of the hole, so the 24 bit ack mask cannot carry them
    // how far ahead of the hole the out of order arrivals were, in the three bands that decide how wide
    // an ack mask would have to be: 1..24 is what today's 24 bit mask carries (bit dist-1, so dist 24 is
    // its last bit), 25..64 is what a 64 bit one would add, and beyond 64 not even that could speak.
    // Nothing acts on these: they exist so that the cost of widening the mask (both ends must be
    // upgraded) is only paid after the distribution says it would buy anything. Note that these bands
    // are one frame wider than counter_recv_far above, which counts dist > 23 -- dist 24 does fit in
    // the mask, so the old counter is a frame stricter than the thing it is describing.
    uint32_t counter_ooo_d1_24  ; // ... and the distance was 1..24 frames: the mask of today reaches these
    uint32_t counter_ooo_d25_64 ; // ... 25..64 frames: a 64 bit mask would reach these, this one cannot
    uint32_t counter_ooo_dgt64  ; // ... more than 64 frames: out of reach for a single extension word
    // ... and how far past 64 the tail actually goes. On a real link 8% of the out of order arrivals
    // sat beyond 64, while nothing did in the simulator (whose window is only 64 wide), so the width
    // of the extension has to be decided by this, not by the argument that the in-flight gate would
    // bound the distance -- it does not: a frame that the peer reports in its mask leaves our send
    // list while the hole stays open, so the sender keeps running ahead of the hole without limit.
    uint32_t counter_ooo_d65_128; // ... 65..128 frames: a second extension word would reach these
    uint32_t counter_ooo_dgt128 ; // ... more than 128 frames: out of reach of two words
    // the mask itself counted in bits instead of frames. counter_recv_ooo says how many out of order
    // frames arrived; bits_sent says how many of them we actually told the peer about, and a frame keeps
    // its bit in every later ack until the hole below it fills, so bits_sent >= ooo is the healthy
    // reading. Comparing bits_sent (the receiver's count) with bits_recv (the sender's) is the only way
    // to tell a report that was never made from one that was made and never arrived -- neither end can
    // see that on its own.
    uint32_t counter_mack_bits_tx;// mack bits we put on the wire
    uint32_t counter_mack_bits_rx;// mack bits the peer sent us
    uint32_t rtt_min;             // lowest round trip seen this interval: the path's floor, the number to judge latency by
    uint32_t inflight_used;       // frames actually on the wire (recounted every pass): what step29 limits

    uint32_t counter_rto_backoff; // times the rto was multiplied
    uint32_t counter_timeout_evt; // laps in which at least one frame timed out
    // 下面这几个量的是工作线程"每圈"（lap）的节奏。名字里的 upd 是旧版留下的：那时协议由应用每轮调
    // ffrdp_update 驱动，现在由 lowlevel 的 TIMER 回调驱动，量的是同一件事的另一种来源。
    uint32_t tick_last_upd  ;     // tick of the previous lap
    uint32_t counter_upd_cnt;     // number of laps
    uint32_t counter_upd_gap;     // sum of the gaps between laps, in ms
    uint32_t counter_upd_gap_max; // longest gap between two laps, in ms
    // 2026-10-15（①-5）：上一圈**发出去几帧** ✓。为什么缺这个数：真机服务端 656k 次 fire
    //（期限说"现在有活"）却只发了约 70k 帧 ✗ ⇒ 90% 的整 tick 一帧都发不出去 ✗，而
    // `ffrdp_next_due` 的 `armed_now` 判据却一直判"有活" ⇒ 两者并不真的对齐（同 §26.21 那类病 ✓）。
    // 纯计数 ✓：lap_sent_ref 是上一圈的快照（0 当哨兵 ✓），四个桶只加不清 ✓。
    uint32_t counter_lap_sent0;   // 上一圈一帧都没发出去 ✗（模型却要求"现在"）
    uint32_t counter_lap_sent1;   // 上一圈发出去 1 帧 ✓
    uint32_t counter_lap_sentN;   // 上一圈发出去 >= 2 帧 ✓
    uint32_t lap_sent_ref;        // 上一圈结束时的(首发+重传)计数快照（0 = 还没结算过 ✓）
    // 2026-10-15（①-26）：每个协议圈**干了多少活** + **为什么停** ✓。为什么缺这个数：§26.45 把两个
    // 候选（ack 时机 / RX 批上限）都排除之后，剩下的事实只有一条 —— 每圈只往线上放 ≈2 帧 ✗，而
    // `cwnd=128` / 在途上限 64 / `swnd` 充足 / 三个闸门全 0 ✓ ⇒ **分不出**是"没人喂进表"还是
    // "喂了却发不出去" ✗。四组桶各自独立 ✓，纯计数 ✓，不改行为 ✗。
    uint32_t counter_dr0, counter_dr1, counter_dr2, counter_drM;      // 这一圈 drain_tx 成帧了几帧（口径 = send_seq 差 ✓）
    uint32_t counter_ls0, counter_ls1, counter_ls2, counter_ls3, counter_ls5;  // 这一圈发出几帧
    uint32_t counter_sp_list, counter_sp_cwnd, counter_sp_cap, counter_sp_swnd0, counter_sp_other;  // 发送循环停在哪一条
    uint32_t counter_ll0, counter_ll2, counter_ll8, counter_llM;      // 圈末在途表长（0 / 1-2 / 3-8 / >=9 ✓）
    // 2026-10-15（①-28 A）：超时帧**距 ack 的 una 多远** ✓ + 在不在 ack 的 24 位位图里 ✓。
    // 为什么缺这个数：零丢包的 lo 上 100% 的超时是伪超时 ✓，但"为什么 ack 没覆盖它"没有数 ✗ ——
    // ack 报的是 una + 24 位位图（`ffrdp_apply_acks` 里 `for (i=23; ...)` ✓）⇒ 若窗口一大、帧跑到
    // una 之外 24 格，**位图就盖不住它** ✗，只能等 una 追上来 ✓。纯计数 ✓。
    uint32_t counter_to_d0_7, counter_to_d8_15, counter_to_d16_23, counter_to_d24_63, counter_to_dM;
    uint32_t counter_to_dmax;
    uint32_t counter_pseudo_nocut;   // ①-28 B："本来要减半、但被关掉了"的伪超时次数 ✓
    // ①-35b：每消息固定开销的账 —— 受理次数 / 真叫醒次数 / 被省掉的叫醒 ✓
    uint32_t counter_send_calls;
    uint32_t counter_send_wakes;
    uint32_t counter_send_wake_skip;
    uint32_t tx_woken;               // ①-35c："已经叫过、工作线程还没跑过" ✓（①-41：改由 tick 末尾清零 ✓）
    // 2026-10-15（①-29）：减半**按调用点**分账 ✓ + 同一帧被减了几次 ✓。为什么缺这个数：`cwnd-back`
    // 远多于三类触发事件（a_def 898 vs 68 ✓）⇒ 猜是"同一帧反复减半" ✗ ⇒ 必须让每个调用点自己报数 ✓。
    uint32_t counter_cc_hole;     // 洞等够久分支
    uint32_t counter_cc_hole_cut; // 其中**真的**调了 CC 的那几次（开了守卫后应远小于上一项 ✓）
    uint32_t counter_cc_rto;      // 真 RTO
    uint32_t counter_cc_pseudo;   // 伪超时这一档
    uint32_t counter_cut_multi;   // 同一帧第 2 次及以后被减半的次数 ✓
    uint32_t counter_cut_max;     // 单帧最多被减半几次 ✓
    // 2026-10-15（§26.30 步骤1-①）：应用侧被**哪一道**闸门拒掉 ✓。为什么缺这个数：只看
    // counter_send_failed 只能知道"被拒了"✗，分不出是帧数闸（FFRDP_MAX_WAITSND ✓）还是字节闸
    // （FFRDP_APP_TX_SIZE ✓）—— 而 §26.30 的第 1 步要判的正是"这两个常量是不是成了瓶颈"✓。
    // 纯计数 ✓，三条路径都是原来就 return 的那几条 ✓，一个字节的行为都不动 ✗。
    uint32_t counter_refuse_wnd;   // 帧数闸：ceil(len/smss) + pub_wait_snd > FFRDP_MAX_WAITSND
    uint32_t counter_refuse_bytes; // ①-41 起**退役**：应用队列没了 ⇒ 没有字节闸（恒 0 ✓，字段留给 dump 对齐）
    uint32_t counter_refuse_big;   // ①-41 起**退役**："单条消息上限"这条判据随队列一起消失（恒 0 ✓）
    // 2026-10-15（①-24）：**其余**拒绝出口的账。为什么缺这个数：上面三条 gate 之外，ffrdp_send 还有
    // 几条裸 return/goto（表槽已空 / closing / 服务端未连上 / 握手失败 / 握手未完成 ✗）——实测 burst=8
    // 时应用侧 refused 7046 ✓ 而三条 gate 里只有 queue-bytes 6996 ✓ ⇒ 对上了 ✓；但"没有计数"这件事
    // 本身让分账不安全 ✗（下一个人照样只能猜 ✓）。纯计数 ✓，一个字节行为不动 ✗。
    uint32_t counter_ref_closing;  // ffrdp->closing ⇒ CLOSED
    uint32_t counter_ref_notconn;  // 服务端、对端还没连上 ⇒ CLOSED
    uint32_t counter_ref_hsfail;   // 握手超时 ⇒ CLOSED
    uint32_t counter_ref_hsdown;   // 客户端、握手还没成 ⇒ EAGAIN
    // 2026-10-15（§26.30 步骤1-②）：信用侧 —— swnd 回补到"能再发一帧"要等多久 ✓。为什么缺这个
    // 数：counter_swnd0 只数"没信用的**次数**"✗，分不出那是一瞬间（下一圈 ack 就到 ✓）还是被饿住
    // 很久 ✗ —— 而"发送被饿死"正是 lo 与 v1.1 差距的候选根因 ✓。
    // 口径：从第一次"有帧等首发、却没有信用"起，到**真的首发出去一帧**为止 = 一个 episode ✓。
    uint32_t counter_swnd_wait_ep;  // episode 数（结算一次 +1）
    uint32_t counter_swnd_wait_ms;  // episode 时长之和（ms）
    uint32_t counter_swnd_wait_max; // 最长的一个 episode（ms）
    uint32_t tick_swnd_starve;      // 当前 episode 的起点（0 = 不在 episode 里 ✓）
    // 2026-10-15（§26.30 第 2 步 ④-14）：ack 的**覆盖数**与**来源** ✓。为什么缺这个数：v1.1 是
    // "一圈一个 ack"（把这一圈收到的帧一起报掉 ✓），本架构是"收到帧就当场回 ack"（为省一整圈的
    // 延迟 ✓，见 ffrdp_lowlevel_cb 里那句注释 ✓）⇒ 两条路差多少、到底谁在回，一直是估的 ✗。
    // 覆盖数口径：这条 ack 距上一条 ack 之间**到达的**全帧数（counter_rxfull 的差 ✓，含重复 ✓）。
    uint32_t counter_ack_sent;     // 真的交给内核的 ack 帧数 ✓（分母 ✓）
    uint32_t counter_ack_ondata;   // 其中"收到帧就在 RECV 回调里当场回"的 ✓
    uint32_t counter_ack_ontick;   // 其中从 TIMER 那一支回的（整段 + ack-only ✓）
    uint32_t counter_ack_cov0;     // 这条 ack 覆盖 0 个新到达的数据帧（只有 QUERY / 重复 ✓）
    uint32_t counter_ack_cov1;     // 1 个 ✓（= "一帧一个 ack" ✓）
    uint32_t counter_ack_cov2;     // 2 个 ✓（真攒上了 ✓）
    uint32_t counter_ack_covN;     // >= 3 个 ✓
    uint32_t rx_frames_ref;        // 上一条 ack 时的 counter_rxfull 快照 ✓
    // 2026-10-15（§26.30 第 2 步 ④-15）：在途闸门"紧不紧" ✓。为什么缺这个数：真机 server 的
    // cap-block 占 98.4% 圈 ✗，但**光看次数分不出**"贴着上限跑（⇒ 门就是工作点 ✓）"还是
    // "上限远高于实际在途、只是偶尔碰到（⇒ 门根本没在干活 ✓）" ✗ ⇒ 只能量**占用率**。
    // 口径：每圈重数在途量时同时累加"在途量"与"当圈上限"（两个和 + 一个计数 ⇒ 均值对均值 ✓）。
    uint32_t counter_inflight_sum; // Σ 每圈在途帧数 ✓
    uint32_t counter_limit_sum;    // Σ 每圈生效的上限（MAX(inflight_max, INFLIGHT_MIN) ✓）
    uint32_t counter_inflight_n;   // 圈数（分母 ✓）
    uint32_t counter_capb_floor;   // 被挡时上限**在地板上**（<= FFRDP_INFLIGHT_MIN ⇒ 探测压到底了 ✗）
    uint32_t counter_capb_above;   // ... 在地板之上 ✓
    uint32_t counter_capb_nosend;  // 被挡的圈里，**一帧都没出去**的圈数（= 闸门直接掐住 ✓ 最硬的证据 ✓）
    uint32_t cap_ref;              // 上一圈结算时的 counter_capblock 快照 ✓
    uint32_t counter_cap_wait_ep;  // "被闸门挡住的帧"等多久才真出去（episode ✓）
    uint32_t counter_cap_wait_ms;  // ... 之和（ms ✓）
    uint32_t counter_cap_wait_max; // ... 最长（ms ✓）
    uint32_t tick_cap_starve;      // 当前 episode 的起点（0 = 不在 episode 里 ✓）
    // 2026-10-15（①-6）：TIMER 分支到底走了哪一支、期限模型给的是哪一档 ✓。为什么缺这个数：
    // 真机 server fire÷收帧 ≈ 6.6 ✗，而回环只有 1.3 ✓ ⇒ 不是这条路径本身有问题（它是故意的优化 ✓），
    // 而是真机上有东西在**反复把期限设成"现在"** ✗ ⇒ 只能在这一支里按来源分档量 ✓。纯计数 ✓。
    uint32_t counter_fire_ackonly;// 只回 ack 那一支（不跑整圈 ⇒ 不进协议圈数 ✓）
    uint32_t counter_fire_tick;   // 跑整圈那一支 ✓
    uint32_t counter_fire_d0;     // ffrdp_next_due 给的是 0（眼下没别的事 ✓）
    uint32_t counter_fire_dnow;   // 给的是 <= now（要求"现在"✗ ⇒ 这一档就是自旋源 ✓）
    uint32_t counter_fire_dnear;  // 给的是 now+1..4ms
    uint32_t counter_fire_dfar;   // 给的是 > now+4ms ✓（正常 ✓）
    uint32_t counter_upd_cost_max;// longest time spent inside one lap, in ms
    uint32_t counter_ack_una_dup; // ack frames whose una is the same as the previous one: the peer reports no progress
    uint32_t counter_rto_head ;   // rto retransmits of the send list head, the frame that blocks the stream
    uint32_t counter_rto_chain;   // rto retransmits of a frame queued behind the head
    uint32_t counter_rto_stale;   // rto retransmits of a frame the peer had already reported as received
    uint32_t counter_rto_behind;  // rto retransmits of a frame the peer had not reported yet
    uint32_t ack_una;             // last una seen in an ack frame
    // 2026-10-12 真机 A/B 后加的三个判据。真机上 counter_capblock 三组全是 0（在途闸门从未挡过），
    // 而吞吐确实随窗口 ×3，所以嫌疑落在另外两处：drain 的早退闸（它也随同一个宏缩放、却一直没有
    // 计数器），以及"乱序被当成丢包"把 cwnd 打回。前者按 tick 计，后者按事件计：
    uint32_t counter_drain_block; // drain 因 wait_snd >= MAX_CWND_SIZE 而少喂一轮的 tick 数
    uint32_t counter_cwnd_back;   // cwnd 被 RTO/快速重传打回的次数（乱序误判的直接证据）
    uint32_t counter_cwnd_sfail;  // ... 被"内核没收下这一包"打回的次数（发送缓冲压力）
    // 2026-10-15（B 案前置）：cwnd 已经塌在下限时再挨一刀，`ssthresh = MAX(cwnd/2, MIN)` 会把它
    // 算成 1 ✗ ⇒ `cwnd < ssthresh` 永远不成立 ⇒ 慢启动那一档用不上，此后只剩 +1/ACK 一档。真机
    // dump 里 cwnd<=2 的档位 14/277（≈5%，几乎都在 server 侧 ✓）⇒ 现象确实在发生 ✓；但它"贵不贵"
    // 要看爬回去多久（t_cwnd 量这个 ✓）⇒ 这里先只计数，一个字节的行为都不动 ✓。
    uint32_t counter_ssth_floor;  // ssthresh 被算到下限（1）的次数：此后只能 +1/ACK 爬
    // 2026-10-15（lo 调查 ①-2）：lowlevel 报"worker 空转"（期限=现在 ✗）时，协议这边到底
    // 把期限设成了什么。三种"要求现在"里，只有第三种是纯粹的空转 ✗：
    uint32_t counter_now_src[8];  // 只读（§26.38 ①-19）：**出口口径** —— next_due 返回值 <= now 时按源记 ✓
                                  // 为什么缺：①-2 只记了"stale（把过期的改写成 now）"那一种 ✗ ⇒
                                  // 收尾期那 1.25M 次 `<=now` 的 arm 找不到源 ✗（①-18 把矛盾压到了
                                  // "tick 之前 vs tick 之后"两次 next_due 之间 ✓ ⇒ 这里两端都记 ✓）。
    uint32_t counter_due_now_ack;    // ① 欠着对端一个 ack ⇒ 这一刻就该回（合法 ✓）
    uint32_t counter_due_now_work;   // ② 真有活可干（有队可发/有帧可首发 ✓）
    uint32_t counter_due_now_stale;  // ③ **没活可干却要求"现在"** ✗ ⇒ 这一项就是自旋来源
    uint32_t counter_due_future;     // 正常：期限落在未来 ✓
    // STALE 时是哪个期限源赢的（7 个候选按下面的顺序 ✓）；[0] 未分类 ✓
    uint32_t counter_stale_src[8];
    // 2026-10-15（①-3：护栏盲点）：上面三档都只看**护栏之后**的 due ✗ ⇒ 被护栏从
    // "现在"改写成"1ms 后"的那些，全被记成 future ✗ —— 而真机服务端那 8,969 圈/秒正是它们
    // （lowlevel-udp.c:232-237：一圈活超过 1ms ⇒ now+1 已过期 ⇒ left=0 ⇒ 实睡 0ms ✗）。
    // 所以护栏那一支要单独记，并记住是哪个源赢的 ✓。纯计数，不改行为 ✗。
    uint32_t counter_arm_eq_now;     // 护栏把"正好现在"改写成"1ms 后"的次数 ✗
    uint32_t counter_arm_eq_src[8];  // 上面那次的源（顺序同 counter_stale_src ✓）
    uint32_t counter_arm_le2ms;      // 记账后期限离现在 <= 2ms 的次数（真·自旋指标 ✓）
    // 2026-10-15（A 案收尾）：把"被重传过的帧**最后**被确认时等了多久"（conf = now - tick_send ✓）
    // 的**原始**分布记下来。为什么缺这个数：rtt_tail 只留了**限速后**的值 ✗、maxwait 只留了
    // "超时瞬间"的年龄 ✗ ⇒ 两者都分不清"ack 真的迟到"与"洞挡在前面、对端没法确认"✓。
    // 纯计数：不参与任何判据 ✗、不改控制流 ✗（只读诊断 ✓）。
    uint32_t counter_conf_16, counter_conf_32, counter_conf_64, counter_conf_128;
    uint32_t counter_conf_256, counter_conf_512, counter_conf_1024, counter_conf_more;
    uint32_t counter_conf_max;    // 见过的最大原始 confirm 等待（ms ✓）
    // 2026-10-12 真机 n_*.log：33~55 次/秒的伪超时（resend_waste 99.7~100%），每次都把 cwnd
    // 打到 MIN。要修就得先知道它属于哪一种：等待期间一个 ack 都没到（ack 路径停摆/ack 丢了），
    // 还是收到过 ack 却没覆盖这一帧（洞上方的帧无法被确认，只能等洞补上）。纯计数，不改行为。
    uint32_t counter_to_noack;    // 超时时：自首发以来一个 ack 都没收到过
    uint32_t counter_to_hadack;   // 超时时：收到过 ack，但没有覆盖这一帧
    uint32_t counter_to_age_max;  // 超时时"距首发"的最大毫秒数
    uint32_t hs_state;            // 握手状态（HS_*；0 = 不走握手）—— 放 reserved 之后 ⇒ 不动清零跨度 ✓
    uint32_t tick_last_recv;      // 最后一次**听到对端**（任意帧 ✓）—— keepalive 判活用（同样放 reserved 之后 ✓）
    uint32_t hs_tick;             // 上次发握手帧的时刻（重传计时）
    uint32_t hs_start;            // 本次握手开始（或上次重传）的时刻：与 handshake_ms 比就是超时
    uint32_t hs_retry;            // 已重传次数
    uint32_t counter_rx_foreign;  // 来源地址不是本连接对端的帧：按地址分发后应当只有"别人的包"
    uint32_t counter_hs_refused;  // 握手 SYN 没能建连接（表满/内存不足）：设计稿要求"不静默"✓
                                  // （P2 第二步新增 ⇒ 放在 reserved 之后：不参与 dump 的清零跨度、也不打印 ⇒ 输出不变 ✓）
    uint32_t reserved;            // ffrdp_dump clears everything up to here: nothing that must outlive a
                                  // dump, and nothing a decision depends on, may sit above this line
                                  // (the probe would otherwise be reset exactly when it is about to run).
    // ---- delay probe (step28): is the delay we see our own queue, or the path's? ----
    // No observation can tell those apart: the lowest round trip ever seen already contains a queue
    // that never drains, so the delay-versus-minimum comparison that every delay based scheme uses is
    // blind exactly where it is needed. The only way to know is an experiment -- take half the frames
    // out of flight and see whether the delay follows -- and the only reason it is safe to run is that
    // a link that queues and a link that loses answer differently: if the delay falls while the rate
    // the peer confirms does not, the queue was ours; if the delay does not fall, the delay belongs to
    // the path and the window goes straight back. Needs nothing from the peer.
    uint32_t probe_state;         // 0 idle, 1 probing
    uint32_t probe_tick;          // when the current probe started
    uint32_t probe_hold;          // how long the reduced window is held
    uint32_t probe_cap;           // the reduced in-flight limit under test
    uint32_t probe_rtt_ref;       // lowest round trip just before the probe
    uint32_t probe_best;          // lowest round trip seen during the probe
    uint32_t probe_del_ref;       // delivered frames at the start of the probe
    uint32_t probe_res_ref;       // 重传总数（rto+fast）在探测开始时的快照：用来量"重传强度变没变" ✓
    uint32_t giveback_idle;       // 连续健康的秒数（giveback 要先攒够 FFRDP_PROBE_GIVEBACK_IDLE_S ✓）
    uint32_t last_giveback;       // 上一次 giveback 的时刻：频率不得高于探测周期 ✓
    uint32_t probe_waste_ref;     // 探测前一刻的 probe_waste（每千首发帧的重传数 ✓）：与之相比
    uint32_t probe_rate_ref;      // frames per second the peer confirmed just before the probe
    uint32_t rate_best;           // best confirmed rate ever seen (decays 1/16 per second): the reference a keep must survive
    uint32_t probe_gap;           // current back off between probes
    uint32_t probe_next;          // earliest tick for the next probe
    uint32_t cap, cap_kept;       // in-flight limit enforced now, and the one a probe proved
    uint32_t inflight_max;        // the frames-on-the-wire ceiling actually used (0 = FFRDP_INFLIGHT_MAX)
    uint32_t rate_tick, rate_del_ref, rate_now; // delivered frames per second, sampled once a second
    uint32_t probe_waste;         // retransmits as a percentage of first transmissions over the last second
    uint32_t probe_resend_seen, probe_first_seen; // ... the raw counts that percentage is taken against
    uint32_t counter_delivered;   // frames the peer has confirmed and we have let go of
    uint32_t counter_probe, counter_probe_keep, counter_probe_revert;
    uint32_t counter_probe_mem;   // ①-39：其中由"上限定得比实际需要高"触发的次数 ✓
    uint32_t counter_probe_narrow, counter_giveback;
} FFRDPCONN;

// ── 进程级状态 ───────────────────────────────────────────────────────────
// 一个传输层、一条接收线程、一把锁：这里的字段必须与"有几条连接"无关 ✓
typedef struct {
    // ---- 上层边界 ----
    // 协议与应用线程只在下面这些东西上见面：一个发送字节队列（应用写、协议读）、一个接收字节队列
    // （协议写、应用读）、以及几个"协议发布出来给应用看"的镜像量。协议自己的状态（send_list /
    // inflight / rto / 各个计数器……）仍然只被一个执行者碰 —— 这正是这次搬家不需要给协议加锁的原因。
    FFRDP_LOWLEVEL   *lowlevel;      // 传输层：socket/收包/定时都在它自己的工作线程里
    int               closing;       // 应用说要关：让等待中的应用线程醒来并返回（全部访问都在锁内，不需要原子类型）
    FFRDP_CALLBACK    app_cb;        // 应用回调（v2 起：ACCEPT/CONNECT/DISCONN/ENCRYPT/DECRYPT/RECVDATA）
    void             *app_cbctx;
    int               maxconn;       // 配置：最大连接数（"maxconn:N"，默认 FFRDP_MAXCONN）
    int               handshake_ms;  // 配置：握手超时（"handshake:N"，默认 10000）
    int               idle_ms;       // 配置：空闲判死超时（"idle:N"，默认 5000）
    int               hs_enable;     // 配置：是否走三次握手（"hs:1" 开 / "hs:0" 关，**默认 1** ✓；
                                     // hs:0 才是与 v1 线上行为一致的那条路 ✓）
    int               is_server;     // 角色（v1 的 FLAG_SERVER 搬到进程级：它是"这个进程"的属性，不是连接的 ✗）
    int               next_slot;     // 连接表里下一个可用的下标（**永不复用** ✓，见 ffrdp.h 的设计说明）
    pthread_mutex_t   lock;          // 保护 recv_buff+recv_size/head/tail / send_list / cur_new_* / peer_addr / flags / 上列几个
                                     // （①-41：app_tx_* 已随应用队列一起删除 ✓）
    pthread_cond_t    cond;          // 协议侧腾出空间或送来数据时叫醒应用侧

    uint32_t   tick_ffrdp_dump;
    // 2026-10-15（①-9）：回调入口分档 ✓。为什么缺这个数：回环实测低层 fire 131 万 ✗，
    // 而表里两条连接的 TIMER 段各只跑了 5.6 万 ✓ ⇒ 多出的 126 万次回调**没有进入**
    // "按连接遍历"那一段 ✗ ⇒ 只能在这里按类型与入口时的 nconn 分开量 ✓。纯计数 ✓。
    uint32_t   cb_timer_calls;    // 入口 type==TIMER ✓
    uint32_t   cb_data_calls;     // 入口 type==RECV ✓
    uint32_t   cb_other_calls;    // 其他类型 ✓
    uint32_t   cb_nconn_zero;     // 入口时 nconn==0 ✗（那一段循环体一次都不跑 ✓）
    uint32_t   cb_nconn_sum;      // 入口时 nconn 之和 ✓（配 timer_calls 看平均几条 ✓）
    // 2026-10-15（§26.38 ①-18）：**中间那一跳**的账 ✓。为什么缺：每连接的 due 有账（①-2 / ①-3 ✓）、
    // 低层收到的 arm 值也有账（①-17 ✓），唯独"`ffrdp_arm_timer` 取 min 之后到底是多少、是哪条连接
    // 赢的"**没有账** ✗ ⇒ 收尾期那 17 万次/s 的 `≤now` arm（①-17 ✓）就卡在这一跳上 ✗。
    uint32_t   arm_calls;         // arm_timer 被调用的次数（应 ≈ TIMER 回调数 ✓）
    uint32_t   arm_d0;            // 算出的 due == 0（没有期限 ✓）
    uint32_t   arm_dnow;          // due <= now（真的要求"现在" ✗）
    uint32_t   arm_dnear;         // now+1..4
    uint32_t   arm_dfar;          // > now+4
    uint32_t   arm_win[FFRDP_MAXCONN];   // 是哪条连接的 x 赢了（赢家就是"要现在"的那条 ✗）
    uint32_t   arm_ack_win;       // ACK_DUE 覆盖赢的次数 ✓
    int        arm_samp;          // 只读（①-20）：收尾态采样打印的条数（8 条够用 ✓）
    uint32_t   tick_ffrdp_init;   // tick of ffrdp_init, for the elapsed time in the dump
    // ①-24：在"查表"之前就被拒的两条（还没有 c ⇒ 只能记在上下文上 ✓）
    uint32_t   stat_ref_noarg;    // !ffrdp / !buf / len<=0 ⇒ EINVAL
    uint32_t   stat_ref_noconn;   // conn 越界 / 表槽空 ⇒ CONN_INVALID
    FFRDPCONN  conn;   // P1：内嵌的主连接（第一步的表 0 号就指向它 ⇒ 内存/行为与 P1 一致 ✓）

    // ── 连接表（P2 第一步）──────────────────────────────────────────────────
    // 连接句柄 = 下标 ✓；lowlevel 收到帧后按对端地址查这张表 ✓（v2 的分发点）。
    // 第一步只启用 0 号（指向上面的内嵌主连接 ✓）⇒ 还没有任何行为变化 ✓、内存也不涨 ✓；
    // 第二步起才谈"多条连接要不要预分配固定缓冲"（那是内存代价，要先定 ✓）。
    FFRDPCONN *table[FFRDP_MAXCONN];
    int        nconn;
} FFRDPCONTEXT;

static uint32_t ringbuf_write(uint8_t *rbuf, uint32_t maxsize, uint32_t tail, uint8_t *src, uint32_t len)
{
    uint8_t *buf1 = rbuf + tail;
    int      len1 = MIN(maxsize - tail, len);
    uint8_t *buf2 = rbuf;
    int      len2 = len  - len1;
    memcpy(buf1, src + 0   , len1);
    memcpy(buf2, src + len1, len2);
    return (tail + len) % maxsize;
}

static uint32_t ringbuf_read(uint8_t *rbuf, uint32_t maxsize, uint32_t head, uint8_t *dst, uint32_t len)
{
    uint8_t *buf1 = rbuf + head;
    int      len1 = MIN(maxsize - head, len);
    uint8_t *buf2 = rbuf;
    int      len2 = len  - len1;
    if (dst) {
        memcpy(dst + 0   , buf1, len1);
        memcpy(dst + len1, buf2, len2);
    }
    return (head + len) % maxsize;
}

static int seq_distance(uint32_t seq1, uint32_t seq2) // calculate seq distance
{
    int c = seq1 - seq2;
    if      (c >=  0x7FFFFF) return c - 0x1000000;
    else if (c <= -0x7FFFFF) return c + 0x1000000;
    else return c;
}

static FFRDP_FRAME_NODE* frame_node_new(int type, int size) // create a new frame node
{
    FFRDP_FRAME_NODE *node = calloc(1, sizeof(FFRDP_FRAME_NODE) + 4 + size + (type <= FFRDP_FRAME_TYPE_SHORT ? 0 : 2));
    if (!node) return NULL;
    node->size    = 4 + size + (type <= FFRDP_FRAME_TYPE_SHORT ? 0 : 2); // 4 bytes header + data size + 2 bytes fec seq
    node->data    = (uint8_t*)node + sizeof(FFRDP_FRAME_NODE);
    node->data[0] = type;
    return node;
}

static int frame_payload_size(FFRDP_FRAME_NODE *node) {
    return node->size - 4 - (node->data[0] <= FFRDP_FRAME_TYPE_SHORT ? 0 : 2);
}

static void list_enqueue(FFRDP_FRAME_NODE **head, FFRDP_FRAME_NODE **tail, FFRDP_FRAME_NODE *node)
{
    FFRDP_FRAME_NODE *p;
    uint32_t seqnew, seqcur;
    int      dist;
    if (*head == NULL) {
        *head = node;
        *tail = node;
    } else {
        seqnew = GET_FRAME_SEQ(node);
        for (p = *tail; p; p = p->prev) {
            seqcur = GET_FRAME_SEQ(p);
            dist   = seq_distance(seqnew, seqcur);
            if (dist == 0) { free(node); return; } // duplicated frame, drop it and free memory
            if (dist >  0) {
                if (p->next) p->next->prev = node;
                else *tail = node;
                node->next = p->next;
                node->prev = p;
                p->next    = node;
                return;
            }
        }
        node->next = *head;
        node->next->prev = node;
        *head = node;
    }
}

static void list_remove(FFRDP_FRAME_NODE **head, FFRDP_FRAME_NODE **tail, FFRDP_FRAME_NODE *node)
{
    if (node->next) node->next->prev = node->prev;
    else *tail = node->prev;
    if (node->prev) node->prev->next = node->next;
    else *head = node->next;
    free(node);
}

static void list_free(FFRDP_FRAME_NODE **head, FFRDP_FRAME_NODE **tail)
{
    while (*head) list_remove(head, tail, *head);
}

// 原来这里有一个 ffrdp_sleep：select(udp_fd, 10ms) + FLAG_FLUSH 短路。它整个搬进了 lowlevel ——
// "等 socket 可读、按时醒来、立刻发"这三件事现在是传输层的实现，协议只说"下次在 X 时刻叫我"。
// 这不只是搬家：非 fd 的后端（串口、隧道）现在也能等了，而那正是"底层可以不是 UDP"的关键。
static int ffrdp_send_data_frame(FFRDPCONTEXT *ffrdp, FFRDPCONN *c, FFRDP_FRAME_NODE *frame)
{
    switch (frame->size - c->smss) {
    case 6 : c->counter_fec_tx ++; *(uint16_t*)(frame->data + 4 + c->smss) = c->fec_txseq++; break; // tx fec frame
    case 4 : c->counter_txfull ++; break; // tx full  frame
    default: c->counter_txshort++; break; // tx short frame
    }
    // 0 = 传输层收下了这一帧；-1 = 它没收（非阻塞发送缓冲满、或链路报错）。协议不区分这两者：帧已经
    // 不在它手里了，丢没丢由上面的重传负责 —— 与旧版 sendto 失败的语义一致。
    if (ffrdp->lowlevel->send(ffrdp->lowlevel, c->peer_addr, (uint8_t*)frame->data, (int)frame->size) != 0) { c->counter_send_err++; return -1; }
    if (frame->size == 4 + c->smss + 2) { // fec frame
        uint32_t *psrc = (uint32_t*)frame->data, *pdst = (uint32_t*)c->fec_txbuf, i;
        for (i = 0; i < (4 + c->smss) / sizeof(uint32_t); i++) *pdst++ ^= *psrc++; // make xor fec frame
        if (c->fec_txseq % c->fec_txredundancy == c->fec_txredundancy - 1) {
            *(uint16_t*)(c->fec_txbuf + 4 + c->smss) = c->fec_txseq++; c->fec_txbuf[0] = c->fec_txredundancy;
            // count it like any other send: a redundancy frame the socket refused is a repair that will
            // not happen, and it used to leave no trace anywhere.
            if (ffrdp->lowlevel->send(ffrdp->lowlevel, c->peer_addr, (uint8_t*)c->fec_txbuf, (int)frame->size) != 0) c->counter_send_err++; // send fec frame
            memset(c->fec_txbuf, 0, sizeof(c->fec_txbuf)); // clear tx_fecbuf
            c->counter_fec_tx++;
        }
    }
    return 0;
}

static int ffrdp_recv_data_frame(FFRDPCONTEXT *ffrdp, FFRDPCONN *c, FFRDP_FRAME_NODE *frame)
{
    uint32_t fecseq, fecrdc, *psrc, *pdst, type, i;
    // The length below is not just a number: the payload size of the peer (rmss) is learned from it, and
    // rmss is used as a divisor (the window this ack advertises) and as a copy length (the bytes handed
    // to the application). A fragment shorter than its own overhead therefore does not merely look odd:
    // a four byte "full" frame makes rmss zero (divide by zero, SIGFPE), a four byte "fec" frame -- whose
    // overhead is six -- makes it negative, which underflows to four billion and reaches memcpy as a
    // length (SIGSEGV). Both were reachable with a single datagram sent by anyone who can reach the port,
    // and both are what tools/sim/t_fuzz.c exists to keep out.
    switch (frame->data[0]) {
    case FFRDP_FRAME_TYPE_SHORT: c->counter_rxshort++; return 0; // short frame
    case FFRDP_FRAME_TYPE_FULL :
        if (frame->size <= 4) { c->counter_rx_bad++; return -1; }  // header only: no payload at all
        c->counter_rxfull ++; c->rmss = frame->size - 4; return 0; // full frame
    default:
        if (frame->size <= 6) { c->counter_rx_bad++; return -1; }  // header + fec seq, no payload
        c->counter_fec_rx ++; c->rmss = frame->size - 6; break;// fec  frame
    }
    fecseq = *(uint16_t*)(frame->data + frame->size - 2);
    fecrdc = frame->data[0];
    if (fecseq / fecrdc != c->fec_rxseq / fecrdc || c->fec_rxredundancy != fecrdc) { // group changed or fec_rxredundancy changed
        memcpy(c->fec_rxbuf, frame->data, frame->size);
        c->fec_rxseq = fecseq; c->fec_rxredundancy = fecrdc;
        if (fecseq % fecrdc == fecrdc - 1) {
            // ⚠️ 这一帧**就是冗余帧** ⇒ 它不能算成"收到一个数据帧" ✗：此刻组里收到的数据是 0 个 ✓。
            //    旧写法把它记成 rxcnt = 1 并直接 return -1 ⇒ 组里只剩一个数据帧时（rdc == 2 ✓）
            //    那一帧永远补不回来 ✗（实测：fec:2 的冗余等于白搭 ✓，现在由 t_fec 阶段 C 钉住 ✓）。
            c->fec_rxmask = 0;
            c->fec_rxcnt  = 0;
        } else {
            c->fec_rxmask = 1 << (fecseq % fecrdc);
            c->fec_rxcnt  = 1;
        }
        // 这里**不再 return**：接着走下面的"冗余帧恢复 / 数据帧累积" ✓（rdc == 2 正是靠这条才恢复 ✓）
    } else c->fec_rxseq = fecseq; // group not changed
    if (fecseq % fecrdc == fecrdc - 1) { // it's redundance frame
        if (c->fec_rxcnt == fecrdc - 1) return -1;
        if (c->fec_rxcnt != fecrdc - 2) { c->counter_fec_failed++; return -1; }
        type = frame->data[0];
        // rxcnt == 0（只有 rdc == 2 到得了这儿 ✓）：组里一个数据帧都没收到 ⇒ 冗余帧**本身就是**
        // 那一帧 ✓（异或空集 = 原样 ✓）⇒ 不能再拿它跟自己异或一遍（那是全 0 ✗）。
        if (c->fec_rxcnt > 0) {
            psrc = (uint32_t*)c->fec_rxbuf; pdst = (uint32_t*)frame->data;
            for (i = 0; i < (frame->size - 2) / sizeof(uint32_t); i++) *pdst++ ^= *psrc++;
        }
        frame->data[0] = type;
        c->counter_fec_ok++;
    } else if (!(c->fec_rxmask & (1 << (fecseq % fecrdc)))) { // update fec_rxbuf
        psrc = (uint32_t*)frame->data; pdst = (uint32_t*)c->fec_rxbuf;
        for (i = 0; i < (frame->size - 2) / sizeof(uint32_t); i++) *pdst++ ^= *psrc++;
        c->fec_rxmask |= 1 << (fecseq % fecrdc); c->fec_rxcnt++;
    }
    return 0;
}

static long ffrdp_lowlevel_cb(void *cbctx, int type, void *buf, int len);  // lowlevel 的工作线程从这里回到协议
static void ffrdp_dump_conn(FFRDPCONTEXT *ffrdp, FFRDPCONN *c, int handle, int clearhistory); // 按连接 dump；ffrdp_dump_locked 逐连接调用
static int  ffrdp_hs_send(FFRDPCONTEXT *ffrdp, FFRDPCONN *c, uint8_t sub); // 握手：定义在 lowlevel 回调之前
static void ffrdp_arm_timer(FFRDPCONTEXT *ffrdp);                          // 定义在协议后段（init 里要用 ✓）
static int  ffrdp_conn_handle(FFRDPCONTEXT *ffrdp, FFRDPCONN *c);
static long ffrdp_emit(FFRDPCONTEXT *ffrdp, int type, int conn, uint64_t addr, void *buf, int len); // 事件出口

// 一条连接的默认值：主连接与"按需分配的新连接"共用同一套（方案 A：每连接一个 calloc ✓，
// 每连接仍用固定大小缓冲 ✓ —— 与 ffrdp.h 的设计说明 的"第一阶段不优化"一致 ✓）。
// ①-38：两块大缓冲的按需分配 / 归还。容量仍取那两个源码常量 ⇒ 窗口与队列行为一字不变 ✓。
// 幂等 ✓：已经在的就不重复分配 ✓（服务端 hs:0 的旧路径要"用时再分配"，见 lowlevel_cb ✓）。
static int ffrdp_conn_bufs(FFRDPCONN *c)
{
    if (!c->recv_buff) {
        if (!(c->recv_buff = calloc(1, FFRDP_RECVBUF_SIZE))) return -1;
        c->recv_cap = FFRDP_RECVBUF_SIZE;
    }
    return 0;
}
static void ffrdp_conn_bufs_free(FFRDPCONN *c)
{
    if (c->recv_buff)   { free(c->recv_buff);   c->recv_buff   = NULL; c->recv_cap   = 0; }
}

static void ffrdp_conn_init(FFRDPCONN *c, int smss, int sfec)
{
    c->swnd     = FFRDP_DEF_CWND_SIZE;
    c->cwnd     = FFRDP_DEF_CWND_SIZE;
    c->ssthresh = FFRDP_DEF_CWND_SIZE;
    c->rtts     = (uint32_t) -1;
    c->rto      = FFRDP_MIN_RTO;
    c->rmss     = FFRDP_MAX_MSS;
    c->smss     = MAX(1, MIN(smss, FFRDP_MAX_MSS));
    c->fec_txredundancy = MAX(0, MIN(sfec, FFRDP_FRAME_TYPE_FEC32));
    // ①-38：**这里不分配大缓冲** ✗ —— 分配交给真正的使用方（下面的 ffrdp_conn_bufs ✓）。
    // 为什么：服务端那条内嵌主连接从不用于数据，若在这里分配、再在 init 里还回去，
    // glibc 不会把 152 KB 还给 OS ⇒ RSS 一点不降 ✗（实测 3456 → 3712 KB ✓）。
}

// 按需分配一条新连接（方案 A）。句柄 = 表下标 ✓、**索引永不复用** ✓（见 ffrdp.h 的设计说明）：
// 所以下标只从 next_slot 往后取，关掉的槽留在表里当 NULL，不再被借走 ✓。
// 返回新连接；*handle 给句柄。表满（maxconn / FFRDP_MAXCONN）⇒ NULL。
static FFRDPCONN *ffrdp_conn_new(FFRDPCONTEXT *ffrdp, uint64_t addr, int *handle)
{
    FFRDPCONN *c;
    int        slot = ffrdp->next_slot;
    if (slot >= ffrdp->maxconn || slot >= FFRDP_MAXCONN) return NULL;
    if (!(c = calloc(1, sizeof(FFRDPCONN)))) return NULL;
    ffrdp_conn_init(c, ffrdp->conn.smss, ffrdp->conn.fec_txredundancy);
    if (ffrdp_conn_bufs(c) != 0) { ffrdp_conn_bufs_free(c); free(c); return NULL; }   // ①-38：受理的新连接必须有缓冲
    c->peer_addr = addr;
    c->flags    |= FLAG_CONNECTED;  // 服务端：对端就是它（学到即连上 ✓）
    ffrdp->table[slot] = c;
    ffrdp->next_slot   = slot + 1;
    ffrdp->nconn       = slot + 1;
    if (handle) *handle = slot;
    return c;
}

// 释放一条连接（主连接是内嵌的 ⇒ 只清空它的链表，不 free 结构体本身 ✓）。
static void ffrdp_conn_free(FFRDPCONTEXT *ffrdp, FFRDPCONN *c)
{
    int i;
    if (!c) return;
    list_free(&c->send_list_head, &c->send_list_tail);
    list_free(&c->recv_list_head, &c->recv_list_tail);
    if (c->cur_new_node) { free(c->cur_new_node); c->cur_new_node = NULL; }
    ffrdp_conn_bufs_free(c);   // ①-38：大缓冲按需分配 ⇒ 这里归还 ✓（内嵌主连接也一样安全 ✓）
    for (i = 0; i < ffrdp->nconn; i++) if (ffrdp->table[i] == c) ffrdp->table[i] = NULL;  // 槽不回收 ✓
    if (c != &ffrdp->conn) free(c);
}

// 配置串解析（v2）："ffrdps://ip[:port][,key:val]..." / "ffrdpc://ip[:port][,key:val]..."。
// 角色由 scheme 决定（替代 v1 的 server 参数）；smss/sfec 由 mss:/fec: 给（v2 的 init 没有位置参数）。
// 解析失败 ⇒ 返回 NULL 并说明原因 ✓（不静默）。
void* ffrdp_init(void *params, FFRDP_CALLBACK callback, void *cbctx)
{
    FFRDPCONTEXT *ffrdp = NULL;
    char          buf[192];
    char         *text, *addr, *key;
    int           server = 0, smss = FFRDP_MAX_MSS, sfec = 0;
    int           maxconn = FFRDP_MAXCONN, handshake_ms = 10000, idle_ms = 5000, hs_enable = 1;

    if (!params) { printf("ffrdp_init: params is NULL\n"); return NULL; }
    snprintf(buf, sizeof(buf), "%s", (char*)params);
    text = buf;
    if      (strncmp(text, "ffrdps://", 9) == 0) { server = 1; text += 9; }
    else if (strncmp(text, "ffrdpc://", 9) == 0) { server = 0; text += 9; }
    else { printf("ffrdp_init: params must start with ffrdps:// or ffrdpc:// (got \"%s\")\n", buf); return NULL; }
    addr = text;
    key  = strchr(text, ',');
    if (key) *key++ = 0;
    while (key && *key) {
        char *next = strchr(key, ',');
        char *colon;
        int   val;
        if (next) *next++ = 0;
        colon = strchr(key, ':');
        if (!colon) { printf("ffrdp_init: bad option \"%s\" (want key:value)\n", key); return NULL; }
        *colon++ = 0;
        val = atoi(colon);
        if      (!strcmp(key, "maxconn"))      maxconn      = val;
        // 时间的单位一律写进键名（毫秒 ✓）—— 与 ffrdp_send 的 timeout_ms 同一套风格，
        // 不留"配置是秒、内部是毫秒"的换算点 ✗（那正是踩过的坑）。
        else if (!strcmp(key, "handshake_ms")) handshake_ms = val;
        else if (!strcmp(key, "idle_ms"))      idle_ms      = val;
        else if (!strcmp(key, "mss"))          smss         = val;
        else if (!strcmp(key, "fec"))          sfec         = val;
        else if (!strcmp(key, "hs"))           hs_enable    = val;   // 三次握手开关（**默认开** ✓；hs:0 关掉 ⇒ v1 行为 ✓）
        else { printf("ffrdp_init: unknown option \"%s\"\n", key); return NULL; }
        key = next;
    }
    if (maxconn < 1 || maxconn > FFRDP_MAXCONN) { printf("ffrdp_init: maxconn %d out of range (1..%d)\n", maxconn, FFRDP_MAXCONN); return NULL; }

    // 平台相关的初始化（Windows 的 WSAStartup / 定时器精度）不在这里：协议层不碰 socket，socket
    // 那一层（socket.c / lowlevel-udp.c）自己管自己的。
    if (!(ffrdp = calloc(1, sizeof(FFRDPCONTEXT)))) return NULL;
    ffrdp->app_cb       = callback;
    ffrdp->app_cbctx    = cbctx;
    ffrdp->maxconn      = maxconn;
    ffrdp->handshake_ms = handshake_ms;
    ffrdp->idle_ms      = idle_ms;
    ffrdp->hs_enable    = hs_enable ? 1 : 0;
    ffrdp->next_slot    = 1;   // 0 号被内嵌的主连接占用 ✓
    // 连接表：第一步只有 0 号，指向内嵌的主连接 ⇒ 必须在第一次碰 ffrdp->conn 之前设好 ✓。
    ffrdp->table[0]     = &ffrdp->conn;
    ffrdp->nconn        = 1;

    // 主连接（P4）：单连接阶段协议只跑在它上面；多连接按地址分发是下一步。
    FFRDPCONN *c = &ffrdp->conn;
    ffrdp_conn_init(c, smss, sfec);
    // ①-38：**只有客户端**给内嵌主连接分配那两块大缓冲 ✓；
    // 服务端在握手模式下不使用它（受理进来的连接自己分配 ✓）⇒ 服务端**根本不分配** ⇒ 省 ~152 KB ✓。
    // （hs:0 的旧路径真要用到它时，由 lowlevel_cb 里那句按需分配 ✓）
    if (!server && ffrdp_conn_bufs(c) != 0) { free(ffrdp); return NULL; }
    ffrdp->tick_ffrdp_dump = get_timestamp32_ms();
    ffrdp->tick_ffrdp_init = ffrdp->tick_ffrdp_dump;
    c->tick_rto_backoff    = ffrdp->tick_ffrdp_dump - c->rto; // allow the first timeout to backoff
    // ①-41：应用队列（app_tx_buff）已删除 ✓ —— 应用线程在 ffrdp_send 里**直接成帧** ✓，
    // 工作线程只负责把成好的帧发出去 ✓（这条链路上少了一次"排队—出队"✓）。

    // 传输层：socket、bind、非阻塞、缓冲区大小、收包线程、定时，全在 lowlevel 里。协议只认
    // send / timer / 回调。服务端把 addr 交给它（bind:<addr>）；客户端交 "none"（不绑定，由内核在
    // 第一次发包时择源端口，与旧版一致），再按同一套约定解析对端。
    // 递归锁：事件回调是在**持锁**状态下被调用的 ✓，而 v2 的回调契约允许应用在回调里
    // 立刻 ffrdp_recv 把数据取走（FFRDP_CONSUMED ✓）。普通互斥量会让那一次调用自锁死 ✗。
    {   pthread_mutexattr_t mattr;
        pthread_mutexattr_init(&mattr);
        pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&ffrdp->lock, &mattr);
        pthread_mutexattr_destroy(&mattr);
    }
    pthread_cond_init (&ffrdp->cond, NULL);
    {
        char llparams[128], peer[128];
        if (server) {
            ffrdp->is_server = 1;
            // 允许直接给传输层的原样写法（"bind:..."），其余情况由这里补前缀。
            if (addr && strncmp(addr, "bind:", 5) == 0) snprintf(llparams, sizeof(llparams), "%s", addr);
            else snprintf(llparams, sizeof(llparams), "bind:%s", (addr && *addr) ? addr : "0.0.0.0");
        } else snprintf(llparams, sizeof(llparams), "none");
        ffrdp->lowlevel = ffrdp_udp_lowlevel_init(llparams, ffrdp_lowlevel_cb, ffrdp);
        if (!ffrdp->lowlevel) {
            printf("failed to start the lowlevel transport !\n");
            goto failed;
        }
        if (!server) {
            uint32_t peer_ip;
            int      peer_port;
            snprintf(peer, sizeof(peer), "%s", (addr && *addr) ? addr : "127.0.0.1");
            // 解析走共享约定（utils）；"对端必须给真端口（0 不算）"是协议这边的策略。
            if (ffrdp_addr_parse_text(peer, &peer_ip, &peer_port, FFRDP_ADDR_DEFAULT_PORT) != 0 || peer_port <= 0) {
                printf("failed to parse peer address \"%s\" !\n", peer);
                goto failed;
            }
            c->peer_addr = ffrdp_addr_make(peer_ip, (uint16_t)peer_port);
        }
    }

    // 握手（v2）：只有打开了 hs 才走。客户端建好就发 SYN；服务端等 SYN（收到才建连接 ✓）。
    if (ffrdp->hs_enable && !ffrdp->is_server && c->peer_addr) {
        pthread_mutex_lock(&ffrdp->lock);      // 低层已经在跑回调 ⇒ 动协议状态要持锁
        c->hs_state = HS_SYN_SENT;
        c->hs_start = get_timestamp32_ms();
        ffrdp_hs_send(ffrdp, c, FFRDP_HS_SYN);
        // 第一发很可能是失败的（lowlevel 的 socket 还没建好 ✓）⇒ 必须自己把定时器挂上，
        // 否则真 lowlevel"没有期限就永久 select" ⇒ 连重传的机会都没有 ✗。
        ffrdp_arm_timer(ffrdp);
        pthread_mutex_unlock(&ffrdp->lock);
    }

    return ffrdp;

failed:
    if (ffrdp->lowlevel) { ffrdp->lowlevel->exit(ffrdp->lowlevel); ffrdp->lowlevel = NULL; }
    pthread_cond_destroy (&ffrdp->cond);
    pthread_mutex_destroy(&ffrdp->lock);
    free(ffrdp);
    return NULL;
}

void ffrdp_exit(void *ctxt)
{
    FFRDPCONTEXT   *ffrdp = (FFRDPCONTEXT*)ctxt;
    FFRDPCONN      *c;
    FFRDP_LOWLEVEL *ll;
    int             i;
    if (!ctxt) return;  // 判空**必须**在最前：下面每句都要用 ctxt ✓
    c = &ffrdp->conn;   // 主连接（内嵌 ✓，由末尾那几行负责 ✓）
    // 顺序是有讲究的：先让等待中的应用线程醒过来（它们看到 closing 就返回），再停工作线程。反过来的话，
    // 一个卡在 cond 上的应用线程会等一个再也不会有人推进的队列，而 exit 又 join 工作线程 —— 两边互等。
    // exit 必须在锁外调用：它要 join 的正是那个每次回调都要拿这把锁的线程。
    pthread_mutex_lock(&ffrdp->lock);
    ffrdp->closing = 1;
    pthread_cond_broadcast(&ffrdp->cond);
    ll = ffrdp->lowlevel;
    pthread_mutex_unlock(&ffrdp->lock);
    // 先停线程 + join（socket 是它自己关的，见 lowlevel.h）。**必须在摘掉把手之前**：工作线程此刻可能正在
    // 回调里跑一整段（ffrdp_tick_conn 会经 ffrdp_send_data_frame 用 ffrdp->lowlevel）—— 先前这里先把 lowlevel
    // 置 NULL 再 exit，那把正在跑的 tick 就会在 ffrdp_send_data_frame 里对 NULL 取成员（ffrdp->lowlevel->send ✓），实测 0.17% 的收尾段错误就是这么来的。
    if (ll) ll->exit(ll);
    pthread_mutex_lock(&ffrdp->lock);
    ffrdp->lowlevel = NULL; // 线程已停、handle 也已被它自己释放 ⇒ 现在摘掉才安全
    // ⚠️ 按需分配的连接**必须等在这里**再放 ✗✓：工作线程刚刚可能正跑在
    //    `ffrdp_tick_conn → ffrdp_send_data_frame` 里（手里攥着某条连接 ✓）。
    //    旧顺序（在入口处、锁外就把它们 free 掉）实测让**服务端收尾 6/6 段错误** ✓，
    //    栈是 `ffrdp_send_data_frame ← ffrdp_tick_conn ← ffrdp_lowlevel_cb ✓` ——
    //    下面那段"顺序是有讲究的"注释讲的是同一件事，只是漏了连接这一半 ✓。
    for (i = 0; i < ffrdp->nconn; i++) {
        if (ffrdp->table[i] && ffrdp->table[i] != &ffrdp->conn) {
            ffrdp_conn_free(ffrdp, ffrdp->table[i]);
            ffrdp->table[i] = NULL;          // 表槽不回收的老规矩不变 ✓（这里只是清指针 ✓）
        }
    }
    pthread_mutex_unlock(&ffrdp->lock);
    if (c->cur_new_node) free(c->cur_new_node);
    list_free(&c->send_list_head, &c->send_list_tail);
    list_free(&c->recv_list_head, &c->recv_list_tail);
    ffrdp_conn_bufs_free(c);   // ①-38：内嵌主连接不经 conn_free ⇒ 这里归还它那两块大缓冲 ✓
    pthread_cond_destroy (&ffrdp->cond);
    pthread_mutex_destroy(&ffrdp->lock);
    free(ffrdp);
}

// ①-41：ffrdp_send 现在直接成帧 ⇒ 下面这个成帧器需要前向声明 ✓。
static int ffrdp_frame_message(FFRDPCONTEXT *ffrdp, FFRDPCONN *c, const uint8_t *buf, int len);

// 发送对外只有一个入口，语义没有变：**要么整条接受、要么一个字节都不接受**。
// ⚠️ 2026-10-16（①-41，实验）：**应用队列已删除** ✓ ⇒ 回到 v1.0/v1.1 的形状：这里**直接成帧** ✓。
//   为什么可以：ffrdp_frame_message 本来就写成原子的 ✓（先把这一条要用的**所有**节点预分配好，再拷字节；
//   任何一次分配失败就整条放弃 ✓ ⇒ 调用方只会看到 len 或 <0，绝不会看到"半个消息"✓）；
//   并且这一段本来就整段持有 ffrdp->lock ✓ ⇒ 协议状态与工作线程之间仍然只被这把锁串起来 ✓。
//   代价：成帧成本从工作线程挪到应用线程 ✓；受理判据只剩帧数闸 + `g_unsent` 未发出帧预算 ✓。
// timeout_ms：0 = 暂时收不下就直接失败；>0 = 最多等这么久（等工作线程腾出未发出额度）；<0 = 等到能收。
int ffrdp_send(void *ctxt, int conn, char *buf, int len, int timeout_ms)
{
    FFRDPCONTEXT *ffrdp = (FFRDPCONTEXT*)ctxt;
    uint32_t      t0;
    int           rc = FFRDP_EAGAIN;   // 默认：暂时收不下（队列满/在途满）⇒ 稍后重试
    if (!ffrdp || !buf || len <= 0) { if (ffrdp) ffrdp->stat_ref_noarg++; return FFRDP_EINVAL; }          // ①-24（只读 ✓）
    if (conn < 0 || conn >= ffrdp->nconn || !ffrdp->table[conn]) { ffrdp->stat_ref_noconn++; return FFRDP_CONN_INVALID; }  // ①-24（只读 ✓）
    pthread_mutex_lock(&ffrdp->lock);
    FFRDPCONN *c = ffrdp->table[conn]; // 这句就是"句柄 ⇒ 连接"的查表（v2 的分发入口之一）
    // ⚠️ 锁外那次判空**不够** ✗：上面到这里的几行之间没拿锁 ✓，协议线程可能正好在收 FIN /
    //    keepalive 判死时把这条连接收掉（表槽清空 + free ✓）⇒ 这里再读就是 NULL ✓。
    //    真机上这就是空指针解引用 ⇒ SIGSEGV（对端主动关连接后，应用照旧 send/recv 就会踩到 ✓）。
    if (!c) { ffrdp->stat_ref_noconn++; rc = FFRDP_CONN_INVALID; goto out; }   // ①-24（只读 ✓）
    // ⚠️ 锁外那次判空**不够** ✗：上面到这里的几行之间没拿锁 ✓，协议线程可能正好在收 FIN /
    //    keepalive 判死时把这条连接收掉（表槽清空 + free ✓）⇒ 这里再读就是 NULL ✓。
    //    真机上这就是空指针解引用 ⇒ SIGSEGV（对端主动关连接后，应用照旧 send/recv 就会踩到 ✓）。
    if (!c) { ffrdp->stat_ref_noconn++; rc = FFRDP_CONN_INVALID; goto out; }   // ①-24（只读 ✓）
    // ⚠️ 锁外那次判空**不够** ✗：上面到这里的几行之间没拿锁 ✓，协议线程可能正好在收 FIN /
    //    keepalive 判死时把这条连接收掉（表槽清空 + free ✓）⇒ 这里再读就是 NULL ✓。
    //    真机上这就是空指针解引用 ⇒ SIGSEGV（对端主动关连接后，应用照旧 send/recv 就会踩到 ✓）。
    if (!c) { ffrdp->stat_ref_noconn++; rc = FFRDP_CONN_INVALID; goto out; }   // ①-24（只读 ✓）
    // 受理判据与旧版逐字相同：帧数上限（在途 + 这一条要占的帧）算在这里，不在出队时算。区别只在
    // "这个在途帧数从哪来"：协议跑在工作线程里，应用不再读协议自己的 wait_snd，而读它发布的镜像
    // （pub_wait_snd，每个回调结束时刷新）—— 值与时序都和旧版一致，但不再越过线程边界。
    // 三条拒绝路径都跳到同一个出口：计数与解锁只有一处，rc 已经预置成 -1。对端还没连上也在这里
    // 被拒绝（与以前一样）。
    if (ffrdp->closing) { c->counter_ref_closing++; rc = FFRDP_CLOSED; goto out; }    // 正在关：当作已断（①-24 ✓）
    if (ffrdp->is_server && (c->flags & FLAG_CONNECTED) == 0) { c->counter_ref_notconn++; rc = FFRDP_CLOSED; goto out; } // 对端还没连上（①-24 ✓）
    if (ffrdp->hs_enable) {
        if (c->hs_state == HS_FAILED) { c->counter_ref_hsfail++; rc = FFRDP_CLOSED; goto out; }   // 握手超时 ⇒ 当成已断（①-24 ✓）
        if (!ffrdp->is_server && c->hs_state != HS_UP) { c->counter_ref_hsdown++; rc = FFRDP_EAGAIN; goto out; } // 还在握手 ⇒ 先别发（①-24 ✓）
    }
    // ①-41：受理判据（队列删掉后）—— 帧数闸 + **未发出帧预算** ✓。三条都在下面的循环里判 ✓：
    //   ① g_unsent（默认 24 帧 ≈ 32 KB ✓）：接管原来应用队列的"一次只能攒 32 KB"整形作用 ✓；
    //   ② g_max_cwnd：等价于原来 ffrdp_drain_tx 的早退判据（`wait_snd >= g_max_cwnd` ⇒ 不再成帧 ✓）；
    //   ③ g_waitsnd：帧数闸（仍在，用实时 wait_snd ✓，不再用滞后的 pub_wait_snd ✗）。
    // ⚠️ 那条"单条消息比队列还大 ⇒ 永久 EINVAL"的判据随队列一起消失 ✓ ⇒ **任何大小的消息都能发** ✓
    //    （顺带修掉一个 wart ✓，也更接近 v1 的语义 ✓）。
    t0 = get_timestamp32_ms();
    {   int counted = 0;
        for (;;) {
            uint32_t need = (uint32_t)((len + c->smss - 1) / c->smss);
            // ⚠️ ①-41d：`tx_unsent == 0` 时**总放一条** ✗✓ —— 否则预算比一条消息的帧数还小
            //    （比如 UNSENT=8 而一条 16KB 消息要 13 帧 ✓）时 `tx_unsent + need > g_unsent` 永远成立
            //    ⇒ 这条消息永远进不来 ✗✗（lo 实测 UNSENT=8 ⇒ 吞吐掉到 1.5KB/s ✗，就是这个 ✓）。
            if ((!g_unsent || c->tx_unsent == 0 || c->tx_unsent + need <= g_unsent) &&
                c->wait_snd < (uint32_t)g_max_cwnd &&
                need + c->wait_snd <= g_waitsnd) {
                uint32_t seq0 = c->send_seq;   // ①-26：这一条消息成帧用了几帧 ✓（口径从"每圈"改成"每次 send"✓）
                rc = ffrdp_frame_message(ffrdp, c, (const uint8_t*)buf, len);   // 原子 ✓：整条或一个字节都不收
                {   uint32_t d = c->send_seq - seq0;
                    if      (d == 0) c->counter_dr0++;
                    else if (d == 1) c->counter_dr1++;
                    else if (d == 2) c->counter_dr2++;
                    else             c->counter_drM++;
                }
                if (rc > 0) break;                              // 整条被接受 ✓
                rc = FFRDP_EAGAIN;                              // 节点分配失败 ⇒ 稍后重试（计数由 out: 统一记 ✓）
            } else if (!counted) { c->counter_refuse_wnd++; counted = 1; }
            if (timeout_ms == 0 || ffrdp->closing) break;
            if (timeout_ms > 0 && (int32_t)(get_timestamp32_ms() - t0) >= timeout_ms) break;
        // 放不下而调用方愿意等：睡在 cond 上，让工作线程去成帧、去收 ack 腾空间。旧版是在这里自己跑
        // 一轮协议 —— 那正是"应用被迫驱动协议"的最后一个入口，现在协议由它自己的线程驱动。
        {   struct timespec ts;
            int wait_ms = timeout_ms > 0 ? timeout_ms - (int32_t)(get_timestamp32_ms() - t0) : FFRDP_COND_WAIT_MS;
            if (wait_ms <= 0) break;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec  += wait_ms / 1000;
            ts.tv_nsec += (long)(wait_ms % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&ffrdp->cond, &ffrdp->lock, &ts);
        }
        // ⚠️ 等的时候锁是**放开**的 ✓：协议线程可能正在这时把这条连接收掉
        //    （对端 FIN / keepalive 判死 ⇒ 表槽清空 + free ✓）⇒ 醒来必须**重新查表** ✗✓，
        //    不能续用旧指针 ✗（真机上这是 use-after-free ⇒ SIGSEGV：对端关连接时就踩到 ✓）。
        c = ffrdp->table[conn];
        // 连接没了 ⇒ 直接解锁返回（不能走 out: —— 那里要用 c 记计数 ✓）
        if (!c) { pthread_mutex_unlock(&ffrdp->lock); return FFRDP_CLOSED; }
        }   // ①-41：关闭上面新建的 for (;;)
    }       // ①-41：关闭 { int counted = 0; ... }
out:
    c->counter_send_calls++;   // ①-35b（只读 ✓）
    // 计数口径与旧版一致：只要这条消息没被接受就记一次（现在 rc 有 0 / -1 / -2 三种，
    // 旧版只有 -1 ⇒ 条件从 rc < 0 改成 rc <= 0 才能保持 fail 这个数不变 ✓）。
    if (rc <= 0) c->counter_send_failed++;
    // 刚排进去的东西不该等下一个期限：叫醒工作线程立刻跑一轮（这是 timer 的第二种用法）。
    if (rc > 0 && ffrdp->lowlevel) {   // ①-35c：叫醒（可合并 ✓）
        int wake = 1;
        if (g_sendwake_coalesce && c->tx_woken) { wake = 0; c->counter_send_wake_skip++; }
        if (wake) { c->counter_send_wakes++; c->tx_woken = 1; ffrdp->lowlevel->timer(ffrdp->lowlevel, get_timestamp32_ms()); }
    }
    pthread_mutex_unlock(&ffrdp->lock);
    return rc;
}

// 把一条消息成帧：要么整条被接受，要么一个字节都不接受 ✓。
// ①-41：数据直接来自**应用的内存**（`buf` ✓）—— 应用队列删掉之后，这里是唯一的成帧入口 ✓。
// 帧数上限（FFRDP_MAX_WAITSND）不在这里判：它在 ffrdp_send 的受理判据里判 ✓。
// ── 加密回调（v2）────────────────────────────────────────────────────────────
// 帧**定稿**时把载荷交给应用（FFRDP_ENCRYPT ✓）：原地改、长度不变 ⇒ 对 send/recv 完全透明 ✓。
// ⚠️ 帧有**两条**出口，每一处定稿都必须调它 ✓✗：
//      ① 攒满一个 smss（ffrdp_frame_message ✓）
//      ② 超时 / flush 的**短帧**（ffrdp_drain_tx ✓）
//    只挂一处 ⇒ 另一种帧明文出去 ✗，对端却按密文解 ⇒ 数据直接坏掉 ✗
//    （v1 那版 AES 就只挂在 ① 上 ✗；本仓库没编它，所以那个坑一直没暴露 ✓）。
// 只做一次：帧节点之后会被**原样重传** ✓ ⇒ 重传的是同一份密文 ✓
//    （重传再加密一次，对带状态/随机 IV 的密码就是解不开 ✗）。
static void ffrdp_frame_encrypt(FFRDPCONTEXT *ffrdp, FFRDPCONN *c, FFRDP_FRAME_NODE *node)
{
    if (!node || !ffrdp->app_cb) return;   // 没挂回调 = 不加密 ✓（线上原样 ✓）
    ffrdp_emit(ffrdp, FFRDP_ENCRYPT, ffrdp_conn_handle(ffrdp, c),
               c->peer_addr, node->data + 4, frame_payload_size(node));
}

static int ffrdp_frame_message(FFRDPCONTEXT *ffrdp, FFRDPCONN *c, const uint8_t *buf, int len)
{
    FFRDP_FRAME_NODE *spare = NULL, *node;
    int n = len, size, need, room;
    // ①-41：数据来自**应用自己的内存**（应用队列已删除 ✓）⇒ 这里必须判 buf，并把字节拷进帧里 ✓。
    if (!ffrdp || !buf || len <= 0) return -1;
    // Allocate every frame this call will need before writing a single byte: if one of the
    // allocations fails the whole call is abandoned (return -1, nothing accepted). Doing it the
    // other way round ("allocate while copying") lets the caller end up with 0 < ret < len, and it
    // cannot safely retry that message then: the part already taken would show up twice in the
    // stream. With this order ffrdp_send is atomic from the caller's point of view -- it either
    // accepts the entire message (returns len) or accepts nothing at all (returns -1).
    room = c->cur_new_node ? (int)(c->smss - c->cur_new_size) : 0;
    need = (len > room) ? ((len - room) + (int)c->smss - 1) / (int)c->smss : 0;
    while (need-- > 0) {
        node = frame_node_new(c->fec_txredundancy, c->smss);
        if (!node) {
            while (spare) { node = spare; spare = spare->next; free(node); }
            // ①-41：**不在这里记 counter_send_failed** ✗（那个计数器含义是"应用被拒过"✓，
            // 由 ffrdp_send 的出口统一记一次 ✓，否则一次 OOM 会被记两遍 ✓）。
            return -1;
        }
        node->next = spare; spare = node;
    }
    while (n > 0) {
        if (!c->cur_new_node) { // the pre-allocated chain is exactly enough, never runs dry
            c->cur_new_node = spare; spare = spare->next;
            c->cur_new_node->next = NULL;
        }
        SET_FRAME_SEQ(c->cur_new_node, c->send_seq);
        size = MIN(n, (int)(c->smss - c->cur_new_size));
        memcpy(c->cur_new_node->data + 4 + c->cur_new_size, buf + (len - n), (size_t)size);   // ①-41
        c->cur_new_size += size; n -= size;
        if (c->cur_new_size == c->smss) {
            ffrdp_frame_encrypt(ffrdp, c, c->cur_new_node);   // 出口 ①：攒满 smss ✓
            list_enqueue(&c->send_list_head, &c->send_list_tail, c->cur_new_node);
            c->send_seq++; c->wait_snd++;
            c->tx_unsent++;   // ①-41：已成帧、还没上过线 ✓（首发置 FLAG_FIRST_SEND 时减 ✓）
            c->cur_new_node = NULL;
            c->cur_new_size = 0;
        } else c->cur_new_tick = get_timestamp32_ms();
    }
    while (spare) { node = spare; spare = spare->next; free(node); } // cannot happen, but never leak
    return len - n; // with the pre-allocation above this is always len: the call is atomic
}

static int ffrdp_isdead_locked(FFRDPCONTEXT *ffrdp, FFRDPCONN *c)
{
    if (c->hs_state == HS_FAILED) return 1;   // 握手超时：对端从未连上 ⇒ 就是"已断"
    // keepalive：UP 且整整 idle_ms 没听到对端 ⇒ 判死（这是 v2 才有的"空闲也能判死" ✓）。
    if (ffrdp->hs_enable && c->hs_state == HS_UP &&
        (int32_t)(get_timestamp32_ms() - c->tick_last_recv) > ffrdp->idle_ms) return 1;
    if (!c->send_list_head) return 0;
    if (c->send_list_head->flags & FLAG_FIRST_SEND) {
        return (int32_t)get_timestamp32_ms() - (int32_t)c->send_list_head->tick_1sts > FFRDP_DEAD_TIMEOUT;
    } else {
        return (int32_t)c->tick_send_query - (int32_t)c->tick_recv_ack > FFRDP_DEAD_TIMEOUT;
    }
}

// 接收：有数据就立刻返回（不会为了凑满 len 而等），没数据时按 timeout_ms 等。
// 等待期间协议由 lowlevel 的工作线程自己推进（旧版是在这里让应用调 ffrdp_update，那一步已经删掉），
// 所以应用不再需要"主循环里调 update"这件事 —— 这个函数就是应用侧的等待点。对端失联时提前返回 0，免得调用方永远等下去。
int ffrdp_recv(void *ctxt, int conn, char *buf, int len, int timeout_ms)
{
    FFRDPCONTEXT *ffrdp = (FFRDPCONTEXT*)ctxt;
    uint32_t      t0;
    int           ret = FFRDP_EAGAIN;
    if (!ffrdp || !buf || len <= 0) return FFRDP_EINVAL;
    if (conn < 0 || conn >= ffrdp->nconn || !ffrdp->table[conn]) return FFRDP_CONN_INVALID;
    pthread_mutex_lock(&ffrdp->lock);
    FFRDPCONN *c = ffrdp->table[conn];   // 句柄 ⇒ 连接
    // ⚠️ 与 ffrdp_send 同一个坑 ✗：锁外判过空，锁内这次查询仍可能是 NULL ✓
    //    （两次查表之间协议线程收了 FIN / 判了死 ✓）⇒ 必须再判一次，不能直接解引用 ✓。
    if (!c) { pthread_mutex_unlock(&ffrdp->lock); return FFRDP_CONN_INVALID; }
    // ⚠️ 与 ffrdp_send 同一个坑 ✗：锁外判过空，锁内这次查询仍可能是 NULL ✓
    //    （两次查表之间协议线程收了 FIN / 判了死 ✓）⇒ 必须再判一次，不能直接解引用 ✓。
    if (!c) { pthread_mutex_unlock(&ffrdp->lock); return FFRDP_CONN_INVALID; }
    // ⚠️ 与 ffrdp_send 同一个坑 ✗：锁外判过空，锁内这次查询仍可能是 NULL ✓
    //    （两次查表之间协议线程收了 FIN / 判了死 ✓）⇒ 必须再判一次，不能直接解引用 ✓。
    if (!c) { pthread_mutex_unlock(&ffrdp->lock); return FFRDP_CONN_INVALID; }
    t0 = get_timestamp32_ms();
    for (;;) {
        ret = MIN(len, c->recv_size);
        if (ret > 0) {
            c->recv_head = ringbuf_read(c->recv_buff, c->recv_cap, c->recv_head, (uint8_t*)buf, ret);
            c->recv_size-= ret; c->counter_recv_bytes += ret;
            break;
        }
        // 先判死、再看"要不要等"：已断的连接，即使 timeout=0 也必须报 CLOSED ✓
        //（否则应用在 timeout=0 的轮询里永远学不到"断了"，只会一直拿到 EAGAIN ✗）。
        if (ffrdp_isdead_locked(ffrdp, c)) { ret = FFRDP_CLOSED; break; }
        if (timeout_ms == 0 || ffrdp->closing) { ret = FFRDP_EAGAIN; break; }   // 只是没有数据（还没断）
        if (timeout_ms > 0 && (int32_t)(get_timestamp32_ms() - t0) >= timeout_ms) { ret = FFRDP_EAGAIN; break; }
        // 睡在 cond 上等协议送数据来 —— 这一步之后，"等"是真正免费的：不再需要应用去驱动协议。
        {   struct timespec ts;
            int wait_ms = timeout_ms > 0 ? timeout_ms - (int32_t)(get_timestamp32_ms() - t0) : FFRDP_COND_WAIT_MS;
            if (wait_ms <= 0) { ret = FFRDP_EAGAIN; break; }
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec  += wait_ms / 1000;
            ts.tv_nsec += (long)(wait_ms % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&ffrdp->cond, &ffrdp->lock, &ts);
        }
        // ⚠️ 等的时候锁是**放开**的 ✓：协议线程可能正在这时把这条连接收掉
        //    （对端 FIN / keepalive 判死 ⇒ 表槽清空 + free ✓）⇒ 醒来必须**重新查表** ✗✓，
        //    不能续用旧指针 ✗（真机上这是 use-after-free ⇒ SIGSEGV：对端关连接时就踩到 ✓）。
        c = ffrdp->table[conn];
        if (!c) { ret = FFRDP_CLOSED; break; }   // 等的时候连接没了 ⇒ 按"已断"报 ✓（与上面判死同一个口径 ✓）
    }
    pthread_mutex_unlock(&ffrdp->lock);
    return ret;
}

// v2：失联是**每连接**的状态 ⇒ 按句柄问 ✓（v1 只有一个上下文，没法表达"哪条" ✗）。
// 返回：1 = 这条连接已被判定失联（含"句柄指向一条已关闭的连接" ✓ —— 无连接可问，就当死 ✓）
//       0 = 还活着；-1 = 参数错 / 句柄越界（与 send/recv 的 FFRDP_EINVAL 同值 ✓）
// 主动关一条连接（v2）：走 FIN 挥手（对端回 FIN 才真完 ✓）；hs:0 时只做本地拆除 ✓。
int ffrdp_close(void *ctxt, int conn)
{
    FFRDPCONTEXT *ffrdp = (FFRDPCONTEXT*)ctxt;
    FFRDPCONN    *c;
    if (!ctxt || conn < 0 || conn >= FFRDP_MAXCONN) return -1;
    pthread_mutex_lock(&ffrdp->lock);
    c = ffrdp->table[conn];
    if (!c) { pthread_mutex_unlock(&ffrdp->lock); return -1; }      // 已经关掉了 ⇒ 无效句柄 ✓
    if (ffrdp->hs_enable && c->hs_state == HS_UP) {
        // 正常路径：发 FIN，进 FIN_WAIT（重传与超时由 hs_* 那套接管 ✓）；
        // 等对端的 FIN 到了才 DISCONN + 收摊 ✓（那种情况在 ffrdp_hs_rx 里处理 ✓）。
        c->hs_state = HS_FIN_WAIT;
        c->hs_start = get_timestamp32_ms();
        ffrdp_hs_send(ffrdp, c, FFRDP_HS_FIN);
        ffrdp_arm_timer(ffrdp);
    } else {
        // 还没 UP（握手中/已失败）、或老路径（没有 FIN 这东西 ✗）⇒ 本地拆除 + 报 DISCONN ✓。
        ffrdp_emit(ffrdp, FFRDP_DISCONN, conn, c->peer_addr, NULL, 0);
        ffrdp_conn_free(ffrdp, c);
    }
    pthread_mutex_unlock(&ffrdp->lock);
    return 0;
}

int ffrdp_isdead(void *ctxt, int conn)
{
    FFRDPCONTEXT *ffrdp = (FFRDPCONTEXT*)ctxt;
    FFRDPCONN *c;
    int ret;
    if (!ctxt || conn < 0 || conn >= FFRDP_MAXCONN) return -1;
    pthread_mutex_lock(&ffrdp->lock);
    c = ffrdp->table[conn];
    ret = c ? ffrdp_isdead_locked(ffrdp, c) : 1;
    pthread_mutex_unlock(&ffrdp->lock);
    return ret;
}

// 诊断用微秒时钟：只为量"收到帧 → ack 已交给内核"这一段。
static uint64_t ffrdp_us(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)(ts.tv_nsec / 1000);
}

// ══ ①-25（本轮实验 ✓）：ack 的**时机**旋钮 ═══════════════════════════════════════════
// 默认 0 = 保持现状：收到帧就在 RECV 回调里**当场回** ack（注释里写明这是为省一整圈 RTT ✓）。
// 打开 FFRDP_ACK_COALESCE=1 ⇒ 退回"**一圈一个** ack"：只置 FLAG_ACK_DUE ✓，由 TIMER 那一支
// 统一发（`ffrdp_apply_acks` 的注释本来就是这么写的："真正发出去的地方是 ffrdp_tick_conn ——
// 与搬走之前'一次 update 一个 ack'同形" ✓）—— 那正是 v1.1 的形状 ✓。
// 为什么要这个旋钮：lowlevel-udp.c 文件头那张表（批 6 → 19.1 MB/s、批 64 → 10.3 MB/s ✗）把
// "批上限"与"ack 节奏"绑在一起 ✓ ⇒ 只动一个量不出因果 ✗，必须两个一起扫 ✓。
static int g_ack_coalesce = -1;
static int ffrdp_ack_coalesce(void)
{
    if (g_ack_coalesce < 0) {
        const char *e = getenv("FFRDP_ACK_COALESCE");
        g_ack_coalesce = (e && *e && atoi(e)) ? 1 : 0;
    }
    return g_ack_coalesce;
}

static void ffrdp_recvdata_and_sendack(FFRDPCONTEXT *ffrdp, FFRDPCONN *c)
{
    FFRDP_FRAME_NODE *p;
    int32_t dist, recv_mack, recv_wnd, size, i, alen;
    uint8_t data[9];
    while (c->recv_list_head) {
        dist = seq_distance(GET_FRAME_SEQ(c->recv_list_head), c->recv_seq);
        // size > 0 is the second line of defence against a frame whose length arithmetic went negative:
        // ringbuf_write would take that as four billion bytes to copy. The recv path no longer lets such
        // a frame in (see ffrdp_recv_data_frame), and this keeps a future path from doing it silently.
        if (dist == 0 && (size = frame_payload_size(c->recv_list_head)) > 0 &&
            size <= (int)(c->recv_cap - c->recv_size)) {
            // 收侧：位置与 v1 的 AES 一致 ✓ —— 就在把载荷写进应用缓冲之前，
            // 而且只在这一帧**真的要交付**时解一次 ✓（乱序 / 重复帧在那之前就已经丢掉 ✓）。
            if (ffrdp->app_cb)
                ffrdp_emit(ffrdp, FFRDP_DECRYPT, ffrdp_conn_handle(ffrdp, c),
                           c->peer_addr, c->recv_list_head->data + 4, size);
            // RECVDATA（异步通知 ✓）：解密之后、入缓冲之前报一次 ✓ —— 应用可以在回调里直接取走。
            //   返回 FFRDP_CONSUMED ⇒ 协议**不再留着** ✓（这帧仍然算已交付：recv_seq 照常推进、ack 照常发 ✓
            //   ⇒ 发送侧不会卡住 ✓）；否则数据照旧进接收缓冲，等应用 ffrdp_recv 取 ✓。
            // ⚠️ 回调返回后这块内存就还给协议了 ⇒ 要留就**在回调里搬走** ✓（拷贝或自行接管 ✓）。
            if (ffrdp_emit(ffrdp, FFRDP_RECVDATA, ffrdp_conn_handle(ffrdp, c),
                           c->peer_addr, c->recv_list_head->data + 4, size) != FFRDP_CONSUMED) {
                c->recv_tail = ringbuf_write(c->recv_buff, c->recv_cap, c->recv_tail, c->recv_list_head->data + 4, size);
                c->recv_size+= size;
            }
            c->recv_seq++; c->recv_seq &= 0xFFFFFF;
            list_remove(&c->recv_list_head, &c->recv_list_tail, c->recv_list_head);
        } else break;
    }
    for (recv_mack = 0, i = 0, p = c->recv_list_head; i <= 24 && p; i++, p = p->next) {
        dist = seq_distance(GET_FRAME_SEQ(p), c->recv_seq);
        if (dist <= 24) recv_mack |= 1 << (dist - 1); // dist is obviously > 0
    }
    // how many frames this ack is telling the peer about, counted in bits so that it can be compared
    // with counter_mack_bits_rx on the other side: the difference between the two is the part of the
    // reporting that the path ate. a frame can contribute a bit to several acks in a row, so this is
    // not a frame count -- it answers "is the reporting happening at all", which is the question that
    // counter_ack_mack (acks carrying any bit) is too coarse to answer.
    for (i = 0; i < 24; i++) if (recv_mack & (1 << i)) c->counter_mack_bits_tx++;
    // rmss is a divisor learned from whatever arrived (ffrdp_recv_data_frame rejects the lengths that
    // cannot carry a payload; it starts at FFRDP_MAX_MSS). Never divide by it unconditionally.
    recv_wnd = c->rmss ? (c->recv_cap - c->recv_size) / c->rmss : 0;   // ①-38：容量取自字段（值不变 ✓）
    recv_wnd = MIN(recv_wnd, 255);
    *(uint32_t*)(data + 0) = (FFRDP_FRAME_TYPE_ACK << 0) | (c->recv_seq << 8);
    *(uint32_t*)(data + 4) = (recv_mack <<  0);
    *(uint32_t*)(data + 4)|= (recv_wnd  << 24);
    // one byte of independent evidence for the sender: how many frames arrived since our last ack that we
    // already had, i.e. how many of its retransmits were pointless. the sender's own retransmit counter
    // cannot answer that (it counts attempts, not arrivals), this can. a peer that does not send the byte
    // simply leaves the field zero, and then the sender's floor never moves -- old peers stay safe.
    // pay for the extra byte only when there is something to say: on a link where nothing is being
    // retransmitted twice the ack stays exactly as long as it always was, so nothing about the sender's
    // behaviour can change there at all.
    if (c->dup_seen) { data[8] = (uint8_t)MIN(c->dup_seen, 255); alen = (int32_t)sizeof(data); }
    else             { alen = 8; }
    c->dup_seen = 0;
    // 只读（§26.30 ④-14）：这条 ack 覆盖了几个**到达的**全帧 ⇒ 就是"ack 攒了没有"的答案 ✓（见字段注释）
    {   uint32_t n = c->counter_rxfull - c->rx_frames_ref;
        c->rx_frames_ref = c->counter_rxfull;
        c->counter_ack_sent++;
        if      (n == 0) c->counter_ack_cov0++;
        else if (n == 1) c->counter_ack_cov1++;
        else if (n == 2) c->counter_ack_cov2++;
        else             c->counter_ack_covN++;
    }
    // this sendto used to be unchecked, which made an ack the socket refused indistinguishable from an ack
    // the network ate -- and the peer pays for both with a timeout. counting it is the whole change: the
    // ack is still attempted, and nothing here acts on the result.
    if (ffrdp->lowlevel->send(ffrdp->lowlevel, c->peer_addr, data, alen) != 0) c->counter_send_err++; // send ack frame
}

enum { CEVENT_ACK_OK, CEVENT_ACK_TIMEOUT, CEVENT_ACK_TIMEOUT_MISSED, CEVENT_FAST_RESEND, CEVENT_SEND_FAILED };
static void ffrdp_congestion_control(FFRDPCONTEXT *ffrdp, FFRDPCONN *c, int event)
{
    ffrdp_read_knobs();   // ①-28：幂等，一次之后就是常数 ✓
    switch (event) {
    case CEVENT_ACK_OK:
        if (c->cwnd < c->ssthresh) c->cwnd *= 2;
        else c->cwnd++;
        c->cwnd = MIN(c->cwnd, g_max_cwnd);
        c->cwnd = MAX(c->cwnd, FFRDP_MIN_CWND_SIZE);
        break;
    case CEVENT_ACK_TIMEOUT:
        c->counter_cwnd_back++;
        c->ssthresh = MAX(c->cwnd / 2, FFRDP_MIN_CWND_SIZE);
        c->cwnd     = FFRDP_MIN_CWND_SIZE;
        if (c->ssthresh == FFRDP_MIN_CWND_SIZE) c->counter_ssth_floor++;   // 由已塌的 cwnd 推出 ⇒ 只剩 +1/ACK ✗
        break;
    case CEVENT_SEND_FAILED:
        c->counter_cwnd_sfail++;
        c->ssthresh = MAX(c->cwnd / 2, FFRDP_MIN_CWND_SIZE);
        c->cwnd     = FFRDP_MIN_CWND_SIZE;
        if (c->ssthresh == FFRDP_MIN_CWND_SIZE) c->counter_ssth_floor++;   // 由已塌的 cwnd 推出 ⇒ 只剩 +1/ACK ✗
        break;
    case CEVENT_ACK_TIMEOUT_MISSED:
        // 伪超时：等待期间 ack 到过（只是没覆盖这一帧）⇒ **链路没丢，是估计器没看见** ⇒ 降半就够 ✓
        if (g_pseudo_nocut) {   // ①-28 B：默认关 ✓；开了就**不动窗口**（证据只留给 rtt_tail/rto ✓）
            c->counter_pseudo_nocut++;
            break;
        }
        // 2026-10-15 真机：快 AP 上 cwnd 被打回 1 时发送速率掉到约"1 帧/RTT"、吞吐掉到 1.58~3.09 MB/s ✗，
        // 而 timeout diag 显示绝大多数超时属于这一类（acks-arrived-but-missed 139~167 ✓）。
        // fall through —— 与快速重传同一档：乘性降半，不做慢启动重启 ✓
    case CEVENT_FAST_RESEND:
        c->counter_cwnd_back++;
        c->ssthresh = MAX(c->cwnd / 2, FFRDP_MIN_CWND_SIZE);
        c->cwnd     = c->ssthresh;
        if (c->ssthresh == FFRDP_MIN_CWND_SIZE) c->counter_ssth_floor++;   // 同上（快速重传/伪超时这一档 ✓）
        break;
    }
}

// 确认等待的原始分布（纯计数 ✓、不参与任何判据 ✗）：桶 16/32/64/128/256/512/1024ms ✓。
// 数据来源见下面那个"被重传过的帧"分支：conf = 现在 - p->tick_send ✓（首发到被确认的整段等待 ✓，
// 里面含 RTO 本身 ✓）。它回答的是"这条链路的确认停顿时长分布长什么样"✓ —— 这正是决定
// 要不要动 rtt_tail/rto_floor 的那个数 ✗（单看限速后的 rtt_tail 看不出来 ✓）。
static void ffrdp_conf_hist(FFRDPCONN *c, uint32_t conf)
{
    if      (conf <= 16)   c->counter_conf_16++;
    else if (conf <= 32)   c->counter_conf_32++;
    else if (conf <= 64)   c->counter_conf_64++;
    else if (conf <= 128)  c->counter_conf_128++;
    else if (conf <= 256)  c->counter_conf_256++;
    else if (conf <= 512)  c->counter_conf_512++;
    else if (conf <= 1024) c->counter_conf_1024++;
    else                   c->counter_conf_more++;
    if (conf > c->counter_conf_max) c->counter_conf_max = conf;
}

// 把应用队列里的消息取出来成帧。队列是环形的，所以这里不需要 memmove 压实：
//   1) 先"窥看"4 字节长度前缀（ringbuf_read 会给新 head，丢掉即可 = 只读不推进）；
//   2) 成帧（ffrdp_frame_message 只读）；
//   3) 成功了才用 ringbuf_read(..., NULL, 4 + len) 一次消费整条，head 往前挪。
// 失败时队列一个字都不动，消息下一轮还在（所以这里也**不**动 counter_send_failed：那个计数器只表示
// "应用被拒过"，含义不能变 —— dump 与仿真都看它）。
// 帧数上限不在这里判：它在 ffrdp_send 的受理判据里判，这里只是把排进来的东西全部成帧（注释修正之二）。
// ①-41：**这个函数已经删掉** ✓（原来是"把应用队列里的消息取出来成帧"✓）。
// 成帧现在发生在 ffrdp_send 里（应用线程 ✓），工作线程只负责把成好的帧发出去 ✓。
// 保留下面的历史注释，因为两条经验仍然有效 ✓：
//   · 表里排着窗口那么多"已成帧、未确认"的帧时不再往下喂（喂长了表尾的帧既发不出去、也等不到超时
//     ⇒ wait_snd 只涨不落，真机 lo 实测 264→279→401、吞吐 16.8→7.4MB/s ✗）—— 这条现在由
//     ffrdp_send 的 `wait_snd < g_max_cwnd` 与 `g_unsent` 预算承担 ✓；
//   · 短帧（超时 / FLUSH 凑不满一帧）不走这里 ✓ —— 它在 ffrdp_tick_conn 里定稿（出口 ② ✓）。
#if 0
static void ffrdp_drain_tx(FFRDPCONTEXT *ffrdp, FFRDPCONN *c)
{
    // ①-35c：抽空之后允许下一次 send 再叫醒（与 send 同一把锁 ⇒ 不漏醒 ✓）
    uint32_t cap = c->app_tx_cap;   // ①-38：容量取自字段（值不变 ✓）
    // 表里已经排着窗口那么多"已成帧、未确认"的帧时，这一轮不再往下喂：tick 的发送循环只从表头看
    // cwnd 个条目，喂得更长只会让表尾那些帧既发不出去、也等不到超时，wait_snd 于是只涨不落、应用被
    // 判满（真机 lo 实测：wait_snd 264->279->401，吞吐 16.8->7.4MB/s）。留在应用队列里的消息下一轮
    // 还能成帧，背压由队列容量（FFRDP_APP_TX_SIZE）承担，与旧版等价。
    // 早退一次就记一次（counter_drain_block）：真机上它是"窗口×3"的那个嫌疑，而它此前没有计数器。
    if (c->wait_snd >= (uint32_t)g_max_cwnd) { c->counter_drain_block++; return; }
    while (c->app_tx_size >= 4) {
        uint32_t len, head = (c->app_tx_head + 4) % cap;   // 负载起点（可能回绕）
        ringbuf_read(c->app_tx_buff, cap, c->app_tx_head, (uint8_t*)&len, 4);
        if (len == 0 || c->app_tx_size < 4 + len) {   // 不该发生；真发生了就清空队列，别把它卡死
            c->app_tx_head = c->app_tx_tail = 0;
            c->app_tx_size = 0;
            return;
        }
        if (ffrdp_frame_message(ffrdp, c, head, (int)len) != (int)len) {
            return;                                       // 分配失败：消息留在队列里，下一轮再试
        }
        c->app_tx_head = ringbuf_read(c->app_tx_buff, cap, c->app_tx_head, NULL, 4 + len);
        c->app_tx_size -= 4 + len;
    }
    if (c->app_tx_size == 0) c->tx_woken = 0;   // ①-35c：抽空了 ⇒ 下一次 send 重新叫醒 ✓
}
#endif

// 一批帧里跨帧累积的东西。旧写法在 recvfrom 循环外累积它；现在由调用方持有：
// 循环整批调用时是"一批一份"，RECV 回调里是"一帧一份" —— 语义相同，只是一次处理几帧的差别。
typedef struct {
    int32_t recv_una;   // 本批内预期的接收序号（洞被填上时往前走）
    int32_t send_una;   // 本批内确认到的最小未确认发送序号
    int32_t send_mack;  // 本批内对方向上报告的接收位图（按 seq 右移合并）
    int     got_data;   // 本批收到过数据帧 ⇒ 要回 ack
    int     got_query;  // 本批收到过查询帧 ⇒ 要回 ack
} FFRDP_RX_BATCH;

// 一帧：解析、记账。node 由调用方分配好（data/size 已就绪），这样"复用同一个节点"的旧写法保持
// 不变。返回 1 = 已移交给乱序队列（调用方不要再碰它）；返回 0 = 调用方可以复用或释放它。
static int ffrdp_on_frame(FFRDPCONTEXT *ffrdp, FFRDPCONN *c, FFRDP_FRAME_NODE *node, int len, FFRDP_RX_BATCH *b)
{
    int32_t una, mack, dist, i, j;
    int     taken = 0;

    if (node->data[0] <= FFRDP_FRAME_TYPE_FEC32) { // data frame
        node->size = len; // frame size is what the transport handed over
        if (ffrdp_recv_data_frame(ffrdp, c, node) == 0) {
            dist = seq_distance(GET_FRAME_SEQ(node), b->recv_una);
            if (dist == 0) { b->recv_una++; }
            // out of order arrival = this frame is ahead of a hole. If this counter grows while the
            // peer's acks carry no mack bit, then the hole is never being reported, and the sender
            // has no choice but to wait for timeouts.
            if (dist > 0) {
                c->counter_recv_ooo++;
                if (dist > 23) c->counter_recv_far++;
                // how far ahead of the hole this one was. the distance only ever shrinks by the hole
                // being filled, and by then this frame is below una and gets acked cumulatively
                // instead, so a frame further than the mask's last bit is never reported at all --
                // which is why the band it falls in decides whether a wider mask would buy anything.
                if      (dist <= 24) c->counter_ooo_d1_24++;
                else if (dist <= 64) c->counter_ooo_d25_64++;
                else {   // > 64: the tail one extension word cannot carry, split by how far it goes
                    c->counter_ooo_dgt64++;
                    if (dist <= 128) c->counter_ooo_d65_128++;
                    else             c->counter_ooo_dgt128++;
                }
            }
            // below the range we have already taken in: the peer sent this again although we had it,
            // which is exactly the waste we want to count and report back.
            if (dist < 0) { c->counter_recv_dup++; c->dup_seen++; }
            // further ahead than the window we hand out in every ack: the peer is credit limited to
            // that, so this is a lie or a duplicate. storing it would let anyone who can reach the
            // port spend our memory at one frame per datagram, for ever (see FFRDP_RECV_QUEUE_MAX).
            if (dist >= (int32_t)FFRDP_RECV_QUEUE_MAX) { c->counter_recv_drop++; }
            else if (dist >= 0) { list_enqueue(&c->recv_list_head, &c->recv_list_tail, node); taken = 1; }
            b->got_data = 1;
        }
    } else if (node->data[0] == FFRDP_FRAME_TYPE_ACK) { // ack frame
        una  = *(uint32_t*)(node->data + 0) >> 8;
        mack = *(uint32_t*)(node->data + 4) & 0xFFFFFF;
        c->counter_ack_recv++;
        // the peer's count of our retransmits that it had already accepted. this is the one piece of
        // evidence our own counters cannot fake, so it is what the rto floor is driven by.
        if (len >= 9 && node->data[8]) { c->counter_ack_dup += node->data[8]; c->win_dup += node->data[8]; }
        if (c->counter_ack_recv > 1 && (uint32_t)una == c->ack_una) c->counter_ack_una_dup++; // the peer did not move
        c->ack_una = (uint32_t)una;
        if (mack) { // the peer reported frames that arrived after a hole
            c->counter_ack_mack++;
            // counted in bits, to be compared with counter_mack_bits_tx on the other side: a
            // report that never arrived and a report that was never made look identical from here.
            for (j = 0; j < 24; j++) if (mack & (1 << j)) c->counter_mack_bits_rx++;
            for (i = 23; i >= 0 && !(mack & (1 << i)); i--);
            for (j = 0; j < i && (mack & (1 << j)); j++);
            if (j < i) c->counter_ack_hole++; // a gap below the highest acked frame
        }
        dist = seq_distance(una, b->send_una);
        if (dist >= 0) {
            b->send_una  = una;
            b->send_mack = (b->send_mack >> dist) | mack;
            // the peer's window is a buffer size, not a rate: on the measured link it allowed 148
            // frames where the path carried five, and the rest sat in its buffer as a standing queue
            // of about 200ms. cap is what a probe has proven, and what is being tested right now.
            c->swnd = node->data[7];
            c->peer_wnd_raw = c->swnd;
            // 这里原来把信用夹到 cap：那是探测还驱动信用时的写法。现在探测驱动的是"在途量"这道真闸
            // （inflight_max，见 tick 的探测段），再夹一次信用就是第二道刹车 —— 探测把 cap 压到 4 时，
            // 它会让"每收一个 ack 最多再发 4 帧"，真机上就是发送被饿死（swnd 恒等于 cap、send_failed
            // 占绝大多数）。信用只受对端缓冲本身约束，上界用一个不可能触发的大数兜住即可。
            if (c->swnd > g_waitsnd) c->swnd = g_waitsnd;
            c->tick_recv_ack = get_timestamp32_ms();
        }
    } else if (node->data[0] == FFRDP_FRAME_TYPE_QUERY) b->got_query = 1;
    return taken;
}

// 处理完一段接收之后要做的：把收到的 ack 应用到发送表 —— 释放已确认的帧、更新 rtt/rto 下限、
// 触发快速重传，并在收到数据帧时回一个 ack。旧写法是"整批一次"，RECV 回调里是"一帧一次"。
static void ffrdp_apply_acks(FFRDPCONTEXT *ffrdp, FFRDPCONN *c, FFRDP_RX_BATCH *b)
{
    FFRDP_FRAME_NODE *p, *t;
    int32_t dist, maxack, i;

    // 不在这里回 ack，只记一笔。lowlevel 的一圈会把当前就绪的包都送上来（有上限），所以"这一轮收到的帧"
    // 就是一个批次；ack 报的是 recv_seq + 位图，第二个 ack 覆盖的帧第一个已经报过了，什么都不多带。
    // 真正发出去的地方是 ffrdp_tick_conn —— 与搬走之前"一次 update 一个 ack"同形。
    if (b->got_data || b->got_query) c->flags |= FLAG_ACK_DUE;
    if (c->send_list_head && seq_distance(b->send_una, GET_FRAME_SEQ(c->send_list_head)) >= 0) { // handle frame ack
        for (p = c->send_list_head; p;) {
            dist = seq_distance(GET_FRAME_SEQ(p), b->send_una);
            for (i=23; i>=0 && !(b->send_mack&(1<<i)); i--);
            if (i < 0) maxack = (b->send_una - 1) & 0xFFFFFF;
            else maxack = (b->send_una + i + 1) & 0xFFFFFF;

            if (dist > 24 || !(p->flags & FLAG_FIRST_SEND)) break;
            else if (dist < 0 || (dist > 0 && (b->send_mack & (1 << (dist-1))))) { // this frame got ack
                c->counter_send_bytes += frame_payload_size(p); c->wait_snd--;
                ffrdp_congestion_control(ffrdp, c, CEVENT_ACK_OK);
                if (!(p->flags & FLAG_TIMEOUT_RESEND)) {
                    c->rttm = (int32_t)get_timestamp32_ms() - (int32_t)p->tick_send;
                    // rttm above is only the latest sample despite its name; this is the one that says how
                    // quick the path can be at all, and the one a queue on the path would show up in.
                    if (c->rtt_min == 0 || c->rttm < c->rtt_min) c->rtt_min = c->rttm;
                    if (c->rtts == (uint32_t)-1) {
                        c->rtts = c->rttm;
                        c->rttd = c->rttm / 2;
                    } else {
                        c->rtts = (7 * c->rtts + 1 * c->rttm) / 8;
                        c->rttd = (3 * c->rttd + 1 * abs((int)c->rttm - (int)c->rtts)) / 4;
                    }
                    {   // The peer's duplicate reports are the one piece of evidence on this path that our own
                        // retransmit counter cannot corrupt: it says, from the receiving end, how many of the
                        // frames we sent a second time it had already accepted. When that share is high the rto
                        // is still sitting inside the round trip distribution, and every one of those
                        // retransmits is paid for on the wire for nothing -- so push the floor up until they
                        // stop. The floor starts from the current rto when it is lower, so a single step is
                        // immediately binding: a raiser seeded below the rto is exactly what made the earlier
                        // attempts at this do nothing at all.
                        // the decision is taken once per evidence window, and both directions move on that
                        // same clock: raising per window while decaying per ack means the decay wins on any
                        // link with a high ack rate, and the floor then never moves at all.
                        // the evidence window closes on whichever comes first: enough retransmits to speak
                        // with, or two seconds. a window with too little evidence may never *raise* the
                        // floor (a handful of retransmits can read 100% pointless by accident), but it does
                        // let it decay: a link that has gone quiet has nothing a long rto would be
                        // repairing, and leaving the floor up would only hang repair delay on a link that
                        // has become fast again.
                        if (c->win_need >= FFRDP_RTO_WIN ||
                            (int32_t)get_timestamp32_ms() - (int32_t)c->tick_win >= 2000) {
                            c->tick_win = get_timestamp32_ms();
                            if (c->win_need >= FFRDP_WASTE_MIN) {
                                uint32_t waste = c->win_need ? c->win_dup * 100 / c->win_need : 0;
                                // the same evidence decides how much to trust a hole the peer reports: when it
                                // calls most of our repairs pointless, those holes are mostly frames that were
                                // overtaken on the way, so the fast path should wait; when the repairs are
                                // real, it must not wait at all.
                                {   uint32_t rtt = (c->rtts == (uint32_t)-1 || c->rtts < FFRDP_MIN_RTO) ? FFRDP_MIN_RTO : c->rtts;
                                    c->hole_wait = (waste >= FFRDP_WASTE_HI) ? MIN(3 * rtt, 300u)
                                                     : (waste <  FFRDP_WASTE_LO) ? 0 : rtt;
                                }
                                if (waste >= FFRDP_WASTE_HI) {
                                    if (c->rto_floor < c->rto) c->rto_floor = c->rto;
                                    c->rto_floor = MIN(c->rto_floor * 5 / 4, (uint32_t)FFRDP_RTO_FLOOR_MAX);
                                } else if (waste < FFRDP_WASTE_LO) {
                                    c->rto_floor = 0;      // the retransmits are reaching a peer that did not have them: repair fast
                                } else {
                                    c->rto_floor = c->rto_floor * 9 / 10; // in between: give ground slowly
                                }
                            }
                            // The cap is applied on every window, not only when the floor is raised: a floor
                            // that was reasonable while the link had a long round trip must come down by
                            // itself once the link speeds up, even if by then nothing is being retransmitted
                            // any more. Note that a quiet window is *not* fed into the decay path above --
                            // on a rate limited link a quiet window is usually the floor doing its job, and
                            // decaying on it would make the loop undo its own progress.
                            {   // the ceiling has to be relative to THIS path's own round trip. It used to be
                                // MAX(3 * rtt, 100) with rtt raised to FFRDP_MIN_RTO, so on a 1 ms path it was a
                                // flat 100ms -- a hole could sit that long on a path that reorders everywhere
                                // (measured; §26.5/§26.6). A repair delay's worth is a few typical trips + the rto min.
                                uint32_t rtt = (c->rtts == (uint32_t)-1) ? FFRDP_MIN_RTO : MAX(c->rtts, 1u);
                                uint32_t cap = MAX(4 * rtt, (uint32_t)FFRDP_MIN_RTO);
                                if (c->rto_floor > cap) c->rto_floor = cap;
                            }
                            c->win_need = c->win_dup = 0;
                        }
                        c->rto = MAX(c->rtts + 4 * c->rttd, c->rto_floor);
                        // 尾部也要被覆盖住：rtts/rttd 用的是"没被重传过"的样本 ⇒ 结构性看不见 RTO 以上的延迟
                        // （任何 > RTO 的样本都已经被排除），所以要靠 rtt_tail 补这一块。留 25% 余量。
                        if (c->rtt_tail)
                            c->rto = MAX(c->rto, MIN(c->rtt_tail + c->rtt_tail / 4, (uint32_t)FFRDP_MAX_RTO));
                        c->rto = MAX(FFRDP_MIN_RTO, c->rto);
                        c->rto = MIN(FFRDP_MAX_RTO, c->rto);
                    }
                }
                if (p->flags & FLAG_TIMEOUT_RESEND) {
                    // 被重传过的帧：上面那个 if 里的 RTT 采样必须跳过（Karn：这次确认可能对应原包，也可能
                    // 对应重传包），但"首发起多久才被确认"正是判断 RTO 够不够用的量 —— 真机上（RTT 1~5ms）
                    // 超时那一刻该帧已经等了 85~92ms，而 rtts/rttd 只用"没被重传过的帧"⇒ 结构性看不见这一档。
                    // 所以喂进 rtt_tail，但**限速增长**：首次最多 64ms、之后每次最多 +50% + 25ms。
                    // 真丢包时这个样本 ≈ RTO + RTT（Karn 担心的那个），限速之后它不可能一次把尾部推飞，
                    // 而每秒 1/16 的衰减会把它拉回来 ⇒ 收敛到"这条路径真实的确认延迟尾部"。
                    uint32_t conf  = get_timestamp32_ms() - p->tick_send;
                    ffrdp_conf_hist(c, conf);   // 只记账 ✓（限速前 ✓，要的就是原始分布 ✓）
                    uint32_t allow = c->rtt_tail ? c->rtt_tail + c->rtt_tail / 2 + 25 : 64;
                    if (allow > (uint32_t)FFRDP_MAX_RTO) allow = (uint32_t)FFRDP_MAX_RTO;
                    if (conf  > allow) conf = allow;
                    if (conf  > c->rtt_tail) c->rtt_tail = conf;
                }
                t = p; p = p->next; list_remove(&c->send_list_head, &c->send_list_tail, t); c->counter_delivered++; continue;
            } else if (seq_distance(maxack, GET_FRAME_SEQ(p)) > 0) {
                // the peer reported something above this frame, so it is either lost or merely late.
                // how long to wait before believing it is not a guess: it comes from the peer's own
                // duplicate reports (hole_wait, set once per evidence window). on a link where the peer
                // keeps confirming our retransmits as duplicates, holes are usually just frames that were
                // overtaken on the way, so wait; where the retransmits are really repairing something,
                // hole_wait is zero and this reacts exactly as before.
                if ((int32_t)get_timestamp32_ms() - (int32_t)p->tick_send >= (int32_t)c->hole_wait) {
                    c->counter_cc_hole++;                                   // ①-29（只读 ✓）
                    // ⚠️ 这一分支在**每次 ack** 都会重走发送表 ✗ ⇒ 原来同一帧会被反复判减半 ✓；
                    //    守卫从 ①-30 起**默认开** ✓ ⇒ 只认第一次（那一帧的 FLAG_FAST_RESEND 由重传路径清 ✓）。
                    if (!g_hole_cut_once || !(p->flags & FLAG_FAST_RESEND)) {
                        ffrdp_congestion_control(ffrdp, c, CEVENT_FAST_RESEND);
                        p->flags |= FLAG_FAST_RESEND;
                        c->counter_cc_hole_cut++;                           // ①-29（只读 ✓）
                        if (p->cc_cuts++) { c->counter_cut_multi++; if (p->cc_cuts > c->counter_cut_max) c->counter_cut_max = p->cc_cuts; }
                    }
                }
            }
            p = p->next;
        }
    }
}

// 协议要 lowlevel 什么时候叫它：把所有"下一次要做的事"折算成一个最早时刻。0 = 没有期限，睡到有包为止。
// 四处期限的来源与上面 tick 的四段一一对应：在途帧的 ack 超时、攒了一半的那一帧该发了、速率采样、
// 以及探测的下一次动作（再加查询对端窗口的周期）。
// "下一件该做的活"最早什么时候到（**不含**欠着对端一个 ack 这件事，那一条由 ffrdp_arm_timer 单独并进来）。
// 0 = 眼下没有别的事。返回 now 的意思是"现在就有活"（有队可发、有帧可首发）。这个判断与 ffrdp_tick_conn 的几段
// 一一对应，所以它同时也是"要不要跑整段"的判据 —— 只有 ack 欠着的时候，跑整段是白花的钱。
static uint32_t ffrdp_next_due(FFRDPCONTEXT *ffrdp, FFRDPCONN *c)
{
    const FFRDP_FRAME_NODE *q;
    uint32_t now = get_timestamp32_ms(), due = 0, x;
    int      tag = 0;                 // 只读（①-2）：当前 due 是哪个源赢的
    int      n_sent = 0;
    int      entry  = 0;              // 表项序号（与 tick 的 i 同坐标系 ✓）
    int      armed_now = 0;           // due 是"立刻就要发东西"（不是某帧的 RTO 到期 ✓）
    int      pending_first = 0;       // 发送表里有还没首发出去的帧
    int      sent_before_pending = 0; // 第一个"还没首发"的帧前面有几帧已首发
    // entry = 表项序号，与 tick 那个发送循环的 i **同一个坐标系** ✓（下面只数 tick 看得见的那一段 ✓）。
    for (q = c->send_list_head, entry = 0; q; q = q->next, entry++) {
        if (!(q->flags & FLAG_FIRST_SEND)) {
            // tick 的发送循环是"从表头起最多看 cwnd 帧"。表头那些已首发、在等 ack 的帧会把窗口占满，
            // 排在后面的未首发帧这一圈根本够不着 —— 那时候报"现在有活"就是空转：对端静默时 cwnd 塌成
            // 1，实测每秒 ~50 万圈、工作线程 ~40% CPU（notes 19.6 第 1 条）。记下它们的序号，下面一起判。
            if (!pending_first) sent_before_pending = n_sent;
            pending_first = 1;
            continue;
        }
        n_sent++;
        // ⚠️ 只有 tick 这一圈**看得见**的帧才拿它算期限：tick 的重传分支在
        //     `for (i = 0; ...; i < cwnd ...)` 里 ⇒ 表头 cwnd 个条目之外的帧，这一圈根本动不了 ✗。
        //     把窗口外的 tick_timeout 也算进来，就会拿一个"没人会处理"的过期期限要求"立刻" ⇒
        //     叫醒了、没人干活、期限又停在 now ⇒ 自旋 ✓（实测：cwnd=1 时表头没到期、后面那帧过期，
        //     忠实模型一个 tick 里转 60 万次 ✓）。
        //     n_sent 仍然按**全表**数 ✓：它的语义是"在途帧数"，和可见窗口是两件事 ✓。
        if (entry < (int)c->cwnd && (!due || (int32_t)(q->tick_timeout - due) < 0)) { due = q->tick_timeout; tag = 1; }
    }
    if (c->cur_new_node) {
        x = c->cur_new_tick + FFRDP_FLUSH_TIMEOUT;
        if (!due || (int32_t)(x - due) < 0) { due = x; tag = 2; }
    }
    x = c->rate_tick + 1000;
    if (!due || (int32_t)(x - due) < 0) { due = x; tag = 3; }
    // 握手期的重传期限（与 tick 里那条重传判据同一个值 ✓）。
    // ⚠️ 少了它，真 lowlevel 在"没有期限"时是**永久 select** ✓ ⇒ 没人来重传 ⇒ SYN 发不出去就永远发不出去 ✗
    //    （客户端建 lowlevel 用 "none"，socket 由它自己的工作线程懒建 ⇒ ffrdp_init 里第一发很可能
    //     撞上 sockfd < 0 ⇒ 静默失败 ✓）—— 仿真里假 lowlevel 没有这层懒建，所以永远看不见 ✗。
    if (c->hs_state == HS_SYN_SENT || c->hs_state == HS_SYN_RCVD || c->hs_state == HS_FIN_WAIT) {
        x = c->hs_tick + FFRDP_HS_RETRY_MS;
        if (!due || (int32_t)(x - due) < 0) { due = x; tag = 4; }
    }
    // keepalive：UP 的连接也要有人来叫醒（否则真 lowlevel 会一直睡到下次有数据 ✗）——
    // 半程探一次（周期跟 idle_ms 走 ⇒ 不会出现"探测周期比超时还长"的自相矛盾组合 ✓）。
    if (c->hs_state == HS_UP) {
        // ⚠️ 2026-10-15（§26.40）：起点要取"最后听到对端"与"最后发 QUERY"里**较晚**的那个 ✓。
        // 只取前者的话，对端静默之后这条期限**永远落在过去** ✗ ⇒ 被"过期 ⇒ 立刻"那条规则变成 now
        // ⇒ 每圈都要求"现在" ✗（QUERY 本身是限速的（`tick_send_query` ✓），这条期限却不限速 ✗）
        // ⇒ 与 tick 里那条 QUERY 判据对齐 ✓。（hs:0 走不到这里：hs_state 不是 HS_UP ✓）
        x = MAX(c->tick_last_recv, c->tick_send_query) + (uint32_t)(ffrdp->idle_ms / 2);
        if (!due || (int32_t)(x - due) < 0) { due = x; tag = 5; }
    }
    // 查询窗口的期限：**只有真的欠信用时才需要它** ✓。tick 里那条查询分支长在
    // `c->swnd == 0` 那一段里（没信用、还有帧等着首发才会问对端）⇒ swnd 充足时这条期限根本不可达 ✗
    // ⇒ 它会永远落在过去 ⇒ 每圈都要求"现在" ⇒ worker 空转 ✗（2026-10-15 实测：坏轮 STALE 122~132 万次、
    // 全部来自这条 ✓，吞吐 42 → 29 MB/s ✗；低层同时看到 260~910 万次 deadline 空圈 ✗）。
    // 另外"到点"也不该要求"现在"✗：到点后交给下一次 tick（帧超时/keepalive/速率采样必然会来 ✓）
    // 去发那个查询 ✓ —— 它在 500ms 内提前醒来仍然保留 ✓，避免查询被拖到下一次帧超时才做 ✓。
    if (c->tick_send_query && c->swnd == 0) {
        x = c->tick_send_query + FFRDP_QUERY_CYCLE;
        if ((int32_t)(x - now) > 0 && (!due || (int32_t)(x - due) < 0)) { due = x; tag = 6; }
    }
    if (c->probe_next)      { x = c->probe_next;                          if (!due || (int32_t)(x - due) < 0) { due = x; tag = 7; } }
    // 还差一条最容易漏、也最贵的：**现在就有发得出去的东西**（应用排了队，或者发送表里有还没首发的帧），
    // 而且发得出去（有信用、在途数没到限）⇒ 立刻叫一次。
    //   * 少了"应用队列"那一半：应用排完队要等某个 ack 或 RTO 到期才有人来发（实测 50 -> 10 MB/s）；
    //   * 少了"还没首发的帧"那一半：一个 ack 刚把窗口/在途数打开，却没有人立刻把队列里的帧顶上 ——
    //     管线在每个 ack 之后空一段。实测 tick 次数腰斩（45253 -> 22793 / 12s），吞吐同步腰斩。
    // 条件是"发得出去"而不是"有东西要发"：窗口关着时叫醒只是空转，那种情况下一个 ack 自然会来敲门。
    // 立刻叫醒的判据必须与"真正发得出去"的判据一致（否则窗口关着时空转）：
    //   * 应用队列里有东西 **且 drain 这一轮真会喂** —— drain 自己的早退闸是
    //     `wait_snd >= FFRDP_MAX_CWND_SIZE` ⇒ 表里排满窗口那么多未确认帧时它直接返回 ✗。
    //     ⚠️ 少了后面这半句就是自旋：叫醒了、drain 每轮都早退、期限又被 arm 成 now ⇒
    //     忠实模型（SIM_LL_DEADLINE=1）在**一个冻结的 tick 里**能转 10 万次回调，只有防自旋
    //     护栏能打断（实测 0.05 丢包 300 步要 63s、撞 4 次护栏 ✗）。护栏是给仿真兜底的，
    //     不该被当成设计的一部分 ✗。
    //   * 或者发送表里那个"还没首发"的帧，真的落在 tick 这一圈看得见的范围内（序号 < cwnd）。
    // ⚠️ ①-41c（修正我的 ①-41 的错 ✓）：这里要的是"有**没成帧**的数据 ⇒ 现在跑"（原来是
    // `c->app_tx_size` ✓ —— drain 一抽干就清零 ✓）。①-41 时我错换成 `send_list_head || cur_new_node` ✗
    // —— 而 `send_list_head` 装着**所有未确认帧**（整个传输期间恒非空 ✗）⇒ 这句恒真 ⇒ `due=now`
    // 恒触发 ⇒ 低层被 deadline 以 11.5 万圈/s 反复叫醒、98% 空跑 ✗（lo 实测 laps 187 万、
    // `arm_timer due <=now` 191 万 ✗）。正确的"新东西没成帧"在无队列版里就是 **tx_unsent > 0**
    // （已成帧但还没上过线 ✓ —— 那个计数器本来就是为此建的 ✓）。
    if ((c->tx_unsent > 0 && c->wait_snd < (uint32_t)FFRDP_MAX_CWND_SIZE) ||
        (pending_first && sent_before_pending < (int)c->cwnd)) {
        uint32_t limit = MAX(c->inflight_max ? c->inflight_max : g_inflight_max, (uint32_t)FFRDP_INFLIGHT_MIN);
        // 用本圈刚数出来的 n_sent（= 在途帧数）。不能用 c->inflight_used：它只在 ffrdp_tick_conn
        // 里刷新，而 ack 是在 DATA 回调(ffrdp_apply_acks)和 TIMER 的纯 ack 分支里处理的，那两条
        // 路径都不刷新它 ⇒ ack 刚把槽位空出来、该马上补下一帧的那一刻，判据读到的是旧值 ⇒
        // 不叫醒 ⇒ 睡到下面 rate_tick+1000 那条期限上（真机 y-run1：小窗口 lap 最大 1002ms、
        // 两端只剩 32~48 帧/s；大窗口下 limit=128 让旧值几乎永远成立 ⇒ 循环自旋掩盖了它）。
        // 2026-10-15（§26.39/§26.40）：**对端还在回话吗** ✓ —— 上面那三个条件（应用队列非空 ✓、
        // 对端窗口 > 0 ✓、在途没到限 ✓）**全是本端的状态** ✗：对端一消失，它们就停在最后一次好日子里
        //（收尾期实测：应用队列还压着 56 KB 没人清 ✓、`swnd` 停在 129 ✓、在途才 14 ✓）⇒ 每圈判
        //"现在就有活" ⇒ arm now ⇒ 回环 258k 圈/s ✗（§26.39 ✓）。所以这里要求"最近还在收到对端的 ack"✓：
        // 超过 `idle_ms / 2` 没收到 ⇒ 不再要"现在" ✓，收尾交给 keepalive / RTO 那些期限 ✓
        //（`idle_ms/2` 正是 keepalive 发 QUERY 的时刻 ✓，语义一致 ✓）。
        // 判据用 `tick_recv_ack`（ack 到达时刷新 ✓，**两种 hs 模式都有效** ✓）—— 不用 `tick_last_recv` ✗：
        // 它只在 `hs_enable` 时刷新 ✓，老路径（hs:0 / v1 互通 ✓）会被误判成"静默" ✗。
        // 还没收到过任何 ack（0 ✓）⇒ 当作"活着" ✓（起手阶段必须能要求"现在" ✓）。
        {   uint32_t now_a = get_timestamp32_ms();
            int quiet = (c->tick_recv_ack != 0) && (ffrdp->idle_ms > 0) &&
                        ((int32_t)(now_a - c->tick_recv_ack) > (int32_t)(ffrdp->idle_ms / 2));
            if (!quiet && c->swnd > 0 && (uint32_t)n_sent < limit) { due = now; armed_now = 1; tag = 0; }
        }
    }
    // ⚠️ tick 判"到期要重传"用的是 `now - tick_timeout > 0`（**严格**大于 ✓）⇒ tick_timeout 恰好等于
    //    now 的帧，这一圈不会被重传 ✗。若把它当"现在"叫醒，就是在**同一个冻结的 tick 里**空转 ✓
    //    （忠实模型实测：每个这样的步都要撞一次防自旋护栏 ✓）。它只差 1ms，往前挪一下就好 ✓。
    //    注意顺序：必须放在下面"过期 ⇒ 立刻"那句**之前** ✓ —— 真正已过期（< now）的帧 tick 是能干活的 ✓。
    if (!armed_now && due && (int32_t)(due - now) == 0) {
        c->counter_arm_eq_now++;                                     // 只读（①-3）
        c->counter_arm_eq_src[tag < 0 || tag > 7 ? 0 : tag]++;       // 只读：是哪个源要求的 ✗
        due = now + 1;
    }
    if (due && (int32_t)(due - now) < 0) due = now;       // 已经过期 ⇒ 立刻（这也是"快一点"的做法）
    // 兜底：有未确认的帧、或应用队列非空时最多睡 4ms。上面那条判据修好后它不该再触发；
    // 真机上 lap 最大曾到 59/114/222ms，每漏一次小窗口就少送好几帧，而多醒几次只费一点 CPU。
    // ①-41c：master 是 `send_list_head || app_tx_size`（"还没发出去的活儿" ⇒ now+4 ✓）。无队列版里
    // "没成帧的"不存在了 ✓，而未发出帧本来就在 send_list 里 ✓ ⇒ 只剩 `send_list_head` ✓。
    // 半帧（cur_new_node）有自己独立的 flush 期限（上面 1786 行 ✓），不该再进这条 nudge ✗。
    if (c->send_list_head) { if (!due || (int32_t)(due - (now + 4)) > 0) due = now + 4; }
    // 只读记账（①-2）：这一轮把期限设成了什么 ✓
    if (due && (int32_t)(due - now) <= 0) {
        if (armed_now) c->counter_due_now_work++;
        else { c->counter_due_now_stale++; c->counter_stale_src[tag < 0 || tag > 7 ? 0 : tag]++; }
    } else if (due) c->counter_due_future++;
    // 只读（①-19）：出口口径 —— 返回值是"立刻"就记一次源（tag: 1 frame / 2 flush / 3 rate /
    // 4 hs / 5 keepalive / 6 query / 7 probe / 0 其它=应用有活 ✓）⇒ 收尾期是谁在要"现在" ✓
    if (due && (int32_t)(due - now) <= 0) c->counter_now_src[tag < 0 || tag > 7 ? 0 : tag]++;
    if (due && (int32_t)(due - now) <= 2) c->counter_arm_le2ms++;    // 只读（①-3）
    return due;
}

// 协议要 lowlevel 什么时候叫它：上面那些期限，再加上"欠着对端一个 ack"（那件事这一刻就该做）。
static void ffrdp_arm_timer(FFRDPCONTEXT *ffrdp)
{
    uint32_t due = 0;
    int      i, win = -1;
    if (!ffrdp->lowlevel) return;
    // 取所有连接里最早的那个期限（只有一条连接时与 P4 之前逐字等价 ✓）。
    // 顺带：欠着对端 ack 的那条连接 = 这一刻就该做 ⇒ 它直接要求"现在"。
    for (i = 0; i < ffrdp->nconn; i++) {
        FFRDPCONN *c = ffrdp->table[i];
        uint32_t   x;
        if (!c) continue;
        x = ffrdp_next_due(ffrdp, c);
        if (c->flags & FLAG_ACK_DUE) { x = get_timestamp32_ms(); c->counter_due_now_ack++; ffrdp->arm_ack_win++; }
        if (!due || (int32_t)(x - due) < 0) { due = x; win = i; }
    }
    // 只读（§26.38 ①-18）：取 min 之后的那一跳 —— 分档 + 赢家 ✓（不改行为 ✗）
    ffrdp->arm_calls++;
    {   int32_t left = (int32_t)(due - get_timestamp32_ms());
        if      (!due)       ffrdp->arm_d0++;
        else if (left <= 0)  ffrdp->arm_dnow++;
        else if (left <= 4)  ffrdp->arm_dnear++;
        else                 ffrdp->arm_dfar++;
    }
    if (win >= 0 && win < FFRDP_MAXCONN) ffrdp->arm_win[win]++;
    // 只读（①-20）：**只**在"收尾态"（nconn>1 且 1 号槽已空 ✓）采样 8 条原始值 ⇒ 指名那一跳 ✓
    //（①-18/①-19 已把矛盾压到"arm_timer 的 due 与任何连接的 next_due 都不一致"✗ ⇒ 直接把值打出来 ✓）
    if (ffrdp->arm_samp < 16 && win == 1 && due && (int32_t)(due - get_timestamp32_ms()) <= 0 &&
        ffrdp->table[1] && (int32_t)(get_timestamp32_ms() - ffrdp->table[1]->tick_recv_ack) > 1000) {
        // ①-20b：**1 号槽赢 + due <= now** ⇒ 那 1.26M 次 `<=now` 到底是谁要的 ✓（把原始值打出来 ✓）
        FFRDPCONN *w1 = ffrdp->table[1];
        printf("<3> arm_timer: win slot 1, due %d ms, hs %d, swnd %u, wait_snd %u, "
               "send_list %s, cwnd %u, inflight %u, ack_due %d\n",
               (int)((int32_t)due - (int32_t)get_timestamp32_ms()), w1 ? (int)w1->hs_state : -1,
               w1 ? w1->swnd : 0, w1 ? w1->wait_snd : 0,
               (w1 && w1->send_list_head) ? "yes" : "no", w1 ? w1->cwnd : 0, w1 ? w1->inflight_used : 0,
               w1 ? (int)(w1->flags & FLAG_ACK_DUE) : 0);
        fflush(stdout);
        ffrdp->arm_samp++;
    }
    ffrdp->lowlevel->timer(ffrdp->lowlevel, due);
}

// 一段：所有"到期要做的事"。它由 lowlevel 的回调调用（TIMER），跑在它的工作线程上，并且被那一把锁
// 保护着。原来里面的"等 10ms + 把 socket 里的包全收下来"整段被删掉了 —— 收包现在是一帧一次的回调。
static void ffrdp_tick_conn(FFRDPCONTEXT *ffrdp, FFRDPCONN *c)
{
    FFRDP_FRAME_NODE *p = NULL;
    int32_t got_timeout = 0, i;
    uint8_t data[8];
    // ①-26（只读 ✓）：这一圈的入口快照
    uint32_t s0 = 0, cap0 = 0, sw0 = 0;
    ffrdp_read_knobs();   // ①-27：第一次进 tick 时读一次 env（此后是常数 ✓）
    // ①-41：成帧已经在 ffrdp_send（应用线程）里做完 ⇒ 这里不再 drain ✓。
    // ①-35c：应用喊过"叫醒"、这一轮工作线程已经跑过 ⇒ 允许它下次再喊 ✓（原来的清零点在 drain 里 ✓）。
    c->tx_woken = 0;
    // ── keepalive（v2：idle_ms 终于接上了 ✓）────────────────────────────────
    // 空闲 = 半程没听到对端 ⇒ 主动发一个 QUERY 去敲它（对端的 ack 回来我们就续命 ✓）。
    // 超过整整 idle_ms 还是没声 ⇒ 判死：报 DISCONN 并收摊（服务端）/ 置 HS_FAILED（客户端的
    // 主连接不能从表里摘掉 ✗ —— 表槽永不回收 ✓，后续 send/recv 按"已断"报 CLOSED ✓）。
    if (ffrdp->hs_enable && c->hs_state == HS_UP) {
        uint32_t now2 = get_timestamp32_ms();
        if ((int32_t)(now2 - c->tick_last_recv) >= ffrdp->idle_ms) {
            c->hs_state = HS_FAILED;
            ffrdp_emit(ffrdp, FFRDP_DISCONN, ffrdp_conn_handle(ffrdp, c), c->peer_addr, NULL, 0);
            if (c != &ffrdp->conn) ffrdp_conn_free(ffrdp, c);
            return;
        }
        if ((int32_t)(now2 - c->tick_last_recv) >= ffrdp->idle_ms / 2 &&
            (c->tick_send_query == 0 || (int32_t)(now2 - c->tick_send_query) >= ffrdp->idle_ms / 2)) {
            uint8_t q[1];
            q[0] = FFRDP_FRAME_TYPE_QUERY;
            if (ffrdp->lowlevel->send(ffrdp->lowlevel, c->peer_addr, q, 1) != 0) c->counter_send_err++;
            c->tick_send_query = now2;
            c->counter_send_query++;
        }
    }
    // how fast the work thread comes round: a starved lap delays acks and retransmits exactly
    // like a congested network does, and the two need to be told apart.
    c->counter_upd_cnt++;
    {   // 只读（①-5）：结算**上一圈**的产出 —— 模型说"现在有活"时，它到底发出去了几帧 ✗
        uint32_t s_now = c->counter_send_1sttime + c->counter_resend_rto + c->counter_resend_fast;
        // ⚠️ 2026-10-15 修：原来拿 1 当 0 的哨兵（`s_now ? s_now : 1` ✗）⇒ 对**一帧都没发过的
        //    内嵌槽**（服务端 0 号 ✓）会无符号下溢 ✗ ⇒ 它那行被打成 ">=2 帧"（真机实测过 ✓）。
        //    现在：0 = 这个连接还没发过任何帧 ✓ ⇒ 两侧都非零才算一圈 ✓（丢掉第一圈，可忽略 ✓）。
        if (s_now && c->lap_sent_ref) {
            uint32_t d = s_now - c->lap_sent_ref;
            if (d == 0)      c->counter_lap_sent0++;
            else if (d == 1) c->counter_lap_sent1++;
            else             c->counter_lap_sentN++;
            // 只读（§26.30 ④-15）：这一圈被闸门挡过、而且**一帧都没出去** ⇒ 闸门直接掐住 ✓（最硬的证据 ✓）
            if (c->counter_capblock != c->cap_ref && d == 0) c->counter_capb_nosend++;
        }
        c->cap_ref = c->counter_capblock;
        c->lap_sent_ref = s_now;
    }
    if (c->tick_last_upd) {
        uint32_t gap = get_timestamp32_ms() - c->tick_last_upd;
        c->counter_upd_gap += gap;
        if (gap > c->counter_upd_gap_max) c->counter_upd_gap_max = gap;
    }
    c->tick_last_upd = get_timestamp32_ms();

    if (c->cur_new_node && ((int32_t)get_timestamp32_ms() - (int32_t)c->cur_new_tick > FFRDP_FLUSH_TIMEOUT || c->flags & FLAG_FLUSH)) {
        c->cur_new_node->data[0] = FFRDP_FRAME_TYPE_SHORT;
        c->cur_new_node->size    = 4 + c->cur_new_size;
        ffrdp_frame_encrypt(ffrdp, c, c->cur_new_node);   // 出口 ②：超时/flush 的短帧 ✓
        list_enqueue(&c->send_list_head, &c->send_list_tail, c->cur_new_node);
        c->send_seq++; c->wait_snd++;
        c->cur_new_node = NULL;
        c->cur_new_size = 0;
    }

    {   // count what is really on the wire, fresh every pass, so no counter can drift or leak
        FFRDP_FRAME_NODE *q;
        for (c->inflight_used = 0, q = c->send_list_head; q; q = q->next) if (q->flags & FLAG_FIRST_SEND) c->inflight_used++;
        // 只读（§26.30 ④-15）：占用率 —— 在途量与当圈上限一起累加（均值对均值才说明"紧不紧" ✓）
        c->counter_inflight_sum += c->inflight_used;
        c->counter_limit_sum    += MAX(c->inflight_max ? c->inflight_max : g_inflight_max, (uint32_t)FFRDP_INFLIGHT_MIN);
        c->counter_inflight_n++;
    }
    s0   = c->counter_send_1sttime + c->counter_resend_rto + c->counter_resend_fast;   // ①-26
    cap0 = c->counter_capblock; sw0 = c->counter_swnd0;                                // ①-26
    for (i = 0, p = c->send_list_head; i < (int32_t)c->cwnd && p; i++, p = p->next) {
        if (!(p->flags & FLAG_FIRST_SEND)) { // first send
            // 顺序：先看信用、再看在途闸门。反过来的话，在途一满就 break，谁都到不了下面那条"要窗口"
            // —— 而"没有信用"正是最需要问对端一次的时候（真机上 counter_send_query 恒为 0 就是这么来的）。
            if (c->swnd > 0) {
                // frames are first sent in sequence order, so everything already sent sits in front of this
                // one and breaking here skips nothing that needed attention. retransmits are untouched: this
                // only stops adding new frames to a wire that already holds FFRDP_INFLIGHT_MAX of them.
                if (c->inflight_used >= MAX(c->inflight_max ? c->inflight_max : g_inflight_max, (uint32_t)FFRDP_INFLIGHT_MIN)) {
                    c->counter_capblock++;
                    // 只读（§26.30 ④-15）：被挡这一刻 —— 上限在不在地板 ✓、episode 起点 ✓
                    if (c->inflight_max && c->inflight_max > FFRDP_INFLIGHT_MIN) c->counter_capb_above++;
                    else                                                      c->counter_capb_floor++;
                    if (c->tick_cap_starve == 0) c->tick_cap_starve = get_timestamp32_ms();
                    break;
                }
                if (ffrdp_send_data_frame(ffrdp, c, p) != 0) { ffrdp_congestion_control(ffrdp, c, CEVENT_SEND_FAILED); break; }
                p->tick_1sts    = p->tick_send = get_timestamp32_ms();
                p->ticket       = c->counter_ack_recv;   // 诊断用：从这一刻起，ack 有没有来过
                p->tick_timeout = p->tick_send + c->rto;
                p->flags       |= FLAG_FIRST_SEND; if (c->tx_unsent) c->tx_unsent--;   // ①-41：上过线了 ⇒ 还回未发出额度 ✓
                c->swnd--; c->counter_send_1sttime++; c->inflight_used++;
                // 只读（§26.30 ①-②）：信用终于回来了（这一帧真出去了）⇒ 结算"等信用"的 episode ✓。
                // 用 p->tick_send（就是这一帧的首发时刻 ✓）当终点，不再多要一次时钟 ✓。
                if (c->tick_swnd_starve) {
                    uint32_t d = p->tick_send - c->tick_swnd_starve;
                    c->counter_swnd_wait_ep++;
                    c->counter_swnd_wait_ms += d;
                    if (d > c->counter_swnd_wait_max) c->counter_swnd_wait_max = d;
                    c->tick_swnd_starve = 0;
                }
                // 只读（§26.30 ④-15）：同一帧也解开了"被在途闸门挡住"的 episode ✓（与上面同终点 ✓）
                if (c->tick_cap_starve) {
                    uint32_t d2 = p->tick_send - c->tick_cap_starve;
                    c->counter_cap_wait_ep++;
                    c->counter_cap_wait_ms += d2;
                    if (d2 > c->counter_cap_wait_max) c->counter_cap_wait_max = d2;
                    c->tick_cap_starve = 0;
                }
            } else {
                c->counter_swnd0++;                        // 有帧等着首发、却没信用（这一项按帧计）
                if (c->tick_swnd_starve == 0) c->tick_swnd_starve = get_timestamp32_ms();   // 只读（§26.30 ①-②）：episode 起点 ✓
                if (c->tick_send_query == 0 || (int32_t)get_timestamp32_ms() - (int32_t)c->tick_send_query > FFRDP_QUERY_CYCLE) { // query remote receive window size
                    data[0] = FFRDP_FRAME_TYPE_QUERY; if (ffrdp->lowlevel->send(ffrdp->lowlevel, c->peer_addr, data, 1) != 0) c->counter_send_err++;
                    c->tick_send_query = get_timestamp32_ms(); c->counter_send_query++;
                    break;
                }
                // 没信用、又还没到该问的时候：**不 break** —— 这个节点只是发不出去，后面那些"到点该重传"
                // 的帧不吃信用（重传那一段不碰 swnd），必须照走（原逻辑如此，别在这里提前收口）。
            }
        } else if ((p->flags & FLAG_FIRST_SEND) && ((int32_t)get_timestamp32_ms() - (int32_t)p->tick_timeout > 0 || (p->flags & FLAG_FAST_RESEND))) { // resend
            if (!(p->flags & FLAG_FAST_RESEND)) {   // 只统计真正的 RTO 超时，快速重传不算
                uint32_t toage = get_timestamp32_ms() - p->tick_1sts;
                if (toage > c->counter_to_age_max) c->counter_to_age_max = toage;
                if (p->ticket == c->counter_ack_recv) c->counter_to_noack++; else c->counter_to_hadack++;
                // 这里原来按"una 已越过这一帧"采尾部 —— 真机四份日志里 rtt_tail 恒为 0 ⇒ 那个条件几乎
                // 不成立（帧被确认时就从表里移除，"una 越过它"与"它超时"很难同时发生）。采样改到确认
                // 到达的那一刻，见下面那个"被重传过的帧"的兄弟块。
            }
            // 只对真 RTO 分辨，快速重传保持原样（它不吃这一刀 ✓）。
            // p->ticket 是首发时 counter_ack_recv 的快照 ⇒ "等待期间 ack 有没有来过" 一句话就够 ✓
            // （与下面 1619 行那句 timeout diag 计数用的是同一个判断 ✓）。
            {
                int rto_real = !(p->flags & FLAG_FAST_RESEND);
                int pseudo   = rto_real && (p->ticket != c->counter_ack_recv);
                if (pseudo) c->counter_cc_pseudo++; else c->counter_cc_rto++;   // ①-29（只读 ✓）
                ffrdp_congestion_control(ffrdp, c, pseudo ? CEVENT_ACK_TIMEOUT_MISSED : CEVENT_ACK_TIMEOUT);
            }
            if (ffrdp_send_data_frame(ffrdp, c, p) != 0) break;
            if (!(p->flags & FLAG_FAST_RESEND)) {
                if (p == c->send_list_head) c->counter_rto_head++; else c->counter_rto_chain++;
                // was this frame already reported as received (una passed it) when the timer fired?
                // stale: we were told, but had not removed it -> the retransmit is pure waste.
                if (seq_distance(GET_FRAME_SEQ(p), (uint32_t)c->ack_una) < 0) c->counter_rto_stale++;
                else c->counter_rto_behind++;
                {   // ①-28 A（只读 ✓）：这一帧距 una 多远（= ack 的 24 位位图能不能盖到它 ✓）
                    int32_t d2 = seq_distance(GET_FRAME_SEQ(p), (uint32_t)c->ack_una);
                    if      (d2 <= 7)  c->counter_to_d0_7++;
                    else if (d2 <= 15) c->counter_to_d8_15++;
                    else if (d2 <= 23) c->counter_to_d16_23++;
                    else if (d2 <= 63) c->counter_to_d24_63++;
                    else               c->counter_to_dM++;
                    if (d2 > (int32_t)c->counter_to_dmax) c->counter_to_dmax = (uint32_t)d2;
                }
                if (c->rto == FFRDP_MAX_RTO) {
                    p->tick_send = get_timestamp32_ms();
                    p->flags    &=~FLAG_TIMEOUT_RESEND;
                } else p->flags |= FLAG_TIMEOUT_RESEND;
                if ((int32_t)get_timestamp32_ms() - (int32_t)c->tick_rto_backoff >= (int32_t)c->rto) { // rto backs off once per round, not once per frame
                    c->tick_rto_backoff = get_timestamp32_ms();
                    if (c->rto == FFRDP_MAX_RTO) c->counter_reach_maxrto++;
                    c->rto += c->rto / 2;
                    c->rto  = MIN(c->rto, FFRDP_MAX_RTO);
                    c->counter_rto_backoff++;
                }
                c->counter_resend_rto++;
                c->win_need++;
                got_timeout = 1;
            } else {
                p->flags &= ~(FLAG_FAST_RESEND|FLAG_TIMEOUT_RESEND);
                // count fast retransmits in the same evidence window as the timeout ones: they are
                // retransmits like any other, and on a link with reordering they are where most of the
                // waste sits (the peer confirms almost all of them as duplicates).
                c->counter_resend_fast++;
                c->win_need++;
            }
            p->tick_timeout+= c->rto;
        }
    }
    {   // ①-26：这一圈发了几帧 + 循环停在哪一条 + 圈末在途表长 ✓（纯计数 ✓）
        uint32_t d = (c->counter_send_1sttime + c->counter_resend_rto + c->counter_resend_fast) - s0;
        if      (d == 0) c->counter_ls0++;
        else if (d == 1) c->counter_ls1++;
        else if (d == 2) c->counter_ls2++;
        else if (d <= 4) c->counter_ls3++;
        else             c->counter_ls5++;
        if      (!p)                          c->counter_sp_list++;   // 表走完了 ⇒ 没得发
        else if (i >= (int32_t)c->cwnd)       c->counter_sp_cwnd++;
        else if (c->counter_capblock  > cap0) c->counter_sp_cap++;
        else if (c->counter_swnd0     > sw0)  c->counter_sp_swnd0++;
        else                                  c->counter_sp_other++;
        if      (c->inflight_used == 0)       c->counter_ll0++;
        else if (c->inflight_used <= 2)       c->counter_ll2++;
        else if (c->inflight_used <= 8)       c->counter_ll8++;
        else                                  c->counter_llM++;
    }
    if (got_timeout) c->counter_timeout_evt++;

    // 这里原来是"等 socket 可读 + 把所有待收的包一次收完 + 处理收到的 ack"。收包搬走了：lowlevel 一圈把
    // 当前就绪的包都回调上来（一帧一次），帧的解析在 ffrdp_on_frame、ack 的应用在 ffrdp_apply_acks 里；
    // 而应答本身由下面这一句在这一段结束时统一发出去 —— 位置与旧版一致（发送段之后、速率/探测之前）。
    if (c->flags & FLAG_ACK_DUE) { c->flags &= ~FLAG_ACK_DUE; c->counter_ack_ontick++; ffrdp_recvdata_and_sendack(ffrdp, c); }  // 只读（④-14）

    {   // delay probe: take half the in-flight away for a moment and see whether the round trip follows.
        // if it does and the confirmed rate does not, the delay was our own queue, so the smaller window
        // is kept and the experiment repeats later; if the delay does not follow, it belongs to the path
        // and the window goes straight back. that asymmetry is the whole safety of this: a link that
        // loses frames rather than queueing them fails the first test and is left exactly as it was.
        uint32_t now = get_timestamp32_ms();
        FFRDP_FRAME_NODE *q;
        if (!c->probe_gap) c->probe_gap = FFRDP_PROBE_GAP_MS;
        // the probe now drives the real brake (frames on the wire), not the peer's credit: the credit is
        // handed out again on every ack, so limiting it never bound anything and the experiment could not
        // change the round trip even in principle. without a proven cap the ceiling stays at its default.
        c->inflight_max = c->probe_state ? c->probe_cap
                            : (c->cap_kept ? c->cap_kept : g_inflight_max);
        c->cap = c->inflight_max;
        if ((int32_t)now - (int32_t)c->rate_tick >= 1000) {
            uint32_t resend = c->counter_resend_rto + c->counter_resend_fast;
            uint32_t firsts = c->counter_send_1sttime;
            c->rate_now     = c->counter_delivered - c->rate_del_ref;
            // 每秒结算一次"这一秒健不健康"：waste 与"没有队列"都要满足 ✓
            // giveback 只认**连续**健康（见 FFRDP_PROBE_GIVEBACK_IDLE_S ✓）—— 单看某一秒会被
            // "队列刚好排空的那一瞬"骗到 ✗（那正是原实现每秒还 25% 的来源 ✓）。
            {
                uint32_t idle_now = (c->rtt_min && c->rtts != (uint32_t)-1 &&
                                     c->rtts < c->rtt_min + FFRDP_PROBE_RTT_DROP_MS) ? 1u : 0u;
                if (c->probe_waste < FFRDP_PROBE_HEALTHY_PCT && idle_now) c->giveback_idle++;
                else                                                      c->giveback_idle = 0;
            }
            c->rate_tick    = now;
            if (c->rtt_tail) c->rtt_tail -= c->rtt_tail / 16;   // 约 16 秒忘掉旧的尾部
            c->rate_del_ref = c->counter_delivered;
            // the trigger needs evidence over a whole second. the window the rto floor uses holds a few
            // tens of frames, and one retransmit out of thirty is a ratio of three percent -- that is
            // sampling noise, not a wasteful path. the counters can also be cleared by a dump, so a
            // count that moved backwards is read as no evidence at all.
            c->probe_waste = (resend >= c->probe_resend_seen && firsts >= c->probe_first_seen)
                               ? (resend - c->probe_resend_seen) * 1000 / MAX(firsts - c->probe_first_seen, 1)
                               : 0;
            c->probe_resend_seen = resend;
            c->probe_first_seen  = firsts;
            // 见过的最好一秒（升就立刻记，降就每秒衰减 1/16，约 16 秒忘掉旧高水位）。keep 的速率参考要用它：
            // 只跟紧邻的一秒比，第一刀之后参考自己也掉，于是每一刀都通过，闸门被一路 keep 到地板。
            // 衰减 1/16 → 1/4（记忆 ~11s → ~2.5s）：参考要从"历史峰值"变成"最近的能力" ✓
            // 2026-10-14 慢 AP 日志：那条链路的确认速率在 0.31~0.63 MB/s（~250~500 帧/s）之间大幅起伏 ✗，
            // 而 keep 的速率守卫拿"历史最高"比 ⇒ 只要守卫窗口没撞上最好的那一秒就过不去 ✗
            // ⇒ 服务端连做 3 次探测、全部 revert ✗（rtt_down 其实成立：226→222 ✓），窗口永远降不下来。
            if (c->rate_now >= c->rate_best) c->rate_best = c->rate_now;
            else                             c->rate_best -= c->rate_best / 4;
            // 事后修复：被 keep 的闸门把确认速率拖到最好值的某个比例以下，就按秒翻倍还回去。
            // 频率与探测对齐 ✓：一次 giveback 之后至少隔 FFRDP_PROBE_GAP_MS 才允许下一次（两处共用
            // c->last_giveback 这一个预算 ✓）。2026-10-15 慢 AP 实测：这一处原来每秒都能还一次，
            // 用 +25% 的步长十秒内就能把 4 抬回 32 ✗（三轮 10/41/39ms 的波动就是它）。
            // 而那一轮 10ms 与 41ms 的吞吐完全相同（0.30/0.73、0.71/0.29 ✓）⇒ 在这条链路上低窗口
            // 不花吞吐 ⇒ 这种"急着还"是纯损失 ✗。安全网仍在：第一刀立刻给（速率掉到 85% 以下就还 ✓），
            // 只是不再一秒一刀 ✓。
            if (c->cap_kept && c->cap_kept < g_inflight_max && c->rate_best &&
                c->rate_now * 100 < c->rate_best * FFRDP_PROBE_RATE_KEEP_PCT &&
                (int32_t)get_timestamp32_ms() - (int32_t)c->last_giveback >= (int32_t)FFRDP_PROBE_GAP_MS) {
                // 原来这里是 *2：从被证明的 4 翻到 32 只要 3 秒 ✗ ⇒ 一步就能追上下探（探测每 10 秒才问一次 ✗），
                // 于是"刚降下来又被抬回去"。改成 +25%：4→5→6→8→10→13→16→20→25→32 约十秒 = 一个探测周期 ✓
                // （2026-10-15 慢 AP 实测：nogiveback 那个候选只关掉了另一处 giveback，这一处仍在 ⇒ 三轮
                //  9ms/9ms/116ms 的波动就是它 ✗）。安全网不变：速率掉到 85% 以下照样还 ✓，只是不再翻倍 ✓。
                c->cap_kept = MIN(c->cap_kept + MAX(c->cap_kept / 4, 1), g_inflight_max);
                c->last_giveback = get_timestamp32_ms();   // 记时刻 ⇒ 两处共用同一个"还的预算" ✓
                c->counter_giveback++;
            }
        }
        if (c->probe_state == 0) {
            uint32_t waste = c->probe_waste;
            if ((int32_t)now - (int32_t)c->probe_next >= 0) {
                uint32_t out = 0, cap_want, holding, idle;
                // count only frames that are really on the wire: the list also holds frames the
                // application has queued but that have not been sent yet, and halving those would not
                // halve what is in flight at all.
                for (q = c->send_list_head; q; q = q->next) if (q->flags & FLAG_FIRST_SEND) out++;
                // what earns an experiment is a path that is holding a queue, and rtt_min now makes that
                // visible directly: typical round trip far above the path's floor is a queue by definition,
                // and it is the delay a ping sees while the transfer runs. a retransmit count is a much
                // weaker trigger -- on this path it is down to a few per mille once the floor is up -- so it
                // stays only as a second way in for paths whose floor is already low.
                // the size gate used to be "at least 8 frames on the wire", read as "a pipe worth halving",
                // and it acted as a one way ratchet: with the ceiling already down at 4 or 5 there are never
                // 8 frames to see, so the probe could not ask again -- while the giveback below, whose job
                // is to notice a path that got faster, lifts a proven 4 to 5 and leaves it there. measured
                // state on the target path: inflight, cap = 5, 5, holding 4ms of queue over what the very
                // same path held at 4. what is worth asking is not a big pipe but a ceiling that can still
                // come down, so the gate is the halved ceiling being strictly under the one in force, and
                // the floor of FFRDP_INFLIGHT_MIN is where the descent legitimately ends.
                cap_want = MAX(out / 2, (uint32_t)FFRDP_INFLIGHT_MIN);
                // "is the path holding a queue" is asked twice below, but with two DIFFERENT yardsticks --
                // and that is what the measured sweep of 2026-10-10 taught: the ratio is a good gate for
                // starting an experiment and a bad one for ending it. On the target path rtts/rtt_min sat
                // at 3x in 18 of 29 dumps, at 4x in 8 and under 3x in 2, so a giveback gated on it opens
                // and closes from one check to the next; seven givebacks slipped through in 300 seconds,
                // each lifting a proven 4 back to 5 (once to 6), and each one pushing the next probe a whole
                // FFRDP_PROBE_GAP_MS away. The window therefore spent the run at 5 and measured rtts 17ms,
                // where the very same path pinned at 4 measured 11ms. So the trigger keeps the ratio (3x is
                // where a queue becomes worth a question) and the giveback needs the queue to be genuinely
                // gone, judged in milliseconds -- the same scale as the keep test it exists to undo.
                holding = (c->rtt_min && c->rtts != (uint32_t)-1 &&
                           c->rtts > FFRDP_PROBE_QUEUE_RATIO * c->rtt_min) ? 1u : 0u;
                idle    = (c->rtt_min && c->rtts != (uint32_t)-1 &&
                           c->rtts < c->rtt_min + FFRDP_PROBE_RTT_DROP_MS) ? 1u : 0u;
                // ①-39：**内存视角的触发** ✓ —— 上限比"这一圈真正在线上"高一倍以上就值得试 ✓
                // （要求已经量到过速率 ⇒ 不在应用爬坡时就把窗口砍掉 ✓）。
                // 它不需要排队证据 ✗：这里拿"省峰值内存"当收益，代价由 keep/revert 那条现成的
                // 安全网守着 ✓ —— 真需要大窗口的链路上，下探会让确认速率掉 ⇒ revert ✓ 并把
                // 下次间隔翻倍 ⇒ 一次失败的下探只值一个探针窗口，摊薄到可忽略 ✓。
                uint32_t mem_want = (g_probe_mem && c->rate_best > 0 && out >= FFRDP_MEMPROBE_MIN_OUT &&
                                     cap_want < c->inflight_max) ? 1u : 0u;
                if (FFRDP_PROBE_ENABLE && out > FFRDP_INFLIGHT_MIN && cap_want < c->inflight_max &&
                    (holding || waste >= FFRDP_PROBE_WASTE_PM || mem_want)) {
                    uint32_t rtt = (c->rtts == (uint32_t)-1 || c->rtts < FFRDP_MIN_RTO) ? FFRDP_MIN_RTO : c->rtts;
                    c->probe_state   = 1;
                    c->probe_tick    = now;
                    c->probe_hold    = 2 * rtt + FFRDP_PROBE_HOLD_MS;
                    c->probe_cap     = cap_want;
                    // the reference is the TYPICAL round trip, and the experiment tracks that same
                    // estimate, so the two numbers being subtracted come from one filter -- which is what
                    // makes a two millisecond difference readable at all. rttm is only a fallback, for a
                    // connection too young to have a typical yet.
                    c->probe_rtt_ref = c->rtts != (uint32_t)-1 ? c->rtts : c->rttm;
                    c->probe_rate_ref= c->rate_now;
                    c->probe_best    = 0xFFFFFFFF;
                    c->probe_del_ref = c->counter_delivered;
                    c->probe_res_ref = c->counter_resend_rto + c->counter_resend_fast;
                    c->probe_waste_ref = c->probe_waste;
                    c->cap           = c->probe_cap;
                    c->counter_probe++;
                    if (mem_want && !holding && waste < FFRDP_PROBE_WASTE_PM) c->counter_probe_mem++;
                    // ⚠️ 探测**进行中**时，`probe_next` 必须跟着推到未来 ✗：刷新它的两处都在"探测结束"
                    // （keep → now + probe_hold ✓ / revert → now + probe_gap ✓），而这段时间外层
                    // `if (c->probe_state == 0)` 又把整段跳过 ⇒ 它会一直停在启动那一刻（过去）✗
                    // ⇒ 最后一律被压成 `due = now` ⇒ `ffrdp_next_due` 每圈都要求"现在" ⇒ worker 空转 ✗。
                    // 2026-10-15 真机实测：每次探测约 14 秒（= probe_hold ✓）都在空转 ⇒ STALE 187~234 万次/轮、
                    // 低层 94~97% 的圈是空的 ✗（probes 7~9 次/轮 ⇒ 整轮大半时间都在自旋 ✓）。
                    c->probe_next = now + c->probe_hold;
                } else if (waste < FFRDP_PROBE_HEALTHY_PCT && c->cap_kept && idle &&
                           c->giveback_idle >= FFRDP_PROBE_GIVEBACK_IDLE_S &&
                           (int32_t)now - (int32_t)c->last_giveback >= (int32_t)FFRDP_PROBE_GAP_MS) {
                    // nothing is being wasted any more, and the queue is genuinely gone (idle, the absolute
                    // test above -- not the ratio, which sits right at its own threshold on this path): give
                    // a little of the window back, so a path that has become faster is not held down for
                    // ever by a limit proven on an older path. the idle half is not decoration: a giveback
                    // schedules the next look a whole FFRDP_PROBE_GAP_MS away, so giving back while a queue
                    // is still there parks a proven 4 at 5 for ten seconds at a time and hands back the win
                    // the probe just measured -- measured: seven times in 300s, which is why the run sat at
                    // 5. a path that got faster is a path whose queue has drained, and that is this test.
                    c->cap_kept   = MIN(c->cap_kept + MAX(c->cap_kept / 4, 1), g_inflight_max);
                    c->cap        = c->cap_kept;
                    c->probe_next = now + FFRDP_PROBE_GAP_MS;
                    c->last_giveback = now;
                    c->giveback_idle = 0;      // 还过一次就重新攒 ✓
                    c->counter_giveback++;
                } else {
                    // nothing to ask about yet: the queue test is not satisfied, so wait a full back off
                    // before looking again. looking sooner was tried and rejected on the measured link: it
                    // moved the first probe into the connection's ramp up, where both rate samples (before
                    // and during) are still climbing, so the rate guard read a false "the rate fell" and
                    // reverted -- even though the delay had plainly followed (probe rtt ref/best = 86/6).
                    // the revert doubles the back off, so asking early cost twenty seconds instead of
                    // saving ten. a rate guard needs a connection that has settled down before it means
                    // anything.
                    c->probe_next = now + FFRDP_PROBE_GAP_MS;
                }
            }
        } else {
            // track the same estimate the reference came from (an eight sample average, so it settles in
            // roughly a tenth of the hold): comparing rttm's noisiest sample with rtts' typical value was
            // the other half of why this experiment answered at random.
            if (c->rtts != (uint32_t)-1 && c->rtts < c->probe_best) c->probe_best = c->rtts;
            if ((int32_t)now - (int32_t)c->probe_tick >= (int32_t)c->probe_hold) {
                uint32_t span      = MAX(now - c->probe_tick, 1);
                uint32_t rate_held = (c->counter_delivered - c->probe_del_ref) * 1000 / span;
                // the delay followed if the typical round trip now sits at least FFRDP_PROBE_RTT_DROP_MS
                // under where it stood -- an absolute difference, because that is the size of the thing
                // being measured. a missing reference means a connection with no typical yet, and then
                // nothing has been proven: never keep a smaller window on a missing measurement. the order
                // of the two tests matters: probe_best is the 0xFFFFFFFF "no sample" sentinel until the
                // first update, and it must be rejected before it is subtracted.
                uint32_t rtt_down  = c->probe_best != 0xFFFFFFFF &&
                                     c->probe_rtt_ref > c->probe_best &&
                                     c->probe_rtt_ref - c->probe_best >= FFRDP_PROBE_RTT_DROP_MS;
                // 参考取"探测开始前一秒"与"见过的最好一秒"里的较大者。只跟紧邻的前一秒比是不够的：
                // 真机实测里第一刀砍下去之后参考本身跟着掉，于是每一刀都"速率没降"，闸门一路被 keep 到地板。
                uint32_t rate_ref  = MAX(c->probe_rate_ref, c->rate_best);
                uint32_t rate_kept = rate_held * 100 >= rate_ref * FFRDP_PROBE_RATE_KEEP_PCT;
                // 第二条证据：**重传强度**没变差。慢 AP 上真正的收益就在这里，而且是直接量到的 ✓
                // （2026-10-14：窗口砍半把 counter_resend_perMB 从 23.5 压到 4.8、重传率 2.86% → 0.58% ✓）。
                // 为什么需要它：rtts 是滑窗，突发链路上会在两次读数之间漂掉 ✗（同日日志 probe rtt ref/best
                // = 79/79 ✗ 而实际 227ms ↔ 79ms 在跳 ✓）⇒ 只看 RTT 会把"队列其实小了"漏掉 ✓。
                // 归一成"每千交付帧"，与探测前一刻的 probe_waste（每千首发帧）比 —— 两者同为 1 秒窗口 ✓。
                uint32_t resend_during = (c->counter_resend_rto + c->counter_resend_fast) - c->probe_res_ref;
                uint32_t del_during    = c->counter_delivered - c->probe_del_ref;
                uint32_t waste_during  = del_during ? (uint32_t)(1000.0 * resend_during / del_during) : 0;
                uint32_t waste_down    = waste_during + waste_during / 4 < c->probe_waste_ref;   // 低 20% 以上才认 ✓
                // 2026-10-15 定稿（真机三轮 8/11/20ms ✓，见 notes §26.18）：保留更小窗口**不再要求"看到 RTT 下降"** ✗。
                // 依据：慢 AP 同批对照里，10ms 与 40ms 两轮的吞吐完全相同（0.30/0.73、0.71/0.29 ✓）
                // ⇒ 低窗口在这条链路上不花吞吐 ✓；而 rtt_down 依赖的 rtts 是滑窗、突发链路上会踩空 ✗
                // （§26.16：日志 ref/best = 79/79，而实际 227↔79ms 在跳 ✓）⇒ 它只带来"中不中"的运气：
                // 同一条链路上，停在 40ms ✗ 与落到 11ms ✓ 各占一半。保留的两条都更硬：
                //   rate_kept  —— 速率没掉（快 AP 上继续减半就会掉 ⇒ 它自己挡回来 ✓，即"窗口 = 链路 BDP" ✓）
                //   waste_down —— 重传强度明显下来了（慢 AP 上真正的收益就在这里 ✓）
                if (rate_kept || waste_down) {
                    c->cap_kept  = c->probe_cap;
                    c->probe_gap = FFRDP_PROBE_GAP_MS;
                    // the experiment just proved there is more to win, so do not sit out a whole back off
                    // before asking the next question: walk the rest of the descent now. it stays self
                    // limiting -- the first empty answer doubles the gap again -- so a path nobody has
                    // measured converges in a few seconds instead of a few tens of them, and no value has
                    // to be picked for that path in advance.
                    c->probe_next = now + c->probe_hold;
                    c->counter_probe_keep++;
                } else {
                    // the delay did not follow, or it did but the confirming rate fell with it: either way
                    // the smaller window bought nothing, so go back to what was proven and wait longer.
                    if (rtt_down) c->counter_probe_narrow++;
                    c->probe_gap  = MIN(c->probe_gap * 2, (uint32_t)FFRDP_PROBE_GAP_MAX_MS);
                    c->probe_next = now + c->probe_gap;
                    c->counter_probe_revert++;
                }
                c->probe_state = 0;
                c->cap         = c->cap_kept;
            }
        }
    }
    {   // cost of one pass, so a slow application or a slow peer can be told apart from a slow link
        uint32_t cost = get_timestamp32_ms() - c->tick_last_upd;
        if (cost > c->counter_upd_cost_max) c->counter_upd_cost_max = cost;
    }
}

// lowlevel 的入口：它的工作线程在这里回到协议里。同一时刻只会有一路，锁保护整个回调，协议自己的状态
// 因此仍然只被一个线程碰。
// 按对端地址查连接（v2 的分发点：lowlevel 收到帧后用它决定流向哪条连接 ✓）。
// 连接句柄 = 表下标 ✓（见 ffrdp.h 的设计说明）；第一步 nconn 恒为 1 ✓，所以这里就是"是不是我们的对端"。
static FFRDPCONN *ffrdp_conn_by_addr(FFRDPCONTEXT *ffrdp, uint64_t addr)
{
    if (!addr) return NULL; // 0 = 没有来源地址 ⇒ 不认（旧版会把 0 学成对端 ✗）
    for (int i = 0; i < ffrdp->nconn && i < FFRDP_MAXCONN; i++) {
        FFRDPCONN *c = ffrdp->table[i];
        if (c && c->peer_addr == addr) return c;
    }
    return NULL;
}

// 把一个事件送给应用（v2）。调用点都在协议线程上、并且**持着锁**（锁是递归的 ✓，见 init）。
// app_cb 为 NULL 时什么也不做 ✓ —— 旧用法（只 ffrdp_recv 轮询）不受影响，输出也不变 ✓。
// 触发点分几步落地：ACCEPT/RECVDATA 要多连接与连接表（下一步）、CONNECT/DISCONN 要握手与判死
// （v2 的 handshake/idle 超时）、ENCRYPT/DECRYPT 要加密路径（本仓库没带 AES 实现）。
static long ffrdp_emit(FFRDPCONTEXT *ffrdp, int type, int conn, uint64_t addr, void *buf, int len)
{
    FFRDP_EVENT ev;
    if (!ffrdp || !ffrdp->app_cb) return FFRDP_NOT_CONSUMED;
    ev.type = type;
    ev.conn = conn;
    ev.addr = addr;
    ev.buf  = buf;
    ev.len  = len;
    return ffrdp->app_cb(ffrdp->app_cbctx, &ev);
}

// 一条连接在表里的句柄（= 下标）。-- 都没找到时返回 -1。
static int ffrdp_conn_handle(FFRDPCONTEXT *ffrdp, FFRDPCONN *c)
{
    for (int i = 0; i < ffrdp->nconn; i++) if (ffrdp->table[i] == c) return i;
    return -1;
}

// 发一个握手帧：data[0] = 35，data[1] = 子类型。照 QUERY 的写法：小栈缓冲 + lowlevel->send()
//（不走 frame_node_new ⇒ 不参与数据 ARQ、不带 FEC 尾巴 ✓）。调用时持锁。
static int ffrdp_hs_send(FFRDPCONTEXT *ffrdp, FFRDPCONN *c, uint8_t sub)
{
    uint8_t data[2];
    if (!ffrdp->lowlevel || !c || !c->peer_addr) return -1;
    data[0] = FFRDP_FRAME_TYPE_HANDSHAKE;
    data[1] = sub;
    c->hs_tick = get_timestamp32_ms();   // 成败都记：失败也要按 FFRDP_HS_RETRY_MS 的节奏再试 ✓
    if (ffrdp->lowlevel->send(ffrdp->lowlevel, c->peer_addr, data, 2) != 0) {
        c->counter_send_err++;
        return -1;
    }
    c->hs_retry++;
    return 0;
}

// 收到一个握手帧。调用时持锁。
static void ffrdp_hs_rx(FFRDPCONTEXT *ffrdp, uint64_t addr, uint8_t sub)
{
    FFRDPCONN *c;
    int        handle = -1;

    if (!ffrdp->hs_enable || !addr) return;

    if (ffrdp->is_server) {
        c = ffrdp_conn_by_addr(ffrdp, addr);
        if (!c) {
            // 陌生地址：只认 SYN ⇒ 建一条**半开**连接（方案 A：按需分配 ✓）。
            // 半开必须计时 + 封顶（见 tick）：否则伪造源地址的 SYN 能一路吃内存。
            if (sub != FFRDP_HS_SYN) return;
            // 新连接（半开）：先盖上"刚听到"的时间戳 ✓ —— 否则 keepalive 会把它当"已经静了很久" ✓✗。
            c = ffrdp_conn_new(ffrdp, addr, &handle);
            if (!c) { ffrdp->table[0]->counter_hs_refused++; return; } // 表满/内存不足 ⇒ 丢弃（不静默 ✓）
            c->hs_state = HS_SYN_RCVD;
            c->tick_last_recv = get_timestamp32_ms(); // 半开也算听到了 ✓
            c->hs_start = get_timestamp32_ms();
            ffrdp_hs_send(ffrdp, c, FFRDP_HS_SYN_ACK);
            return;
        }
        if (sub == FFRDP_HS_SYN && c->hs_state == HS_SYN_RCVD) { // SYN 丢了 ⇒ 重发 SYN-ACK
            ffrdp_hs_send(ffrdp, c, FFRDP_HS_SYN_ACK);
            return;
        }
        if (sub == FFRDP_HS_ACK && c->hs_state == HS_SYN_RCVD) { // 握手完成
            c->hs_state = HS_UP;
            c->tick_last_recv = get_timestamp32_ms(); // keepalive 计时从这里起 ✓（刚听到对端 ✓）
            c->flags   |= FLAG_CONNECTED;
            ffrdp_emit(ffrdp, FFRDP_ACCEPT, ffrdp_conn_handle(ffrdp, c), addr, NULL, 0);
            return;
        }
        if (sub == FFRDP_HS_FIN) { // 对端要关
            // 四次挥手：我方还没发过 FIN ⇒ **回一个** ✓（否则对端永远不知道我们收到了 ✗）；
            // 然后本地收摊 ✓（不做半关 ✓）。
            if (c->hs_state != HS_FIN_WAIT) ffrdp_hs_send(ffrdp, c, FFRDP_HS_FIN);
            ffrdp_emit(ffrdp, FFRDP_DISCONN, ffrdp_conn_handle(ffrdp, c), c->peer_addr, NULL, 0);
            ffrdp_conn_free(ffrdp, c);
            return;
        }
        return;
    }

    // 客户端：只有主连接，等 SYN-ACK。
    c = ffrdp->table[0];
    if (!c) return;
    // ⚠️ 客户端也要认 FIN ✗✓：对端主动关时，回 FIN 是**两边**都要做的事 ✓。
    //    缺这一段的话，我方 close 之后永远等不到对端的 FIN（真机实测：客户端一直报
    //    "没等到对端 FIN" ✓，只能等自己的 keepalive 超时才发现 ✗）。
    if (sub == FFRDP_HS_FIN) {
        if (c->hs_state != HS_FIN_WAIT) ffrdp_hs_send(ffrdp, c, FFRDP_HS_FIN);  // 对端先关 ⇒ 回一个 ✓
        ffrdp_emit(ffrdp, FFRDP_DISCONN, ffrdp_conn_handle(ffrdp, c), c->peer_addr, NULL, 0);
        // 主连接**不能**从表里摘掉 ✗（表槽永不回收 ✓；send/recv 靠 HS_FAILED 报 CLOSED ✓）
        c->hs_state = HS_FAILED;
        return;
    }
    if (sub == FFRDP_HS_SYN_ACK && c->hs_state == HS_SYN_SENT) {
        c->hs_state = HS_UP;
        c->tick_last_recv = get_timestamp32_ms();       // 同上 ✓
        c->flags   |= FLAG_CONNECTED;
        ffrdp_hs_send(ffrdp, c, FFRDP_HS_ACK);
        ffrdp_emit(ffrdp, FFRDP_CONNECT, 0, c->peer_addr, NULL, 0);
    }
}

static long ffrdp_lowlevel_cb(void *cbctx, int type, void *buf, int len)
{
    FFRDPCONTEXT *ffrdp = (FFRDPCONTEXT*)cbctx;
    long          ret  = 0;
    if (!ffrdp) return -1;
    pthread_mutex_lock(&ffrdp->lock);
    // 只读（①-9）：入口分档 —— 放在拿锁之后 ⇒ nconn 读到的是一致值 ✓
    if (type == FFRDP_LOWLEVEL_TYPE_TIMER)     ffrdp->cb_timer_calls++;
    else if (type == FFRDP_LOWLEVEL_TYPE_RECV) ffrdp->cb_data_calls++;
    else                                       ffrdp->cb_other_calls++;
    ffrdp->cb_nconn_sum += (uint32_t)ffrdp->nconn;
    if (ffrdp->nconn == 0) ffrdp->cb_nconn_zero++;
    if (ffrdp->closing) { ret = -1; goto out; }
    FFRDPCONN *c = ffrdp->table[0];   // RECV 的处理连接：单连接阶段就是主连接
                                      // （多连接时这里改成"按 msg->addr 查表/建半开连接"）
    if (!c) { ret = -1; goto out; }
    if (type == FFRDP_LOWLEVEL_TYPE_RECV) {
        FFRDP_LOWLEVEL_MSG *msg = (FFRDP_LOWLEVEL_MSG*)buf;
        // ⚠️ keepalive 的"听到对端"**不能刷在这里** ✗：这里的 `c` 是 `table[0]`（主连接）✓，
        //    而真正收数据的是下面 `ffrdp_conn_by_addr()` 解出来的 `cc` ✓ —— 刷错连接的话，
        //    服务端那条 accept 连接会一直"没听到" ✓✗ ⇒ 到 idle_ms 被误判死（实测每个 e2e 场景
        //    都在 5012ms 被杀 ✓）。所以刷新放到底下 `cc` 那一句之后 ✓。
        if (len != (int)sizeof(FFRDP_LOWLEVEL_MSG) || !msg->buf || msg->len <= 0) ret = -1;
        else if (ffrdp->hs_enable && msg->len >= 2
                 && ((uint8_t*)msg->buf)[0] == FFRDP_FRAME_TYPE_HANDSHAKE) {
            // 握手帧先处理：它在数据 ARQ 之外，而且服务端要能**从陌生地址**建连接 ✓。
            ffrdp_hs_rx(ffrdp, msg->addr, ((uint8_t*)msg->buf)[1]);
        } else {
            FFRDPCONN *cc;
            // 服务端从第一个收到的包学会对端（旧版是在收包循环里做同一件事）。
            // 走握手的连接不在这里学：它的对端在收 SYN 建连接时就定了 ✓，FLAG_CONNECTED 在收到 ACK 时置 ✓。
            if (c && ffrdp->is_server && !ffrdp->hs_enable && (c->flags & FLAG_CONNECTED) == 0) {
                // ①-38：握手关掉时服务端要靠这条内嵌主连接承接数据 ⇒ **这时**才分配它的缓冲 ✓
                if (ffrdp_conn_bufs(c) != 0) c = NULL;   // 分配失败 ⇒ 当没有连接（下面记孤儿帧 ✓）
            }
            if (c && ffrdp->is_server && !ffrdp->hs_enable && (c->flags & FLAG_CONNECTED) == 0) {
                c->flags |= FLAG_CONNECTED;
                c->peer_addr = msg->addr;
            }
            // "连上之后，别人的包不理"这句以前只是注释 ✗ —— 现在由查表真正执行 ✓：
            // 来源地址不是本连接对端的帧，一律丢弃并计数（不静默 ✓）。这就是 v2 的按地址分发。
            cc = ffrdp_conn_by_addr(ffrdp, msg->addr);
            // keepalive：**这条连接**刚有帧进来 ⇒ 续命 ✓（不管是数据帧还是半开期的帧 ✓ ——
            // "听到对端"本身就是事实 ✓；刷不刷得上跟"这帧最后收不收"是两件事 ✓）。
            if (cc && ffrdp->hs_enable) cc->tick_last_recv = get_timestamp32_ms();
            if (!cc) {
                c->counter_rx_foreign++;
            } else if (ffrdp->hs_enable && cc->hs_state != HS_UP) {
                cc->counter_rx_foreign++;   // 半开连接上的数据帧：握手没完成 ⇒ 丢
            } else if (!ffrdp->is_server || (cc->flags & FLAG_CONNECTED)) {
                int n = msg->len;
                FFRDP_FRAME_NODE *node = frame_node_new(FFRDP_FRAME_TYPE_FEC2, FFRDP_MAX_MSS);
                if (!node) ret = -1;
                else {
                    FFRDP_RX_BATCH b;
                    // 传输层一帧最大 FFRDP_LOWLEVEL_FRAME_MAX，接收节点按 MSS 建：夹一下，别越界。
                    if (n > (int)node->size) n = (int)node->size;
                    memcpy(node->data, msg->buf, (size_t)n);
                    b.recv_una  = cc->recv_seq;
                    b.send_una  = cc->send_list_head ? GET_FRAME_SEQ(cc->send_list_head) : 0;
                    b.send_mack = 0; b.got_data = 0; b.got_query = 0;
                    if (!ffrdp_on_frame(ffrdp, cc, node, n, &b)) free(node);  // 没被乱序队列接管 ⇒ 归我们
                    uint64_t t_ack0 = ffrdp_us();   // 诊断：从能回 ack 这一刻算起
                    ffrdp_apply_acks(ffrdp, cc, &b);
                    // 收到帧就当场回 ack，别等下一圈 TIMER（以前那一步要花掉整整一圈，lap 平均 2~3ms），
                    // 对端看到的 RTT 里就含着我们这一圈。顺带把"收到帧→已交给内核"的耗时记下来：
                    // 服务端比老架构多等的那 5~7ms 若不在这一段，就一定在内核/驱动出队之后。
                    if (!ffrdp_ack_coalesce() && (cc->flags & FLAG_ACK_DUE)) {   // ①-25：关掉就是留到 TIMER 那一支 ✓
                        cc->flags &= ~FLAG_ACK_DUE;
                        cc->counter_ack_ondata++;                 // 只读（④-14）：收到帧当场回的那一支 ✓
                        ffrdp_recvdata_and_sendack(ffrdp, cc);
                        uint64_t dt = ffrdp_us() - t_ack0;
                        cc->ack_lat_n++; cc->ack_lat_sum_us += dt;
                        if (dt > cc->ack_lat_max_us) cc->ack_lat_max_us = (uint32_t)dt;
                    }
                }
            }
        }
    } else if (type == FFRDP_LOWLEVEL_TYPE_TIMER) {
        int i;
        // 逐连接：别的到期工作都没有、只是欠着对端一个 ack 时，就别跑整段了。整段的开销是给
        // "发送/超时/探测/速率"付的，收到一批帧就付一次，实测把回环吞吐压掉了两成。
        for (i = 0; i < ffrdp->nconn; i++) {
            FFRDPCONN *cc = ffrdp->table[i];
            uint32_t   d;
            if (!cc) continue;
            if (ffrdp->hs_enable && (cc->hs_state == HS_SYN_SENT || cc->hs_state == HS_SYN_RCVD ||
                                     cc->hs_state == HS_FIN_WAIT)) {
                uint32_t now = get_timestamp32_ms();
                if ((int32_t)(now - cc->hs_tick) >= FFRDP_HS_RETRY_MS)
                    ffrdp_hs_send(ffrdp, cc, cc->hs_state == HS_SYN_SENT ? FFRDP_HS_SYN :
                                                cc->hs_state == HS_SYN_RCVD ? FFRDP_HS_SYN_ACK : FFRDP_HS_FIN);
                if ((int32_t)(now - cc->hs_start) >= (int32_t)ffrdp->handshake_ms) {
                    if (ffrdp->is_server) {
                        // 半开连接计时封顶：没人来完成握手就把这条连接放掉（表槽不回收 ✓）。
                        ffrdp_emit(ffrdp, FFRDP_DISCONN, i, cc->peer_addr, NULL, 0);
                        ffrdp_conn_free(ffrdp, cc);
                    } else {
                        cc->hs_state = HS_FAILED; // 客户端：此后 send/recv 按"已断"处理
                        ffrdp_emit(ffrdp, FFRDP_DISCONN, i, cc->peer_addr, NULL, 0);
                    }
                    continue;
                }
            }
            d = ffrdp_next_due(ffrdp, cc);
            {   // 只读（①-6）：期限模型这一圈给的是哪一档 ✓ —— 放在分支之前 ⇒ cc 一定有效 ✓
                uint32_t dn = get_timestamp32_ms();
                if (!d)                          cc->counter_fire_d0++;
                else if ((int32_t)(d - dn) <= 0) cc->counter_fire_dnow++;
                else if ((int32_t)(d - dn) <= 4) cc->counter_fire_dnear++;
                else                             cc->counter_fire_dfar++;
            }
            if ((cc->flags & FLAG_ACK_DUE) && (!d || (int32_t)(d - get_timestamp32_ms()) > 0)) {
                cc->flags &= ~FLAG_ACK_DUE;
                cc->counter_fire_ackonly++;                          // 只读（①-6）
                cc->counter_ack_ontick++;                            // 只读（④-14）
                ffrdp_recvdata_and_sendack(ffrdp, cc);
            } else {
                cc->counter_fire_tick++;                             // 只读（①-6）
                ffrdp_tick_conn(ffrdp, cc);
                // ⚠️ tick 里可能刚刚把这条连接**判死并收摊**（keepalive 超时 ⇒ 表槽清空 + free ✓）
                //    ⇒ 这里必须重新查表，绝不能再碰 cc 这个旧指针 ✗
                //   （ASAN 实测：`cc->pub_wait_snd = ...` 就是 heap-use-after-free ✓ ——
                //     仿真不带 ASAN 时读到的还是释放前的内存 ⇒ 不崩、所以一直没露 ✗）。
                if (ffrdp->table[i] != cc) continue;
            }
            cc->pub_wait_snd = cc->wait_snd; // 发布给应用看的镜像（受理判据用）
        }
    }
    c->pub_wait_snd = c->wait_snd; // 发布给应用看的镜像（受理判据用）
    ffrdp_arm_timer(ffrdp);
    pthread_cond_broadcast(&ffrdp->cond); // 可能有应用线程正等着空间或数据
out:
    pthread_mutex_unlock(&ffrdp->lock);
    return ret;
}

void ffrdp_flush(void *ctxt, int conn)
{
    FFRDPCONTEXT *ffrdp = (FFRDPCONTEXT*)ctxt;
    if (!ffrdp) return;
    // conn < 0 = 对全部连接生效 ✓（现在只有 0 号，逐个都刷一遍 ✓）。
    if (conn >= ffrdp->nconn || (conn >= 0 && !ffrdp->table[conn])) return;
    pthread_mutex_lock(&ffrdp->lock);
    FFRDPCONN *c = ffrdp->table[0];   // 第一步只有 0 号（conn < 0 = 全部连接）
    if (c) c->flags |= FLAG_FLUSH;
    // 工作线程可能正睡在"下一个期限"上：把它叫醒，这一帧才不用等到那时候。
    if (ffrdp->lowlevel) ffrdp->lowlevel->timer(ffrdp->lowlevel, get_timestamp32_ms());
    pthread_mutex_unlock(&ffrdp->lock);
}

// 逐连接 dump：conn < 0 ⇒ 表里每条打一块 ✓；conn >= 0 ⇒ **只打那一条** ✓（这就是 ffrdp.h 的承诺 ✓）。
static void ffrdp_dump_locked(FFRDPCONTEXT *ffrdp, int conn, int clearhistory)
{
    int i;
    if (conn >= 0) {
        // 指定连接：只这一条 ✓（空槽 / 越界在 ffrdp_dump 那层已经被拦掉 ✓）。
        // 以前这里把整个表都打一遍 ✗ ⇒ 服务端每次 dump 都多出一大段"0 号槽"的零 ✗，
        // 而那段零看起来就像"同一台机器的另一个时刻"（2026-10-14 实测确实把人误导了 ✓）。
        FFRDPCONN *c = ffrdp->table[conn];
        if (c) ffrdp_dump_conn(ffrdp, c, conn, clearhistory);
        return;
    }
    for (i = 0; i < ffrdp->nconn; i++) {
        FFRDPCONN *c = ffrdp->table[i];
        // ①-38d（2026-10-16 你的实测意见 ✓）：**服务端那块内嵌 0 号槽**在还没有连接时
        // 不要整表打出来 ✗。它每圈都被 ffrdp_tick_conn 空跑一遍（每圈的计数都在涨 ✓）⇒ 躲不过
        // ffrdp_dump_conn 里那道"完全空闲就只打一行"的门 ✗ ⇒ 整表 dump 会多出一大段
        // "peer not set / 全 0" 的块 ✗，看着就像另一条连接 ✓（就是这次的实测反馈 ✓）。
        // 判据：服务端 + 它是内嵌主连接 + 从没学到过对端 ✓；**显式点名**（conn >= 0）时不跳 ✓。
        if (c && ffrdp->is_server && c == &ffrdp->conn && c->peer_addr == 0) continue;
        if (c) ffrdp_dump_conn(ffrdp, c, i, clearhistory);
    }
    // ①-38：只读读数 —— 那两块"按需分配"的大缓冲**实际分配了多少字节** ✓。
    // 为什么需要它：省内存这件事不能靠"代码里没调用分配"来声称 ✗，而要能在自己的机器上
    // 直接看到数字 ✓：空转服务端应打 **0 bytes** ✓（它不使用内嵌 conn 0 ✓）；客户端
    // 打 ~176 KB ✓（主连接的两块 ✓）；服务端 hs:0 的旧路径真用到 conn 0 时也会出现 ✓。
    {
        unsigned long long sum = 0;
        int n = 0;
        for (i = 0; i < ffrdp->nconn; i++) {
            FFRDPCONN *c = ffrdp->table[i];
            if (c && c->recv_cap) { sum += (unsigned long long)c->recv_cap; n++; }
        }
        printf("conn bufs ①-38/①-41 : %llu bytes actually allocated over %d conn(s) — recv(%u) bytes per conn (the 32 KB app queue is gone); a server's embedded conn 0 allocates none\n",
               sum, n, (unsigned)FFRDP_RECVBUF_SIZE);
    }
}

// 握手状态的名字（块头要用 ✓）。
static const char* dump_hs_name(int hs)
{
    switch (hs) {
    case 0:           return "no handshake";   // 0 = 无握手（就是结构体 calloc 清零后的初值 ✓；那个宏已删 ✓）
    case HS_SYN_SENT: return "SYN sent";
    case HS_SYN_RCVD: return "SYN-ACK sent";
    case HS_UP:       return "established";
    case HS_FIN_WAIT: return "FIN sent (waiting for peer)";
    case HS_FAILED:   return "handshake failed";
    default:          return "?";
    }
}

// rtts 在"还没采到样"时是 (uint32_t)-1（算法内部就是这么用的 ✓，见 ffrdp_conn_init）：
// 直接打出来是 4294967295 ✗，这里打 "-" ✓。
static const char* dump_u32(uint32_t v, char *buf, int buflen)
{
    if (v == (uint32_t)-1) return "-";
    snprintf(buf, (size_t)buflen, "%u", v);
    return buf;
}

static void ffrdp_dump_conn(FFRDPCONTEXT *ffrdp, FFRDPCONN *c, int handle, int clearhistory)
{
    int      secs;
    char     addr[48];
    char     b1[16], b2[16];
    uint32_t ip = 0;
    uint16_t port = 0;
    secs = ((int32_t)get_timestamp32_ms() - (int32_t)ffrdp->tick_ffrdp_dump) / 1000;
    secs = secs ? secs : 1;

    // 块头：句柄 + 对端地址 + 握手状态 ✓。没有它的话，多连接时谁也看不出哪块是哪条 ✗
    // —— 服务端那个"从未通信的 0 号内嵌主连接"尤其容易被当成同一台机器的另一个时刻 ✗。
    if (c->peer_addr) {
        ffrdp_addr_split(c->peer_addr, &ip, &port);
        snprintf(addr, sizeof(addr), "peer %u.%u.%u.%u:%u",
                 (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF, (unsigned)port);
    } else snprintf(addr, sizeof(addr), "peer not set (not learned yet)");

    // 从未通信过的槽（服务端表里的内嵌主连接就是典型 ✓）只打一行：它的每一项都是 0 ✓，
    // 打满一百多行只会把真正在跑的那一条挤得看不见 ✗。
    // ⚠️ 2026-10-15（①-7）：这道门原来只看"**没传过用户数据**"✗ —— 可这种槽**照样每圈被
    //    ffrdp_tick_conn 跑一遍** ✓：它的 counter_upd_cnt / 期限分档 / 每圈产出 全都在涨 ✓，
    //    却被这一行整块挡住 ✗。真机 + 回环实测（低层 fire 98.9 万 ✗，而被 dump 的那条连接
    //    只跑了 4.8 万 ✓）⇒ 差的那 94 万就落在这种"看起来空闲"的槽上 ⇒ **账被这行挡住了** ✗。
    //    所以"空闲"必须在**工作计数**上也为零 ✓ —— 只要它跑过圈、或期限模型给过任何一档，
    //    就必须打全块 ✓。
    if (c->hs_state == 0 && !(c->flags & FLAG_CONNECTED) &&
        c->send_seq == 0 && c->recv_seq == 0 &&
        c->counter_send_bytes == 0 && c->counter_recv_bytes == 0 &&
        c->counter_upd_cnt == 0 && c->counter_due_now_ack == 0 &&
        c->counter_due_now_work == 0 && c->counter_due_now_stale == 0 && c->counter_due_future == 0) {
        printf("\n---- conn %d   %s   idle: never talked about AND ran no lap (only this line) ----\n", handle, addr);
        goto done;
    }

    printf("\n---- conn %d   %s   %s%s ----\n", handle, addr, dump_hs_name(c->hs_state),
           (c->flags & FLAG_CONNECTED) ? " (connected)" : "");
    printf("total_send, total_recv: %.2fMB, %.2fMB\n"    , c->counter_send_bytes / (1024.0 * 1024), c->counter_recv_bytes / (1024.0 * 1024));
    // secs 是距上一次 dump 的秒数：刚起来的第一次 dump 可能不足一秒，别拿 0 当除数。
    printf("averg_send, averg_recv: %.2fKB/s, %.2fKB/s\n", c->counter_send_bytes / (1024.0 * MAX(secs, 1)), c->counter_recv_bytes / (1024.0 * MAX(secs, 1)));
    printf("rttm: %u, rtts: %s, rttd: %u, rto: %u\n", c->rttm, dump_u32(c->rtts, b1, (int)sizeof(b1)), c->rttd, c->rto);
    // which binary is this? the compile stamp makes a stale executable impossible to mistake for a fresh
    // one: if this time is older than the build you just ran, you are running something else.
    printf("version & build     : %s probe %d, queue ratio %u, inflight default %u, compiled %s %s\n",
           FFRDP_VERSION, FFRDP_PROBE_ENABLE, (uint32_t)FFRDP_PROBE_QUEUE_RATIO, g_inflight_max, __DATE__, __TIME__);
    // rttm is the LATEST sample (its name is misleading); rtt_min is the lowest of this interval, rtts the
    // smoothed typical latency, rttd the jitter. rto is a timer, not a delay: never read it as latency.
    printf("rtt_min, rtts, rttd : %u, %s, %u (min / typical / jitter, ms)\n",
           c->rtt_min, dump_u32(c->rtts, b2, (int)sizeof(b2)), c->rttd);
    // elapsed 是"自 init 起"（tick_ffrdp_init ✓）；括号里那个是"自上次清零"（clearhistory=1 时才会推进 ✓）。
    // 旧标签写成 dump interval ✗ —— test.c 传的是 clearhistory=0 ⇒ 两个块里它都一样大，更容易误读 ✓。
    printf("send refused before lookup: no-arg/bad-arg %u, bad-handle-or-table %u (①-24; per-connection ones are below)\n",
           ffrdp->stat_ref_noarg, ffrdp->stat_ref_noconn);
    printf("elapsed             : %d s (since init) / %d s (since last clear)\n", ((int32_t)get_timestamp32_ms() - (int32_t)ffrdp->tick_ffrdp_init) / 1000, secs);
    printf("recv_size           : %d\n"  , c->recv_size           );
    printf("flags               : %x\n"  , c->flags               );
    printf("send_seq            : %u\n"  , c->send_seq            );
    printf("recv_seq            : %u\n"  , c->recv_seq            );
    printf("wait_snd            : %u\n"  , c->wait_snd            );
    printf("rmss, smss          : %u, %u\n"    , c->rmss, c->smss);
    printf("swnd, cwnd, ssthresh: %u, %u, %u\n", c->swnd, c->cwnd, c->ssthresh);
    // 三个判据一行打完：drain 早退（它随窗口宏缩放，之前没有计数器）、cwnd 被乱序误判打回、
    // 以及被内核丢包/缓冲压力打回。计数是累计的（测试驱动的 dump 传 clearhistory=0），看增量。
    printf("ssthresh floor hits : %u (cwnd already at the floor, still recomputed => +1/ACK from here)\n", c->counter_ssth_floor);
    printf("due-now sources     : ack %u, work %u, STALE %u (stale = asks for \"now\" with nothing to do); future %u\n",
           c->counter_due_now_ack, c->counter_due_now_work, c->counter_due_now_stale, c->counter_due_future);
    printf("now by source       : frame %u, flush %u, rate %u, hs %u, keepalive %u, query %u, probe %u, other %u\n",
           c->counter_now_src[1], c->counter_now_src[2], c->counter_now_src[3], c->counter_now_src[4],
           c->counter_now_src[5], c->counter_now_src[6], c->counter_now_src[7], c->counter_now_src[0]);
    printf("stale by source     : frame %u, flush %u, rate %u, hs %u, keepalive %u, query %u, probe %u, other %u\n",
           c->counter_stale_src[1], c->counter_stale_src[2], c->counter_stale_src[3], c->counter_stale_src[4],
           c->counter_stale_src[5], c->counter_stale_src[6], c->counter_stale_src[7], c->counter_stale_src[0]);
    // ①-3：被护栏改写过的那批（旧分类会把它记进 future ✗）—— 真机服务端自旋的真身 ✓
    printf("guard-now ask       : %u (asked for \"exactly now\" => rewritten to now+1; each is a 0ms sleep if a lap takes >1ms)\n",
           c->counter_arm_eq_now);
    printf("guard-now by source : frame %u, flush %u, rate %u, hs %u, keepalive %u, query %u, probe %u, other %u\n",
           c->counter_arm_eq_src[1], c->counter_arm_eq_src[2], c->counter_arm_eq_src[3], c->counter_arm_eq_src[4],
           c->counter_arm_eq_src[5], c->counter_arm_eq_src[6], c->counter_arm_eq_src[7], c->counter_arm_eq_src[0]);
    printf("armed <=2ms         : %u (deadline within 2ms of now after accounting; the real spin indicator)\n",
           c->counter_arm_le2ms);
    printf("drain-block/backdrop: %u ticks (drain held), cwnd back %u (rto/fast) + %u (send fail)\n",
           c->counter_drain_block, c->counter_cwnd_back, c->counter_cwnd_sfail);
    printf("timeout diag        : no-ack-arrived %u, acks-arrived-but-missed %u, max wait %u ms\n",
           c->counter_to_noack, c->counter_to_hadack, c->counter_to_age_max);
    printf("peer wnd raw/clamped: %u, %u\n", c->peer_wnd_raw, c->swnd);
    printf("send starved/blocked: swnd0 %u frames, cap-block %u ticks\n", c->counter_swnd0, c->counter_capblock);
    // §26.30 ①-①：被**哪一道闸门**拒（配 counter_send_failed 看总数 ✓；total = wnd + bytes + big + 其它 ✓）
    printf("send refused by gate : frame-count %u, queue-bytes %u, too-big %u | conn-state: closing %u, not-connected %u, hs-failed %u, hs-down %u (see counter_send_failed for the total)\n",
           c->counter_refuse_wnd, c->counter_refuse_bytes, c->counter_refuse_big,
           c->counter_ref_closing, c->counter_ref_notconn, c->counter_ref_hsfail, c->counter_ref_hsdown);
    // §26.30 ①-②：等信用的延迟（episode = 从"没信用"到"真的首发一帧"✓）
    printf("credit wait per frame: %u episodes, %u ms total, %u ms max\n",
           c->counter_swnd_wait_ep, c->counter_swnd_wait_ms, c->counter_swnd_wait_max);
    // §26.30 ④-14：ack 攒了没有 / 谁在回（覆盖数 = 距上一条 ack 之间到达的全帧数 ✓）
    printf("ack coverage        : sent %u (on data %u, on tick %u), frames covered 0/1/2/>=3 = %u/%u/%u/%u\n",
           c->counter_ack_sent, c->counter_ack_ondata, c->counter_ack_ontick,
           c->counter_ack_cov0, c->counter_ack_cov1, c->counter_ack_cov2, c->counter_ack_covN);
    // §26.30 ④-15：在途闸门"紧不紧" —— 占用率（均值对均值 ✓）、挡住的圈里有没有一帧没发的 ✓
    printf("in-flight occupancy : %u laps, sum used %u, sum limit %u (per lap: used/limit; near 1.00 = the gate is the working point)\n",
           c->counter_inflight_n, c->counter_inflight_sum, c->counter_limit_sum);
    printf("cap-block detail    : limit at floor %u, above floor %u; laps blocked with 0 frames sent %u\n",
           c->counter_capb_floor, c->counter_capb_above, c->counter_capb_nosend);
    printf("cap wait per frame  : %u episodes, %u ms total, %u ms max\n",
           c->counter_cap_wait_ep, c->counter_cap_wait_ms, c->counter_cap_wait_max);
    printf("ack latency         : %u us avg, %u us max (%u acks)\n",
           c->ack_lat_n ? (uint32_t)(c->ack_lat_sum_us / c->ack_lat_n) : 0,
           c->ack_lat_max_us, c->ack_lat_n);
    {   // the delay probe: how many frames are really outstanding, what limit is enforced, and what the
        // experiment reported. rtt ref->best is the number to read: if best is well below ref the window
        // was keeping a queue alive; if they are close the delay is the path's and nothing was touched.
        FFRDP_FRAME_NODE *q; uint32_t out = 0;
        for (q = c->send_list_head; q; q = q->next) if (q->flags & FLAG_FIRST_SEND) out++;
        printf("inflight, cap       : %u, %u (kept %u; probes %u keep %u revert %u too-far %u giveback %u)\n",
               out, c->cap, c->cap_kept, c->counter_probe, c->counter_probe_keep,
               c->counter_probe_revert, c->counter_probe_narrow, c->counter_giveback);
        // step29: the brake that actually holds. it limits frames on the wire, so the round trip stops
        // carrying a queue we built ourselves; the peer's credit is left alone (it never limited anything).
        printf("in-flight limit     : %u frames (default %u; the probe lowers it when it proves a win)\n",
               c->inflight_max ? c->inflight_max : g_inflight_max,
               g_inflight_max);
        // what the probe's trigger sees at the moment it is asked: waste is retransmits per mille over the
        // last second, out is what is on the wire, and the queue test is the typical round trip against the
        // path's own floor -- that is the gate which actually earns a probe (the old "rttm >= 50ms" line
        // printed here had stopped being a gate when the queue test replaced it). the size gate is "the
        // halved ceiling is still under the one in force and above the floor", so it is read as two numbers:
        // out/2 is what a probe would ask for, and it must land under the limit on the next line.
        printf("probe trigger       : waste %u pm, rtts/rtt_min %u/%u, out %u (needs %u pm, >%ux, OR ①-39 mem: out >= %u and out/2 under the limit above)\n",
               c->probe_waste, c->rtts, c->rtt_min, out,
               FFRDP_PROBE_WASTE_PM, FFRDP_PROBE_QUEUE_RATIO, (uint32_t)FFRDP_MEMPROBE_MIN_OUT);
        // 1x is the floor itself, and that is the number to watch fall when the probe wins. rtts is
        // smoothed across intervals while rtt_min is the lowest of this one, so rtts can legitimately read
        // below the floor; printing that as 0x would read like a fault, so anything at or under it shows 1x.
        printf("queue test          : rtts %u vs rtt_min %u -> %ux (probe needs >%ux, or waste >= %u pm)\n",
               c->rtts, c->rtt_min,
               (c->rtt_min && c->rtts != (uint32_t)-1 && c->rtts > c->rtt_min)
                   ? c->rtts / c->rtt_min : 1,
               FFRDP_PROBE_QUEUE_RATIO, FFRDP_PROBE_WASTE_PM);
        // both are the same estimate -- the typical round trip before the probe and its lowest value during
        // it -- so the difference is directly the proof the probe is looking for: a fall of at least
        // FFRDP_PROBE_RTT_DROP_MS keeps the smaller window (if the confirmed rate also held).
        printf("probe rtt ref/best  : %u, %u (typical before, lowest during; a keep needs a fall of >= %u ms)\n",
               c->probe_rtt_ref, c->probe_best == 0xFFFFFFFF ? 0 : c->probe_best,
               (uint32_t)FFRDP_PROBE_RTT_DROP_MS);
        printf("confirmed rate      : %u frames/s\n", c->rate_now);
    }
    printf("fec_txredundancy    : %d\n"  , c->fec_txredundancy    );
    printf("fec_rxredundancy    : %d\n"  , c->fec_rxredundancy    );
    printf("fec_txseq           : %d\n"  , c->fec_txseq           );
    printf("fec_rxseq           : %d\n"  , c->fec_rxseq           );
    printf("fec_rxmask          : %08x\n", c->fec_rxmask          );
    printf("counter_send_1sttime: %u\n"  , c->counter_send_1sttime);
    printf("counter_send_failed : %u\n"  , c->counter_send_failed );
    // the other half of "could not send": ffrdp_send counts refusals by the library (queue full), this
    // counts refusals by the kernel (sendto did not take the datagram), over this dump interval. if this
    // is not zero, the send buffer is too small for how we burst -- and the frames affected were retried,
    // so the cost is invisible except here.
    printf("counter_send_err    : %u\n"  , c->counter_send_err    );
    printf("counter_send_query  : %u\n"  , c->counter_send_query  );
    printf("counter_resend_rto  : %u\n"  , c->counter_resend_rto  );
    printf("counter_resend_fast : %u\n"  , c->counter_resend_fast );
    // the three ratios below each need a denominator that can legitimately be zero (a sender that has
    // not sent anything yet, a receiver-only direction, a sparse sender whose frames were all short).
    // printing a number computed against a made-up denominator of 1 is worse than printing nothing:
    // it reads like a real measurement. so: n/a when the denominator is zero.
    //   ratio  —— 每次首发尝试里有多少变成了重传。不按交付归一，所以"饿着"的发送方会显得很好看；
    //   waste  —— 对端说"这一帧我早就有了"的重传，占我们自己重传总数的比例。这是链路上唯一一个
    //            本端计数污染不了的浪费判据（本端只知道自己重传了几次，不知道对端是否早就有）；
    //   perMB  —— 每交付 1MB 花了多少次重传（按交付归一，比 ratio 可信）。
    {   uint32_t resend = c->counter_resend_rto + c->counter_resend_fast;
        // ratio 与 waste 后面都跟着 (分子 / 分母)：分母小的时候（极低频发送）一眼就能看出这个百分比
        // 不能当稳态读。例如限速 1KB/s 时"重传 6 次、累计首发 20 次"会印 30.00%，而它其实只是 6/20；
        // 同理 waste 的 (6 / 6) 说明"对端说我们 6 次重传全部白费"，而不是一个笼统的 100%。
        if (c->counter_send_1sttime) printf("counter_resend_ratio: %.2f%% (%u / %u)\n",
                                                100.0 * resend / c->counter_send_1sttime, resend, c->counter_send_1sttime);
        else                             printf("counter_resend_ratio: n/a\n");
        if (resend)                      printf("counter_resend_waste: %.2f%% (%u / %u)\n",
                                                100.0 * c->counter_ack_dup / resend, c->counter_ack_dup, resend);
        else                             printf("counter_resend_waste: n/a\n");
        if (c->counter_send_bytes)   printf("counter_resend_perMB: %.1f\n"  , resend * 1048576.0 / c->counter_send_bytes);
        else                             printf("counter_resend_perMB: n/a\n");
    }
    printf("counter_reach_maxrto: %u\n"  , c->counter_reach_maxrto);
    printf("counter_ack_recv    : %u\n"  , c->counter_ack_recv    );
    printf("counter_ack_mack    : %u\n"  , c->counter_ack_mack    );
    printf("counter_ack_hole    : %u\n"  , c->counter_ack_hole    );
    printf("counter_recv_ooo    : %u\n"  , c->counter_recv_ooo    );
    printf("counter_recv_far    : %u\n"  , c->counter_recv_far    );
    // where out of order arrivals sat relative to the hole, and how much of that we told the peer
    // about. the three bands add up to counter_recv_ooo; the reach ratio answers the only question
    // that matters before widening the mask: how much of the out of order traffic is even addressable.
    printf("counter_ooo_d1_24   : %u\n"  , c->counter_ooo_d1_24   );
    printf("counter_ooo_d25_64  : %u\n"  , c->counter_ooo_d25_64  );
    printf("counter_ooo_dgt64   : %u\n"  , c->counter_ooo_dgt64   );
    printf("counter_ooo_d65_128 : %u\n"  , c->counter_ooo_d65_128 );
    printf("counter_ooo_dgt128  : %u\n"  , c->counter_ooo_dgt128  );
    {   uint32_t ooo = c->counter_recv_ooo;
        uint32_t r64  = c->counter_ooo_d1_24 + c->counter_ooo_d25_64;
        uint32_t r128 = r64 + c->counter_ooo_d65_128;
        if (ooo) { printf("counter_ooo_64reach : %.1f%% (%u / %u)\n" , 100.0 * r64  / ooo, r64 , ooo);
                   printf("counter_ooo_128reach: %.1f%% (%u / %u)\n" , 100.0 * r128 / ooo, r128, ooo); }
        else     { printf("counter_ooo_64reach : n/a\n");
                   printf("counter_ooo_128reach: n/a\n"); }
    }
    printf("counter_mack_bits_tx: %u\n"  , c->counter_mack_bits_tx);
    printf("counter_mack_bits_rx: %u\n"  , c->counter_mack_bits_rx);
    printf("counter_rto_backoff : %u\n"  , c->counter_rto_backoff );
    printf("counter_timeout_evt : %u\n"  , c->counter_timeout_evt );
    printf("counter_ack_una_dup : %u\n"  , c->counter_ack_una_dup );
    printf("counter_rto_head    : %u\n"  , c->counter_rto_head    );
    printf("counter_rto_chain   : %u\n"  , c->counter_rto_chain   );
    printf("counter_rto_stale   : %u\n"  , c->counter_rto_stale   );
    printf("counter_recv_dup    : %u\n"  , c->counter_recv_dup    );
    // what was thrown away instead of acted on: bad = a fragment whose length cannot describe anything,
    // drop = a well formed frame further ahead of the hole than the window we advertise. On a healthy
    // run both stay at zero; a non zero one means somebody is probing the port.
    printf("counter_rx_bad      : %u\n"  , c->counter_rx_bad      );
    printf("counter_recv_drop   : %u\n"  , c->counter_recv_drop   );
    printf("counter_ack_dup     : %u\n"  , c->counter_ack_dup     );
    // rtt_tail 是这一版"让 RTO 自己够到延迟尾部"的那个学习量（见 FFRDP_MIN_RTO 的注释）；它应当收敛到
    // 这条链路的确认延迟尾部（真机上是 85~92 ms 那一档），并在链路变好后每秒衰减 1/16。
    printf("confirm-wait hist ms: <=16 %u, <=32 %u, <=64 %u, <=128 %u, <=256 %u, <=512 %u, <=1024 %u, more %u (max %u)\n",
           c->counter_conf_16, c->counter_conf_32, c->counter_conf_64, c->counter_conf_128,
           c->counter_conf_256, c->counter_conf_512, c->counter_conf_1024, c->counter_conf_more,
           c->counter_conf_max);
    printf("rto_floor           : %u, rtt_tail %u ms\n"  , c->rto_floor, c->rtt_tail); // 白费率见上面的 counter_resend_waste（同一个量，不再打两遍）
    printf("counter_rto_behind  : %u\n"  , c->counter_rto_behind  );
    printf("lap period avgmax   : %u, %u ms (%u laps, cost max %u ms)\n",
           c->counter_upd_gap / MAX(c->counter_upd_cnt - 1, 1), c->counter_upd_gap_max,
           c->counter_upd_cnt, c->counter_upd_cost_max);
    // ①-5：每圈产出 —— sent0 就是"期限说现在有活、却一帧没发"的那类圈 ✗
    printf("lap output sent     : 0 frames %u, 1 frame %u, >=2 frames %u (0 means a lap that was asked to go now and put nothing on the wire)\n",
           c->counter_lap_sent0, c->counter_lap_sent1, c->counter_lap_sentN);
    // ①-6：TIMER 分支哪一支 × 期限模型哪一档 ✓（ack-only 不跑整圈 ⇒ 不进协议圈数 ✓）
    printf("lap work ①-26       : framed/send 0/%u 1/%u 2/%u >=3/%u | sent 0/%u 1/%u 2/%u 3-4/%u >=5/%u | stop: list-empty %u, cwnd %u, inflight-cap %u, swnd0 %u, other %u | end in-flight 0/%u 1-2/%u 3-8/%u >=9/%u\n",
           c->counter_dr0, c->counter_dr1, c->counter_dr2, c->counter_drM,
           c->counter_ls0, c->counter_ls1, c->counter_ls2, c->counter_ls3, c->counter_ls5,
           c->counter_sp_list, c->counter_sp_cwnd, c->counter_sp_cap, c->counter_sp_swnd0, c->counter_sp_other,
           c->counter_ll0, c->counter_ll2, c->counter_ll8, c->counter_llM);
    printf("timer branch        : ack-only %u, full-tick %u (ack-only runs no lap, so fires can far exceed laps)\n",
           c->counter_fire_ackonly, c->counter_fire_tick);
    printf("timer due given     : zero %u, <=now %u, now+1..4 %u, >now+4 %u (the '<=now' one asks to go now)\n",
           c->counter_fire_d0, c->counter_fire_dnow, c->counter_fire_dnear, c->counter_fire_dfar);
    printf("timeout dist ①-28A  : <=7 %u, 8-15 %u, 16-23 %u, 24-63 %u, >=64 %u, max %u (distance from ack una; the ack bitmap covers 24)\n",
           c->counter_to_d0_7, c->counter_to_d8_15, c->counter_to_d16_23, c->counter_to_d24_63, c->counter_to_dM, c->counter_to_dmax);
    printf("send wake ①-35      : calls %u, wakes %u, skipped %u (wakes/call %.2f; coalesce %s)\n",
           c->counter_send_calls, c->counter_send_wakes, c->counter_send_wake_skip,
           c->counter_send_calls ? (double)c->counter_send_wakes / (double)c->counter_send_calls : 0.0,
           g_sendwake_coalesce ? "ON" : "off");
    printf("footprint ①-35      : sizeof(conn) %u, sizeof(context) %u, sizeof(frame node) %u bytes (static part; each in-flight frame costs one node + smss)\n",
           (unsigned)sizeof(FFRDPCONN), (unsigned)sizeof(FFRDPCONTEXT), (unsigned)sizeof(FFRDP_FRAME_NODE));
    printf("memprobe ①-39       : %s (env FFRDP_MEMPROBE=0|1), fired %u of %u probes; floor %u frames, kept %u of ceiling %u (peak in-flight ~%u KB)\n",
           g_probe_mem ? "ON" : "off", c->counter_probe_mem, c->counter_probe,
           (unsigned)FFRDP_INFLIGHT_MIN, c->inflight_max ? c->inflight_max : g_inflight_max, g_inflight_max,
           (unsigned)(((c->inflight_max ? c->inflight_max : g_inflight_max) * (sizeof(FFRDP_FRAME_NODE) + 4u + FFRDP_MAX_MSS + 2u)) / 1024));
    printf("cc sites ①-29       : hole-wait %u (cut %u), rto %u, pseudo %u | same frame cut >=2: %u, max %u | hole-cut-once %s\n",
           c->counter_cc_hole, c->counter_cc_hole_cut, c->counter_cc_rto, c->counter_cc_pseudo,
           c->counter_cut_multi, c->counter_cut_max, g_hole_cut_once ? "ON" : "off");
    printf("pseudo-nocut ①-28B  : %s (env FFRDP_PSEUDO_NOCUT=0|1), times it would have halved: %u\n",
           g_pseudo_nocut ? "ON (pseudo-timeouts do not halve cwnd)" : "off (pseudo-timeouts halve, as before)", c->counter_pseudo_nocut);
    printf("gate knobs ①-27     : inflight %u, max_cwnd %u, waitsnd %u (env FFRDP_INFLIGHT/MAXCWND/WAITSND; source defaults 64/128/256)\n",
           g_inflight_max, g_max_cwnd, g_waitsnd);
    printf("unsent ①-41         : budget %u frames (env FFRDP_UNSENT; 0 = off), now %u, wait_snd %u (this is what took over the app queue's burst shaping)\n",
           g_unsent, c->tx_unsent, c->wait_snd);
    printf("ack mode            : %s (env FFRDP_ACK_COALESCE=%d; 0 = per-frame on receive, 1 = one per lap)\n",
           ffrdp_ack_coalesce() ? "coalesced (one per lap)" : "immediate (on receive)", ffrdp_ack_coalesce());
    printf("counter_txfull      : %u\n"  , c->counter_txfull      );
    printf("counter_txshort     : %u\n"  , c->counter_txshort     );
    printf("counter_rxfull      : %u\n"  , c->counter_rxfull      );
    printf("counter_rxshort     : %u\n"  , c->counter_rxshort     );
    printf("counter_fec_tx      : %u\n"  , c->counter_fec_tx      );
    printf("counter_fec_rx      : %u\n"  , c->counter_fec_rx      );
    printf("counter_fec_ok      : %u\n"  , c->counter_fec_ok      );
    printf("counter_fec_failed  : %u\n\n", c->counter_fec_failed  );
done:
    if (secs > 1 && clearhistory) {
        ffrdp->tick_ffrdp_dump = get_timestamp32_ms();
        memset(&c->counter_send_bytes, 0, (uint8_t*)&c->reserved - (uint8_t*)&c->counter_send_bytes);
    }
}

void ffrdp_dump(void *ctxt, int conn, int clearhistory)
{
    FFRDPCONTEXT *ffrdp = (FFRDPCONTEXT*)ctxt;
    if (!ffrdp) return;
    // conn >= 0 ⇒ 只打那一条（该连接的详细状态 ✓）；conn < 0 ⇒ 表里每条一块 ✓。
    // 旧注释写着"现在与只印 0 号等价"✗ —— 那是单连接时代的说法 ✓，v2 有多连接，必须按 conn 分 ✓。
    if (conn >= ffrdp->nconn || (conn >= 0 && !ffrdp->table[conn])) return;
    // 要打印的全是协议状态，而协议现在跑在工作线程里：拿住同一把锁再读，打印出来的一整套数不会是被
    // 拆散的一半（这正是旧版单线程下不用操心、换线程后必须补上的一件小事）。
    pthread_mutex_lock(&ffrdp->lock);
    if (conn < 0) printf("%s---- connection table: %d slots, one block each below ----\n", (ffrdp->nconn > 1 ? "\n" : ""), ffrdp->nconn);
    // ①-9：入口分档 —— 与低层 fire 对照 ⇒ 多出的回调是哪种类型 / 是否发生在 nconn==0 时 ✓
    if (conn < 0) printf("---- lowlevel callbacks into protocol: timer %u, data %u, other %u | entry nconn==0: %u, sum %u ----\n",
                          ffrdp->cb_timer_calls, ffrdp->cb_data_calls, ffrdp->cb_other_calls,
                          ffrdp->cb_nconn_zero, ffrdp->cb_nconn_sum);
    // ①-18：arm_timer 取 min 之后的 due 分档 + 赢家（把"每连接 due"与"低层收到值"中间那一跳补上 ✓）
    if (conn < 0) {
        int w;
        printf("---- arm_timer due: calls %u, zero %u, <=now %u, now+1..4 %u, >now+4 %u | winner slots:",
               ffrdp->arm_calls, ffrdp->arm_d0, ffrdp->arm_dnow, ffrdp->arm_dnear, ffrdp->arm_dfar);
        for (w = 0; w < FFRDP_MAXCONN; w++) if (ffrdp->arm_win[w]) printf(" %d=%u", w, ffrdp->arm_win[w]);
        printf(" | ack-override %u ----\n", ffrdp->arm_ack_win);
    }
    ffrdp_dump_locked(ffrdp, conn, clearhistory);
    pthread_mutex_unlock(&ffrdp->lock);
}
