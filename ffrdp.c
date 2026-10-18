#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include "utils.h"
#include "lowlevel.h"
#include "ffrdp.h"

/* ══ FFRDP_DIAG：诊断/调试代码的总开关（默认 0）═════════════════════════════
 * 0（默认 · release / 嵌入式）：dump、①-xx、以及“只给 dump 用”的计数器全部**编译掉** ✓
 *   ⇒ 无 printf、无静态内存、无计数开销；逻辑真正要读的量不受影响 ✓。
 * 1（dev / 测试）：完整仪表，行为与加开关之前逐字相同 ✓。
 * 构建：build.sh 与 tools/sim/run.sh 都带 -DFFRDP_DIAG=1（开发）；出货去掉该宏即可。 */
#ifndef FFRDP_DIAG
#define FFRDP_DIAG 0
#endif

#if FFRDP_DIAG
/* 计数器写点（DIAG=1 时与 `c->f++` 等逐字同义 ✓；DIAG=0 时整条消失 ✓） */
#define FFRDP_CNT(c, f)          ((c)->f++)
#define FFRDP_CNT_ADD(c, f, v)   ((c)->f += (v))
#else
/* 冒空：但要“引用”参数 ✓ —— 只用它们算计数器的局部变量（如 now_src 的下标 tag ✓）
   否则 DIAG=0 时那些变量会变成 set-but-not-used ⇒ -Werror 挡下来 ✗。 */
#define FFRDP_CNT(c, f)          ((void)(c))
#define FFRDP_CNT_ADD(c, f, v)   ((void)(c), (void)(v))
#endif

// ── 帧格式 ───────────────────────────────────────────────────────────────
// 帧头 4 字节（类型/长度）+ FEC 帧的 2 字节 fec seq；MSS 由传输层一帧上限派生，向下对齐 4 字节。
#define FFRDP_FRAME_HEAD      4
#define FFRDP_FRAME_FEC_TAIL  2
#define FFRDP_FRAME_OVERHEAD  (FFRDP_FRAME_HEAD + FFRDP_FRAME_FEC_TAIL)

// ── 收 / 发两个 MSS，**必须分开** ─────────────────────────────────────────
// 背景：lowlevel 是 UDP 实现时，线上字节 = 本层帧 + UDP 8 + IPv4 20 = 帧 + 28。
//   ⇒ 只按"传输层一帧上限"建帧（1500）时，线上是 1524 > 1500（以太网 MTU）⇒ **每个满帧都被
//     IP 分片** ✗ —— 丢任一分片 = 整个数据报报废、空口上还要多付一次每包开销 ✗。
//     lowlevel.h 的注释本来就说要"给 UDP 头留位置"，只是漏算了 IP 头那 20 字节 ✗。
//
// FFRDP_MAX_RX_MSS：**能收多大**，从传输层一帧上限派生，**不能缩** ✗。
//   收包节点按它建（见 RECV 回调里的 `if (n > node->size) n = node->size` ⇒ 超了是**静默截断** ✗）；
//   缩了它，老对端（仍按 1492 发）的满帧会被悄悄切掉尾巴 ⇒ 流被静默改坏 ✗。
// FFRDP_MAX_MSS：**发多大**，再扣掉网络头并对齐 4 字节（FEC 的 XOR 循环按 4 字节走 ✓）。
//   取 1464 而不是 1468，是为了让 **FEC 帧**也刚好不出 MTU：
//     普通满帧 4 + 1464 + 28 = 1496 ✓；FEC 帧 4 + 1464 + 2 + 28 = 1498 ✓ —— 都在 1500 以内 ✓。
#define FFRDP_NET_HEADER_RESERVE 28   // UDP 8 + IPv4 20
#define FFRDP_MAX_RX_MSS      ((FFRDP_LOWLEVEL_FRAME_MAX - FFRDP_FRAME_OVERHEAD) & ~3)   // 1492
#define FFRDP_MAX_MSS         ((FFRDP_MAX_RX_MSS - FFRDP_NET_HEADER_RESERVE) & ~3)       // 1464
#define FFRDP_ETHERNET_MTU    1500
#if (FFRDP_MAX_MSS + FFRDP_FRAME_HEAD + FFRDP_FRAME_FEC_TAIL + FFRDP_NET_HEADER_RESERVE) > FFRDP_ETHERNET_MTU
#error "满 FEC 帧 + UDP/IP 头超过以太网 MTU ⇒ 会 IP 分片：该缩 FFRDP_MAX_MSS（或这条链路上本来就有 jumbo）"
#endif

// ── 定时与窗口 ───────────────────────────────────────────────────────────
#define FFRDP_MIN_RTO        16   // RTO 硬下限（ms）；实际 RTO 由 rtt_tail 学到延迟尾部
#define FFRDP_MAX_RTO        2000
// 单次 RTT 样本相对平滑值的上限（4×rtts + 这么多 ms）。见 ffrdp_rtt_sample：样本是"首发→被确认"，
// 排在洞后面的帧要等洞修好，那个时长是**修复时间**不是往返时间 ✗（§26.108）。
#define FFRDP_RTT_SAMPLE_CLAMP_MS 100
// ①-45：上面那个 4×rtts 的上限是自我参照的 ⇒ 被污染的样本流能把它一级一级顶上去（棘轮）。再拿
// rtt_min 兜一个绝对上限 `4×rtt_min + CLAMP_MS`（物理排队 = 窗口/速率）。0 = 老行为（只按 rtts 钳）。
#ifndef FFRDP_RTT_CLAMP_BY_MIN
#define FFRDP_RTT_CLAMP_BY_MIN 1
#endif
#ifndef FFRDP_MAX_WAITSND
#define FFRDP_MAX_WAITSND    256  // 受理侧的帧数闸；必须 >= 在途上限与 max_cwnd，否则它比协议闸门更紧
#endif
#define FFRDP_QUERY_CYCLE    500
// 半帧（还没攒满 smss 的那个节点）什么时候发走 = **距最后一次追加字节**超过 IDLE ✓。
// 音视频要的就是这个：一帧的尾巴不该等着被下一帧填满（25 fps ⇒ 白等 40 ms ✗），更不该等半秒 ✗。
// 判据是"应用停手了"，不是"每 N ms 冲一次" ⇒ 持续供数据的流里这个条件**不会成立** ✓
// （`cur_new_tick` 每次追加都刷新，notes §26.95）⇒ 对饱和吞吐没有影响 ✓。
// 想回到改造前的行为（一直等到 500 ms 或被下一条填满）就把 IDLE 设成 500 ✓。
#define FFRDP_FLUSH_IDLE     2
#define FFRDP_DEAD_TIMEOUT   5000
#define FFRDP_MIN_CWND_SIZE  1
// 拥塞避免的步长档位（0 = 老行为，默认不动 ✓）：
//   0 ⇒ 每个 ACK +1 帧（≈ 每 RTT ×2）。窗口会一路顶到 inflight_max，**即使应用限速也一样** ——
//        实测 SPEED=400KB/s（≈320 fps，BDP≈64 帧）时 cwnd 仍涨到 256 = 4×BDP ⇒ 常驻排队 ≈600ms
//        ⇒ 实际 RTT 超过在途帧的 RTO(318ms) ⇒ 伪超时风暴 ⇒ cwnd/ssthresh 双双塌到 1 ⇒ 双向静默 ⇒ 重连
//        （W=256 每 ~60s 必崩，120s 重连 2 次；限速只把首次崩溃从 55s 推后，不解决）。
//   1 ⇒ 每 RTT 只 +1 帧（标准 CA：要攒够 ≈cwnd 个 ACK）⇒ 窗口贴着 BDP 停住，不再自造队列 ✓
#ifndef FFRDP_CA_RTT_STEP
#define FFRDP_CA_RTT_STEP     0
#endif
// ssthresh 地板（帧，0 = 老行为 = 地板 1）。连续 RTO 会把它反复减半：256→128→…→1；
// 到了 1 之后每个 ACK 才 +1，200ms 链路上要 ≈255 个 RTT（≈51s）才爬回 256 ⇒ 期间再超时就重来。
// 给 4~8 帧的地板，恢复时间从"几十秒"降到"几百毫秒" ✓
#ifndef FFRDP_SSTHRESH_FLOOR
#define FFRDP_SSTHRESH_FLOOR  0
#endif
// 排队时不再加窗（0 = 老行为。）实测根因：满速上传时**发送方自己的 TX 队列**把回程 ACK 一起压住，
// 于是**对端**测到的 RTT 从 200ms 涨到 1.1~2.4s（本端仍是 200ms ⇒ 方向不对称，只有上行被压）⇒
// 对端 RTO 顶到 2000、cwnd 塌到地板、rate 掉到 3~20 fps ⇒ 静默 10s ⇒ 重连（W=256 不限速两轮皆然）。
// 1 ⇒ rtts 明显高于 rtt_min（有常驻队列）时 cwnd 不再涨：窗口停在"刚开始排队"那一点，
//     TX 队列因此不会堆到秒级，ACK 也就不会被压住 ✓。不涨的时间窗里靠既有 RTO/快速重传兜底。
#ifndef FFRDP_CA_NOQUEUE
#define FFRDP_CA_NOQUEUE      0
#endif
#ifndef FFRDP_CA_NOQUEUE_SLACK
#define FFRDP_CA_NOQUEUE_SLACK 25   // ms：rtts 高出 rtt_min 这么多就算"有队列"（200ms 链路的抖动余量）
#endif
#define FFRDP_SSTHRESH_MIN    MAX(FFRDP_MIN_CWND_SIZE, (uint32_t)(FFRDP_SSTHRESH_FLOOR))
#define FFRDP_DEF_CWND_SIZE  32
#ifndef FFRDP_MAX_CWND_SIZE
#define FFRDP_MAX_CWND_SIZE  128
#endif
// ── ①-42（已否决，留档）：越过 cwnd 去修"表头后面"的重传 = 负优化 ✗ ─────────────
// 想法：发送循环的边界是 `i < cwnd`，重传也在里面 ⇒ cwnd 塌到 1 时一圈只能碰表头一帧，
//       于是"排在洞后面的帧"永远等不到重传（1018-191359：cwnd 1、max wait 16~21 s）。
// 实测否决（lo，同一份代码只切这一个开关，3 轮 × 8 s）：
//   on  368.6 / 378.9 / 397.7 MB      waste 41~44%   retx_oob 400~860
//   off 420.5 / 421.5 / 411.0 MB      waste 9~14%
// ⇒ **−7~10% 吞吐**，而且 waste 翻 3~4 倍。原因也清楚了：表头 = 洞（`counter_head_ahead` 恒 0），
//   洞后面的帧本来就不是洞，重传它们只能给对端送重复数据（ACK 位图早就报了"我有"）。
//   所以 `i < cwnd` **不是** bug：它挡住的只是无用功。真要治塌陷得治"为什么一次次回 1"，见
//   FFRDP_PSEUDO_NOCUT（伪超时不砍窗）与 FFRDP_SSTHRESH_FLOOR（回退后能快些爬回来）。
#define FFRDP_MAXCONN        16   // 连接表容量；句柄 = 下标，索引永不复用（见 ffrdp.h）

// ── 握手（v2）────────────────────────────────────────────────────────────
// 只占一个帧类型号，子类型在载荷第 1 字节（加字段只扩载荷）；帧类型 35 安全：v1 只认 <=34 并静默忽略。
#define FFRDP_FRAME_TYPE_HANDSHAKE 35
#define FFRDP_HS_SYN      1
#define FFRDP_HS_SYN_ACK  2
#define FFRDP_HS_ACK      3
#define FFRDP_HS_FIN      4
#define FFRDP_HS_RETRY_MS 500   // 重传间隔（总时限用配置的 handshake_ms）
// 握手状态（每条连接）；0 = 无握手
#define HS_SYN_SENT 1   // 客户端发了 SYN，等 SYN-ACK
#define HS_SYN_RCVD 2   // 服务端发了 SYN-ACK（半开），等 ACK
#define HS_UP       3   // 已建立
#define HS_FIN_WAIT 4   // 我方已发 FIN，等对端 FIN
#define HS_FAILED   5   // 握手超时 ⇒ 此后 send/recv 按"已断"处理

// ── 在途闸门：真正管住"线上同时有几帧"的量 ───────────────────────────────
// 对端通告的窗口当不了闸门（每次 ack 都重新发一遍 ⇒ 不约束任何东西）。
#ifndef FFRDP_INFLIGHT_MAX
#define FFRDP_INFLIGHT_MAX   128   // 与 FFRDP_RX_WINDOW_FRAMES（128）对齐：窗口比闸门大就等于闸门白设 ✓
#endif
// B2（§8 #4）：闸门能"升"的天花板。默认 64 是**没有证据时**的保守值，而"顶死 + 没队列 + 没浪费"
// 三条都由测量给出时，可以升到这里。再往上是 cwnd（FFRDP_MAX_CWND_SIZE 128）、受理侧 wait_snd
// 以及对端通告的窗口（swnd）三道别的闸门 —— 过 CEIL 不再有意义。
#ifndef FFRDP_INFLIGHT_CEIL
#define FFRDP_INFLIGHT_CEIL  128
#endif
#define FFRDP_INFLIGHT_MIN   4    // 探测能下探到的地板

// ── 编译期实验开关（默认即生产值；改这里重编）─────────────────────────────
#define FFRDP_MEMPROBE_ENABLE   1  // 探测的"内存视角"触发：上限远高于实际用量也值得下探（0 = 关）
#define FFRDP_MEMPROBE_MIN_OUT  8u // 在途没到这么深就不问（否则一路问到地板，变成抖动）
#define FFRDP_UNSENT_MAX        24 // 未发出帧预算（接管原应用队列的突发整形；0 = 关）
#ifndef FFRDP_PSEUDO_NOCUT
#define FFRDP_PSEUDO_NOCUT      0  // 伪超时是否减半（0 = 与快速重传同档，原行为）；1 = 不动窗口（A/B 用 EXTRA_CFLAGS ✓）
#endif
#define FFRDP_HOLE_CUT_ONCE     1  // "洞等够久"分支：同一帧只许减半一次
// ①-35c：send 的叫醒合并（1 = 队列"由空变非空"才叫醒工作线程 ✓；0 = 每条都叫一次 ✗）。
// 转正依据（notes §26.55）：wakes/call 0.87 → 0.25、吞吐 +10%、**零内存** ✓；
// 与 drain 同锁 ⇒ 标志的置/清不会交错 ⇒ 不漏醒 ✓（突发 1 时省得少 ~7%、突发 8 时省钱 62% ✓）。
#define FFRDP_SENDWAKE_COALESCE 1

// ── C1（§5）：批级整形 = **ms 令牌桶** ────────────────────────────────────────
// 0 = 关；1 = 开（**默认开** ✓ —— 2026-10-18 真机窗口扫描定档）。设计约束：
//   · 不开整形时 W≥128 会"每 RTT 打一梭子 + 静默"（锯齿形状）⇒ 200ms 链路上 W=128/192 出现 2~4 次重连、
//     重传 2.5~6.3%；开整形后同样两档 **0 次重连**、重传 0.07~0.11% ⇒ 整形修的是形状，不是均值速率 ✓。
//   · 平台可能没有微秒时钟 ⇒ 令牌按**整数毫秒**补充（FFRDP_PACE_MS = 1），只需 ms 级期限 ✓。
//   · 补充速率 = 在途上限 / RTT（不用 rate_now：那会掉进"整形一压、速率跟着降"的反馈陷阱 ✗）。
//   · ⚠️ 桶深必须 ≥ 2 帧：小于 1 帧时令牌永远够不到 16 而**死锁**（W=96 实测 5 次重连，见 refill 处注释）。
#ifndef FFRDP_PACE_ENABLE
#define FFRDP_PACE_ENABLE     1
#endif
#if FFRDP_PACE_ENABLE
#ifndef FFRDP_PACE_MS
#define FFRDP_PACE_MS         1     // 令牌补充周期（ms）
#endif
#ifndef FFRDP_PACE_RATE_PCT
#define FFRDP_PACE_RATE_PCT   125   // 补充速率 = 实得速率 × 125%（25% 余量 ✓）
#endif
#ifndef FFRDP_PACE_BURST_MS
#define FFRDP_PACE_BURST_MS   2     // 桶深 = 2 个周期的量（攒多了又变回突发 ✗）
#endif
#ifndef FFRDP_PACE_MIN_FPS
#define FFRDP_PACE_MIN_FPS    64    // RTT 还没采样到时的下限（≈82KB/s）—— 只在最初几毫秒起作用 ✓
#endif
#ifndef FFRDP_PACE_MIN_RTT
#define FFRDP_PACE_MIN_RTT    5     // rtts 低于这个值（ms）⇒ **不整形**（锯齿的周期就是 RTT，几百微秒的
                                    // 尺度上它不构成问题；而整形的兑现靠 ms 级期限，平台唤醒是几毫秒
                                    // ⇒ 只剩代价。lo 实测 47.3 → 35.9 MB/s，见 notes §26.99 ✓）
#endif
#ifndef FFRDP_PACE_RTT_CAP_X
#define FFRDP_PACE_RTT_CAP_X  4     // 用于整形的 RTT 最多取 rtt_min 的这么多倍（0 = 老行为：直接用 rtts）
#endif
#endif /* FFRDP_PACE_ENABLE */
// 关掉时也要能写进同一行判据 ⇒ 用一个恒假宏（免得满屏 #if ✓）。
#if FFRDP_PACE_ENABLE
#define FFRDP_PACE_BLOCKED(c)   ((c)->pace_due != 0)
#else
#define FFRDP_PACE_BLOCKED(c)   0
#endif

// ── FEC（前向纠错）：编译期特性 ─────────────────────────────────────────────
// 0 = **编译掉**（默认 ✓）：收发的两个 1498 B 缓冲一起消失 ⇒ 连接结构体 3400 → 404 B ✓
//     （16 条连接省 48 KB ✓）。⚠️ 代价说清：对端若开了 FEC，它的冗余帧我们**不修** ✓
//     （数据帧照常收 ✓）；配置串里的 fec:N>0 会被忽略并提示一次 ✓（帧型按无冗余走 = 现状 ✓）。
// 1 = 全功能：fec:N 生效 ✓（判据 / dev 构建用它 ✓，见 build.sh）。
#ifndef FFRDP_FEC_ENABLE
#define FFRDP_FEC_ENABLE 0
#endif

// ── 延迟探测：把在途砍半，看 RTT 跟不跟着降；确认速率没掉就保留更小的上限 ──
#define FFRDP_PROBE_ENABLE          1
#define FFRDP_PROBE_HOLD_MS         500 // 保持时长（= 两个 RTT + 这些）
#define FFRDP_PROBE_RTT_DROP_MS     2   // rtts 至少要降这么多 ms 才算赢（绝对值；相对值会被单次采样带偏）
// "没队列"的**容差**（与上面那个"要降多少"不是一回事 ✓）：绝对 2 ms 在 200 ms 链路上只占 1%，
// 链路上一点抖动就把判据钉死在假 —— 2026-10-17 真机 200 ms 复测里 B2 因此一次都没升 ✗。
// 取 max(2 ms, RTT/16 ≈ 6.25%)：20 ms 链路还是 2 ms（与以前逐字节相同 ✓），200 ms 链路放宽到 12 ms ✓。
#define FFRDP_PROBE_IDLE_SLACK(floor)  MAX((uint32_t)(floor) / 16u, (uint32_t)FFRDP_PROBE_RTT_DROP_MS)
#define FFRDP_PROBE_RATE_KEEP_PCT   85  // ... 同时确认速率不低于历史最好值的这个百分比
#define FFRDP_PROBE_GIVEBACK_IDLE_S 5   // giveback 要先"连续健康"这么多秒（还的速度不快过证明的速度）
#define FFRDP_CAP_RISE_IDLE_S       5   // B2：闸门"能升"要先连续这么多秒"顶死 + 没队列 + 没浪费" ✓
#define FFRDP_PROBE_QUEUE_RATIO     3   // 排队判据：rtts/rtt_min 超过这个倍数才值得问
// ①-46：`rtts/rtt_min > 3` 这个**比值**在"地板极小、排队不是我们造成的"链路上恒真 —— 1018-202512 实测
// rtt_min 1~4 ms、rtts 7~109 ms（比值 3~100），可用于是每次都触发探测，把 cap 一路棘轮到 5~34 帧：
//   conn 2: cap 15（发送 24.9 MB/120 s）   conn14: cap 5（只发出 4.6 MB，是 conn1 的 1/8）
// 而同一时刻真正在线上只有 10~15 帧 ⇒ 我们最多能造出 out×1000/rate ≈ 12~19 ms 的队列，剩下的 70~90 ms
// 是 WiFi 空口/别的流/ack 聚合的 ⇒ 把窗口砍半分不到一点带宽，只是让探测量到了别人的队列 ✗。
// 改法：排队的尺子换成**物理量**——我们自己能造出的排队 = 在途帧 / 确认速率（ms）；只有"多余延迟
// 能被我们自己的窗口解释"（excess ≤ 2×q_ours）且 q_ours 本身够深（≥ MIN_MS）才触发探测。
// 0 = 老行为（纯比值），保留只为 A/B ✓。
#ifndef FFRDP_PROBE_QUEUE_BY_MODEL
#define FFRDP_PROBE_QUEUE_BY_MODEL  1
#endif
#if FFRDP_PROBE_QUEUE_BY_MODEL
#define FFRDP_PROBE_QUEUE_MIN_MS    10  // 我们自己能造出的排队至少这么深，砍半才有可测的效果
#endif
// ①-46b：窗口根本没顶死（`out` 远小于 `cap`）时，把 cap 砍半纯属误伤：`cap_want = out/2` 会把上限
// 压到"当前在途的一半"，而当前在途本来就不是瓶颈。要求 out 至少到 cap 的 3/4 才允许开始探测 ✓。
#ifndef FFRDP_PROBE_NEEDS_FULL_WINDOW
#define FFRDP_PROBE_NEEDS_FULL_WINDOW 1
#endif
#define FFRDP_PROBE_WASTE_PM        5   // ... 或重传强度（每千次首发）到这个数
#define FFRDP_PROBE_GAP_MS          10000
#define FFRDP_PROBE_GAP_MAX_MS      60000
#define FFRDP_PROBE_HEALTHY_PCT     1   // 浪费低于这个百分比就把窗口还一点

// ── 两条"别让窗口替别人背锅"的判据（§26.105）──────────────────────────────────
// ① 本地 sendto 被拒（非阻塞 socket 的发送缓冲满 ⇒ EWOULDBLOCK）要不要当拥塞证据。
//    0 = 不当 ✓：只记账，那一帧留在表里下一圈原样再发。这是 6 客户端压满本机时 cwnd 被连续打到 1、
//    继而出现 2 s 无字节、应用层判 lost 重连的主因 ✗（1018-163155：4199 vs 180）。
//    1 = 旧口径（cwnd=1 + ssthresh 砸地板），保留这个值只为 A/B ✓。
#ifndef FFRDP_SENDFAIL_CUTS_CWND
#define FFRDP_SENDFAIL_CUTS_CWND        0
#endif
// ② 探测要"保留更小的在途上限"时，是否**要求凭证**：RTT 真降了（且有速率兜底）或重传强度真降了。
//    0 = 退回 2026-10-15 的口径（只看"速率没掉"）✗ —— 在"应用/CPU 才是瓶颈"的链路上，把在途砍半
//    交付速率当然不掉 ⇒ 每一刀都被 keep ⇒ 上限一路棘轮到地板上限（实测 7 / 15 帧），窗口小了却没买来东西 ✗。
#ifndef FFRDP_PROBE_KEEP_NEEDS_EVIDENCE
#define FFRDP_PROBE_KEEP_NEEDS_EVIDENCE 1
#endif
#if FFRDP_PROBE_KEEP_NEEDS_EVIDENCE
// 证据②（"重传强度降了"）的统计门：窗口里交付不足这么多帧、或每千帧重传数没绝对降这么多，都不算证据 ✓
#define FFRDP_PROBE_EVID_MIN_DEL    64
#define FFRDP_PROBE_EVID_DROP_PM    25
#endif

// ── RTO 地板的学习（把"对端其实早就有"的超时学进 RTO；见 rtt_tail）──────────
#define FFRDP_RTO_FLOOR_MAX  1000 // ms：比这更长的修复延迟不值得付
#define FFRDP_RTO_WIN        128  // 证据窗里的重传数（约 1 秒一轮）
#define FFRDP_WASTE_MIN      16   // 窗口里至少这么多重传才有资格下结论
#define FFRDP_WASTE_HI       80   // 对端早就有我们的重传超过这个百分比 ⇒ RTO 太短
#define FFRDP_WASTE_LO       40   // 低于这个百分比 ⇒ 修复是真的，可以还一点地板

// ── 缓冲 ─────────────────────────────────────────────────────────────────
// 接收窗口（= 我们在 ack 里通告给对端的帧数）；必须大于在途上限并留余量，否则窗口反成瓶颈。
// ── 接收窗口（内存 ↔ 吞吐的总旋钮）──────────────────────────────────────────
// 单位是**帧**（乘 MSS 得字节 ✓）。它同时就是**通告给对端的窗口** ✓：ack 里那个 8 位字段 =
// (recv_cap - recv_size) / rmss ✓ ⇒ **缩这个数 = 缩缓冲 + 缩通告窗**（两边自动一致 ✓，不必另改逻辑 ✓）。
// 默认 128（2026-10-18 真机窗口扫描定档 ✓：W=128 + 令牌桶在 200ms 链路上 120s 零重连；
// 96 偏保守、192 起单方向被饿（收 0.47 → 0.19 MB/s）、256 在**不限速**时必崩 ⇒ 见 FFRDP_PACE_ENABLE 那段）。
// 嵌入式几 KB 的预算就调小这里 ✓（16 帧 ≈ 23.8 KB ✓）。⚠️ 调小只影响**吞吐**，不影响正确性 ✓。
#ifndef FFRDP_RX_WINDOW_FRAMES
#define FFRDP_RX_WINDOW_FRAMES 128
#endif
#if FFRDP_RX_WINDOW_FRAMES < 2
#error "FFRDP_RX_WINDOW_FRAMES 太小（< 2）：窗口至少要让一个在途帧 + 一次握手回环转起来 ✓"
#endif
#define FFRDP_RECVBUF_SIZE  (FFRDP_RX_WINDOW_FRAMES * (FFRDP_MAX_RX_MSS + 0))   // 收侧容量 ⇒ 按收 MSS 算 ✓（用发 MSS 会缩掉窗口）
// 乱序帧最多能越过洞多远（再远就丢 —— 否则陌生人能用无会话的数据报无限占我们内存）。
#ifndef FFRDP_RECV_QUEUE_MAX
#define FFRDP_RECV_QUEUE_MAX 256   // ⚠️ 必须 >= 通告窗（RX_WINDOW_FRAMES），否则大窗口下"合法乱序"会被当谎言丢掉 ✗
#endif
// 应用侧等待时即使调用方没给期限也周期醒来：不漏掉状态变化，也不让调用方永久睡眠。
#define FFRDP_COND_WAIT_MS   200

// 回调里的"冻结时钟"：协议一次回调就是一圈 tick，时间在整圈里本来就该是同一个值，而取值点有几十处
// （lo 60MB/s 实测 clock_gettime 占整进程 4.4% ✓）。冻结只作用于**跑回调的那个线程**（TLS ✓）：
// 应用线程的 ffrdp_send/ffrdp_recv 超时判断、以及 lowlevel 自己的期限计算，读到的仍然是真钟 ✓。
static __thread uint32_t ffrdp_now_ms;
static __thread int      ffrdp_now_frozen;
#define NOW_MS() (ffrdp_now_frozen ? ffrdp_now_ms : get_timestamp32_ms())

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
    #define FLAG_RESENT         (1 << 3) // 这一帧被**重传过**（RTO 或快速，两者任一）⇒ 它的"首发→确认"时长不再是 RTT 样本 ✗
    uint32_t flags;        // frame flags
    uint32_t tick_1sts;    // frame first time send tick
    uint32_t tick_send;    // frame send tick
    uint32_t tick_timeout; // frame ack timeout tick
    uint32_t ticket;       // 首发时的 counter_ack_recv 快照：超时时用来分辨"等待期间 ack 到没到"。
                           // 不是临时调试：它是 timeout diag 与 rtt_tail 采样条件的依据，保留。
} FFRDP_FRAME_NODE;

// ── 标志位 ───────────────────────────────────────────────────────────────
#define FLAG_CONNECTED   (1 << 1)
#define FLAG_FLUSH       (1 << 2)
#define FLAG_ACK_DUE     (1 << 3) // 收到过需要应答的帧、还没回；由 tick 在这一段结束时统一回一个
#define FLAG_QUERY_PROBE (1 << 4) // A′：尾部探测已发出、还在等它那个 ack（见 tick 里那段）

// ── 每连接状态 ───────────────────────────────────────────────────────────
// v1 单连接的全部协议状态；v2 起是连接表里的一条（句柄 = 表下标）。末尾的 diag 段是 dump 专用计数器。
typedef struct {
    // 接收缓冲按需分配（容量 = FFRDP_RECVBUF_SIZE）：服务端那条内嵌主连接从不用于数据 ⇒ 不占它。
    uint8_t *recv_buff; uint32_t recv_cap; // 接收窗口缓冲（容量 = FFRDP_RECVBUF_SIZE）
    // 接收环形缓冲的读写游标。①-41 起应用线程在 ffrdp_send 里直接成帧，这里不再有应用队列。
    int32_t  recv_size, recv_head, recv_tail;
    uint32_t flags;
    uint64_t peer_addr; // 对端地址：客户端建连时从参数解出，服务端从第一个收到的包学会

    FFRDP_FRAME_NODE *send_list_head;
    FFRDP_FRAME_NODE *send_list_tail;
    FFRDP_FRAME_NODE *recv_list_head;
    FFRDP_FRAME_NODE *recv_list_tail;
    FFRDP_FRAME_NODE *cur_new_node;
    uint32_t          cur_new_size;
    uint32_t          cur_new_tick;
    uint32_t send_seq;   // send seq
    uint32_t recv_seq;   // send seq
    uint32_t wait_snd;   // data frame number wait to send
    uint32_t tx_unsent;  // ①-41：已成帧、但还没上过线（FLAG_FIRST_SEND 未置）的帧数 ✓
    uint32_t tx_stalled; // ①-41e：上一圈"有活却被闸门挡住、一帧没发" ⇒ next_due 别急着要"现在" ✓
    uint32_t rttm, rtts, rttd, rto;
    uint32_t rmss, smss, swnd, cwnd, ssthresh;
    uint32_t cwnd_acc;            // FFRDP_CA_RTT_STEP：拥塞避免的分数累加器（攒够 ≈cwnd 个 ACK 才 +1 帧）
    uint32_t peer_wnd_raw;        // 对端最近一次广告的窗口（夹之前的原值）：用来区分"我们夹的"与"对端给的"
    uint32_t counter_swnd0;       // 有帧等着首发、却没有信用的 tick 数：发送被饿死的直接证据
    uint32_t counter_capblock;    // 有帧等着首发、却被在途闸门挡住的 tick 数
    uint32_t counter_pace_wait;   // C1：有帧等着首发、却被**令牌桶**挡住的次数（按帧 ✓）
    uint32_t rto_floor;           // persistent lower bound on the rto, moved only by the peer's duplicate reports
    uint32_t rtt_tail;            // 学到的"确认延迟尾部"（限速增长、每秒衰减）：让 RTO 覆盖 rtts/rttd 看不见的那一档
    uint32_t win_need, win_dup;   // retransmits attempted in this evidence window, and how many the peer called pointless
    uint32_t tick_win;            // when the current evidence window was opened
    uint32_t hole_wait;           // ms a frame must be outstanding before a reported hole is believed
    uint32_t tick_recv_ack;
    uint32_t tick_send_query;
    uint32_t tick_rto_backoff; // tick of last rto backoff, rto backs off once per round

#if FFRDP_FEC_ENABLE
    uint8_t  fec_txbuf[4 + FFRDP_MAX_MSS + 2];    // 发送侧 XOR 累加器（每个 1498 B ✗ 占比 88%）
    uint8_t  fec_rxbuf[4 + FFRDP_MAX_RX_MSS + 2]; // 接收侧 XOR 累加器 / 组内首个 FEC 帧（按**收** MSS ✓：对端可以比我们发得大 ✓）
#endif
    uint8_t  fec_txredundancy, fec_rxredundancy;
    uint16_t fec_txseq;
    uint16_t fec_rxseq;
    uint16_t fec_rxcnt;
    uint32_t fec_rxmask;
    uint32_t counter_send_1sttime; // first transmissions: the denominator of counter_resend_ratio
    // 2026-10-13 诊断：收帧→回 ack 的耗时（微秒）。服务端（对端）看到的 RTT 里含这一段。
    uint32_t ack_lat_n, ack_lat_avg_us, ack_lat_max_us;
    uint64_t ack_lat_sum_us;
    uint32_t counter_resend_fast;
    uint32_t counter_resend_rto;
    uint32_t counter_txfull, counter_rxfull;
    uint32_t dup_seen;            // duplicates seen since our last ack, the value that ack carries
    uint32_t counter_ack_recv;    // ack frames received
    uint32_t rtt_min;             // lowest round trip seen this interval: the path's floor, the number to judge latency by
    uint32_t inflight_used;       // frames actually on the wire (recounted every pass): what step29 limits
    uint32_t counter_to_dmax;
    uint32_t tx_woken;            // ①-35c："已经叫过、工作线程还没跑过" ✓（①-41：改由 tick 末尾清零 ✓）
    uint32_t ack_una;             // last una seen in an ack frame
    uint32_t counter_conf_max;    // 见过的最大原始 confirm 等待（ms ✓）
    uint32_t counter_to_age_max;  // 超时时"距首发"的最大毫秒数
    uint32_t hs_state;            // 握手状态（HS_*；0 = 不走握手）—— 放 reserved 之后 ⇒ 不动清零跨度 ✓
    uint32_t tick_last_recv;      // 最后一次**听到对端**（任意帧 ✓）—— keepalive 判活用（同样放 reserved 之后 ✓）
    uint32_t hs_tick;             // 上次发握手帧的时刻（重传计时）
    uint32_t hs_start;            // 本次握手开始（或上次重传）的时刻：与 handshake_ms 比就是超时
    uint32_t hs_retry;            // 已重传次数
    uint32_t counter_hs_refused;  // 握手 SYN 没能建连接（表全满 / 内存不足）—— 与"静默失败"相反
    uint32_t handle;              // 本连接的句柄 = 代号*FFRDP_MAXCONN + 槽号（槽复用后代号变 ⇒ 陈旧句柄失效 ✓）

    // ---- 延迟探测：看到的延迟是我们自己的队列，还是链路本身的？----
    // 只能做实验回答：把在途砍半，看延迟跟不跟着降、而确认速率没掉 ⇒ 队列是我们的，保留更小的上限。
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
    // B2：闸门"能升"的那一套。只有"顶死 + 没队列 + 没浪费"连续成立才升，且升频不高于探测周期 ✓
    uint32_t cap_idle;            // 上面三条连续成立的秒数 ✓
    uint32_t tick_cap_rise;       // 上一次升的时刻 ✓
    uint32_t tick_cap_revert;     // 上一次探测 revert 的时刻（它刚说过"小了没赚" ⇒ 一个周期内不升 ✓）
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
    /* ── 诊断计数器（纯 dump 用；仅 FFRDP_DIAG=1 时存在；写点用 FFRDP_CNT*）── */
#if FFRDP_DIAG
    uint64_t counter_send_bytes;   // payload of the frames the peer acked: bytes DELIVERED, not bytes put on the wire
    uint32_t counter_send_err;     // 内核没接下的 sendto 次数（数据 / ack / query / fec 各路径合计）
    uint64_t counter_recv_bytes;   // 我们 ack 过的帧的字节数（必须 64 位：32 位累计到 4 GiB 回绕 ⇒ averg_* 印出假速率）
    uint32_t counter_send_failed;  // 受理失败：被闸门拒 / 帧分配失败（细分见下面 refuse_* 那一组）
    uint32_t counter_send_query;
    uint32_t counter_tailprobe;        // A′：RTO 的 3/4 处发出去的尾部探测 QUERY 次数
    uint32_t counter_tailprobe_lost;   // 探针的 ack 明确说"还是没有它" ⇒ 提前重传的次数（省下的就是剩下的 1/4 RTO）
    uint32_t counter_reach_maxrto;
    uint32_t counter_recv_dup;     // data frames that arrived although we already had them
    uint32_t counter_ack_dup;      // ... as reported back to us by the peer: retransmits of ours that were pointless
    uint32_t counter_txshort, counter_rxshort;
    uint32_t counter_rx_bad;       // 长度不成话的碎片（谁发的都算坏包）
    uint32_t counter_recv_drop;    // 格式没问题、但越过洞比我们通告的窗口还远 ⇒ 只丢不存
    uint32_t counter_fec_tx;
    uint32_t counter_fec_rx;
    uint32_t counter_fec_ok;
    uint32_t counter_fec_failed;
    uint32_t counter_ack_mack   ; // ack frames carrying out of order (mack) info
    uint32_t counter_ack_hole   ; // ack frames reporting a gap below the highest acked frame
    uint32_t counter_recv_ooo   ; // data frames that arrived out of order: there is a hole below them
    uint32_t counter_recv_far   ; // ... and they are more than 23 ahead of the hole, so the 24 bit ack mask cannot carry them
    // 乱序帧越过洞多远（按 ack 位图宽度分档）：决定"把位图加宽"值不值。
    uint32_t counter_ooo_d1_24  ; // 1..24：今天的 24 位位图够得到
    uint32_t counter_ooo_d25_64 ; // 25..64：要 64 位位图才够
    uint32_t counter_ooo_dgt64  ; // >64：多加一个扩展字也够不到
    // 尾巴到底越过 64 多远。注意在途闸门**限不住**这个距离：被对端位图报过的帧会离开发送表，而洞还开着。
    uint32_t counter_ooo_d65_128; // 65..128：需要第二个扩展字
    uint32_t counter_ooo_dgt128 ; // >128：两个字也够不到
    // 位图按"位"计：tx 是我们报出去的乱序位，rx 是对端报给我们的。两端对照才能分辨"没报过"与"报了没到"。
    uint32_t counter_mack_bits_tx;
    uint32_t counter_mack_bits_rx;

    uint32_t counter_rto_backoff; // times the rto was multiplied
    uint32_t counter_timeout_evt; // laps in which at least one frame timed out
    // 超时帧距 ack 的 una 多远（ack 只报 una + 24 位位图 ⇒ 超过 24 格就盖不住，只能等 una 追上来）。
    uint32_t counter_to_d0_7, counter_to_d8_15, counter_to_d16_23, counter_to_d24_63, counter_to_dM;
    uint32_t counter_pseudo_nocut;// ①-28 B："本来要减半、但被关掉了"的伪超时次数 ✓
    // 每消息固定开销的账：受理次数 / 真叫醒次数 / 被合并省掉的叫醒。
    uint32_t counter_send_calls;
    uint32_t counter_send_wakes;
    uint32_t counter_send_wake_skip;
    // 应用侧被哪一道闸门拒掉（只看 counter_send_failed 分不出是谁挡的）。
    uint32_t counter_refuse_wnd;   // 帧数闸：need + wait_snd > FFRDP_MAX_WAITSND（见 ffrdp_send 的受理判据）
    // 上面三条 gate 之外的其余拒绝出口（裸 return / goto 那几条）。
    uint32_t counter_ref_closing;  // ffrdp->closing ⇒ CLOSED
    uint32_t counter_ref_notconn;  // 服务端、对端还没连上 ⇒ CLOSED
    uint32_t counter_ref_hsfail;   // 握手超时 ⇒ CLOSED
    uint32_t counter_ref_hsdown;   // 客户端、握手还没成 ⇒ EAGAIN
    // TIMER 分支走了哪一支、期限给的是哪一档（真机 fire 远多于收帧 ⇒ 要看是谁反复要"现在"）。
    uint32_t counter_fire_ackonly;// 只回 ack 那一支（不跑整圈 ⇒ 不进协议圈数）
    uint32_t counter_fire_tick;   // 跑整圈那一支 ✓
    uint32_t counter_fire_d0;     // ffrdp_next_due 给的是 0（眼下没别的事 ✓）
    uint32_t counter_fire_dnow;   // 给的是 <= now（要求"现在"✗ ⇒ 这一档就是自旋源 ✓）
    uint32_t counter_fire_dnear;  // 给的是 now+1..4ms
    uint32_t counter_fire_dfar;   // 给的是 > now+4ms ✓（正常 ✓）
    uint32_t counter_ack_una_dup; // ack frames whose una is the same as the previous one: the peer reports no progress
    uint32_t counter_ack_una_regress; // ack frames whose una is BEHIND the previous one: the peer's delivery point moved backwards ✗
    uint32_t counter_head_ahead;  // ticks where the send-list head sits ABOVE the peer's una (>0 ⇒ "洞那一帧"已经不在表里了 ✗)
    uint32_t counter_ack_appstall;// acks 里"队首停在交付点、被应用读缓冲卡住"的次数：这些 ack **不报位图** (§26.104)
    uint32_t counter_rtt_clamp;   // RTT 样本被"4×rtts+100ms"钳住的次数（绝大多数是排在洞后面的帧，§26.108）
    uint32_t counter_rto_head ;   // rto retransmits of the send list head, the frame that blocks the stream
    uint32_t counter_rto_chain;   // rto retransmits of a frame queued behind the head
    uint32_t counter_rto_stale;   // rto retransmits of a frame the peer had already reported as received
    uint32_t counter_rto_behind;  // rto retransmits of a frame the peer had not reported yet
    uint32_t counter_cwnd_back;   // cwnd 被 RTO/快速重传打回的次数（乱序误判的直接证据）
    uint32_t counter_cwnd_sfail;  // ... 被"内核没收下这一包"打回的次数（发送缓冲压力）
    uint32_t counter_ssth_floor;  // ssthresh 被算到下限（1）的次数：此后 cwnd < ssthresh 不再成立，只剩 +1/ACK
    uint32_t counter_bm_clip;     // ①-43：本轮 ack 的 24 位位图**盖不全**已收到的帧（>0 才值得动线上格式）
    // "被重传过的帧最后被确认时等了多久"（conf = now - tick_send）的原始分布：rtt_tail 只留限速后的值。
    uint32_t counter_conf_16, counter_conf_32, counter_conf_64, counter_conf_128;
    uint32_t counter_conf_256, counter_conf_512, counter_conf_1024, counter_conf_more;
    uint32_t counter_to_noack;    // 超时时：自首发以来一个 ack 都没到（ack 路径停摆 / ack 丢了）
    uint32_t counter_to_hadack;   // 超时时：收到过 ack，但没覆盖这一帧（洞上方的帧只能等洞补上）
    uint32_t counter_rx_foreign;  // 来源地址不是本连接对端的帧（按地址分发后应当只有"别人的包"）
    uint32_t counter_probe, counter_probe_keep, counter_probe_revert;
    uint32_t counter_probe_mem;   // ①-39：其中由"上限定得比实际需要高"触发的次数 ✓
    uint32_t counter_probe_narrow, counter_giveback;
    uint32_t counter_probe_block; // ①-46：老的比值判据要探测、但"排队不是我们造的/窗口没顶死"把它挡住了 ✓
    uint32_t counter_cap_rise;    // B2：闸门自己升上去的次数 ✓
    uint32_t reserved;            // diag 段到此为止：ffrdp_dump 会把这之前的计数器清零
#endif
    // 本连接被创建的时刻（ms）。用途只有一个：给 dump 的 `total_send/total_recv` 配一个**同源**的分母 ✓
    // （`elapsed` 是上下文级的 ⇒ 重连过、或服务端比这条连接先启动，就会拿 128 s 去除 24 s 的字节 ✗）。
    // 放在 diag 段之后 ⇒ dump 的清零跨度不含它 ✓。
    uint32_t tick_conn_init;
#if FFRDP_PACE_ENABLE
    // C1 令牌桶：单位是 **1/16 帧**（避免"不足 1 帧/周期"被量化成 0 或 1 而失真 ✓）。
    uint32_t pace_tok16;      // 当前令牌（1/16 帧 ✓）
    uint32_t pace_tick;       // 上次补充时刻（ms ✓）
    uint32_t pace_due;        // 有帧在等令牌 ⇒ 下一次该醒的时刻（ms；0 = 没被令牌挡住 ✓）
    uint32_t pace_fps;        // 本圈算出来的整形速率（0 = **不整形**：RTT 太小 / 还没采样 ✓）
#endif
} FFRDPCONN;

// ── 进程级状态 ───────────────────────────────────────────────────────────
// 一个传输层、一条接收线程、一把锁：这里的字段必须与"有几条连接"无关 ✓
typedef struct {
    // ---- 上层边界 ----
    // 协议与应用线程只在这里见面：接收缓冲 + 几个"协议发布给应用看"的镜像量（协议自己的状态仍只被一个执行者碰）。
    FFRDP_LOWLEVEL   *lowlevel;      // 传输层：socket/收包/定时都在它自己的工作线程里
    int               closing;       // 应用说要关：让等待中的应用线程醒来并返回（全部访问都在锁内，不需要原子类型）
    FFRDP_CALLBACK    app_cb;        // 应用回调（v2 起：ACCEPT/CONNECT/DISCONN/ENCRYPT/DECRYPT/RECVDATA）
    void             *app_cbctx;
    int               maxconn;       // 配置：最大连接数（"maxconn:N"，默认 FFRDP_MAXCONN）
    int               handshake_ms;  // 配置：握手超时（"handshake:N"，默认 10000）
    int               idle_ms;       // 配置：空闲判死超时（"idle:N"，默认 5000）
    int               hs_enable;     // 配置：是否走三次握手（"hs:1" 开 / "hs:0" 关，默认 1；hs:0 = 与 v1 线上行为一致）
    int               is_server;     // 角色（v1 的 FLAG_SERVER 搬到进程级：它是"这个进程"的属性，不是连接的 ✗）
    // 槽位复用（见 ffrdp.h 的句柄约定 ✓）：分配时取表里**第一个空槽**，并把该槽的代号 +1 ⇒
    // 句柄 = 代号 * FFRDP_MAXCONN + 槽号 ⇒ 同一个槽被复用后句柄不同 ⇒ **陈旧句柄只会查表失败** ✓。
    uint32_t          gen[FFRDP_MAXCONN];   // 每个槽已分配过几次（0 = 从未用过 ✓；首次分配的句柄 = 槽号 ✓）
    int               live_conn;     // 当前活着的连接数（含内嵌主连接 ✓；dump / 上限判断用 ✓）
    pthread_mutex_t   lock;          // 保护连接状态（recv_buff/游标、send_list、cur_new_*、peer_addr、flags 等）
    pthread_cond_t    cond;          // 协议侧腾出空间或送来数据时叫醒应用侧

    uint32_t   tick_ffrdp_dump;
    // 回调入口分档：按类型 + 入口时的 nconn（用来分辨"多出的回调没进按连接遍历那一段"）。
    uint32_t   cb_timer_calls;    // 入口 type==TIMER ✓
    uint32_t   cb_data_calls;     // 入口 type==RECV ✓
    uint32_t   cb_other_calls;    // 其他类型 ✓
    uint32_t   cb_nconn_zero;     // 入口时 nconn==0 ✗（那一段循环体一次都不跑 ✓）
    uint32_t   cb_nconn_sum;      // 入口时 nconn 之和 ✓（配 timer_calls 看平均几条 ✓）
    // arm_timer 取 min 之后的值与赢家：补上"每连接 due"与"低层收到的 arm 值"之间那一跳。
    uint32_t   arm_calls;         // arm_timer 被调用的次数（应 ≈ TIMER 回调数 ✓）
    uint32_t   arm_d0;            // 算出的 due == 0（没有期限 ✓）
    uint32_t   arm_dnow;          // due <= now（真的要求"现在" ✗）
    uint32_t   arm_dnear;         // now+1..4
    uint32_t   arm_dfar;          // > now+4
    uint32_t   arm_win[FFRDP_MAXCONN];   // 是哪条连接的 x 赢了（赢家就是"要现在"的那条 ✗）
    uint32_t   arm_ack_win;       // ACK_DUE 覆盖赢的次数 ✓
    uint32_t   tick_ffrdp_init;   // tick of ffrdp_init, for the elapsed time in the dump
    // ①-24：在"查表"之前就被拒的两条（还没有 c ⇒ 只能记在上下文上 ✓）
    uint32_t   stat_ref_noarg;    // !ffrdp / !buf / len<=0 ⇒ EINVAL
    uint32_t   stat_ref_noconn;   // conn 越界 / 表槽空 ⇒ CONN_INVALID
    FFRDPCONN  conn;   // 内嵌主连接（表 0 号指向它；服务端这条从不用于数据 ⇒ 两块大缓冲按需分配）

    // ── 连接表 ───────────────────────────────────────────────────────────────
    // 句柄 = 下标；lowlevel 收到帧后按对端地址查这张表（v2 的分发点）。
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

// 返回值：0 = 已入队 ✓；1 = 同序号的帧已在队里，这一份被丢掉（重复帧 ✓）。
// 调用方**必须**区分这两种结果：接收侧要拿"1"回报对端（dup_seen），否则对端的重传统计读到 0，
// 它会以为自己的重传全都到达得是时候，从而一直用同一套激进参数（见 §26.104）✗。
static int list_enqueue(FFRDP_FRAME_NODE **head, FFRDP_FRAME_NODE **tail, FFRDP_FRAME_NODE *node)
{
    FFRDP_FRAME_NODE *p;
    uint32_t seqnew, seqcur;
    int      dist;
    if (*head == NULL) {
        *head = *tail = node;
    } else {
        seqnew = GET_FRAME_SEQ(node);
        for (p = *tail; p; p = p->prev) {
            seqcur = GET_FRAME_SEQ(p);
            dist   = seq_distance(seqnew, seqcur);
            if (dist == 0) { free(node); return 1; } // duplicated frame, drop it and free memory
            if (dist >  0) {
                if (p->next) p->next->prev = node;
                else *tail = node;
                node->next = p->next;
                node->prev = p;
                p->next    = node;
                return 0;
            }
        }
        node->next = *head;
        node->next->prev = node;
        *head = node;
    }
    return 0;
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
static int ffrdp_send_data_frame(FFRDPCONTEXT *ffrdp, FFRDPCONN *c, FFRDP_FRAME_NODE *frame)
{
    switch (frame->size - c->smss) {
#if FFRDP_FEC_ENABLE
    case 6 : FFRDP_CNT(c, counter_fec_tx); *(uint16_t*)(frame->data + 4 + c->smss) = c->fec_txseq++; break; // tx fec frame
#endif
    case 4 : c->counter_txfull ++; break; // tx full frame
    default: FFRDP_CNT(c, counter_txshort); break; // tx short frame
    }
    // 0 = 传输层收下了这一帧；-1 = 它没收（非阻塞发送缓冲满、或链路报错）。协议不区分这两者：帧已经
    // 不在它手里了，丢没丢由上面的重传负责 —— 与旧版 sendto 失败的语义一致。
    if (ffrdp->lowlevel->send(ffrdp->lowlevel, c->peer_addr, (uint8_t*)frame->data, (int)frame->size) != 0) { FFRDP_CNT(c, counter_send_err); return -1; }
#if FFRDP_FEC_ENABLE
    if (frame->size == 4 + c->smss + 2) { // fec frame
        uint32_t *psrc = (uint32_t*)frame->data, *pdst = (uint32_t*)c->fec_txbuf, i;
        for (i = 0; i < (4 + c->smss) / sizeof(uint32_t); i++) *pdst++ ^= *psrc++; // make xor fec frame
        if (c->fec_txseq % c->fec_txredundancy == c->fec_txredundancy - 1) {
            *(uint16_t*)(c->fec_txbuf + 4 + c->smss) = c->fec_txseq++; c->fec_txbuf[0] = c->fec_txredundancy;
            // 一次"某某被 socket 拒了"的记账：冗余帧没发出去 = 这次修复不会发生 ✓（原来不留任何痕迹 ✗）。
            if (ffrdp->lowlevel->send(ffrdp->lowlevel, c->peer_addr, (uint8_t*)c->fec_txbuf, (int)frame->size) != 0) FFRDP_CNT(c, counter_send_err); // send fec frame
            memset(c->fec_txbuf, 0, sizeof(c->fec_txbuf)); // clear tx_fecbuf
            FFRDP_CNT(c, counter_fec_tx);
        }
    }
#endif
    return 0;
}

static int ffrdp_recv_data_frame(FFRDPCONN *c, FFRDP_FRAME_NODE *frame)
{
#if FFRDP_FEC_ENABLE
    uint32_t fecseq, fecrdc, *psrc, *pdst, type, i;
#endif
    // rmss 是学来的，且被当除数与拷贝长度用 ⇒ 比自身开销还短的碎片会让它变 0 或下溢（SIGFPE / SIGSEGV）；t_fuzz 守着这条。
    switch (frame->data[0]) {
    case FFRDP_FRAME_TYPE_SHORT: FFRDP_CNT(c, counter_rxshort); return 0; // short frame
    case FFRDP_FRAME_TYPE_FULL :
        if (frame->size <= 4) { FFRDP_CNT(c, counter_rx_bad); return -1; }  // header only: no payload at all
        c->counter_rxfull ++; c->rmss = frame->size - 4; return 0; // full frame
#if FFRDP_FEC_ENABLE
    default:
        if (frame->size <= 6) { FFRDP_CNT(c, counter_rx_bad); return -1; }  // header + fec seq, no payload
        FFRDP_CNT(c, counter_fec_rx); c->rmss = frame->size - 6; break;// fec  frame
#else
    // 本构建没有 FEC ⇒ 对端发来的冗余帧直接丢 ✓（**数据帧照常收** ✓；它本来也只是"能修一帧"的机会）。
    default: return -1;
#endif
    }
#if FFRDP_FEC_ENABLE
    fecseq = *(uint16_t*)(frame->data + frame->size - 2);
    fecrdc = frame->data[0];
    if (fecseq / fecrdc != c->fec_rxseq / fecrdc || c->fec_rxredundancy != fecrdc) { // group changed or fec_rxredundancy changed
        memcpy(c->fec_rxbuf, frame->data, frame->size);
        c->fec_rxseq = fecseq; c->fec_rxredundancy = fecrdc;
        if (fecseq % fecrdc == fecrdc - 1) {
            // ⚠️ 这一帧**就是冗余帧** ⇒ 它不能算成"收到一个数据帧" ✗：此刻组里收到的数据是 0 个 ✓。
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
        if (c->fec_rxcnt != fecrdc - 2) { FFRDP_CNT(c, counter_fec_failed); return -1; }
        type = frame->data[0];
        // rxcnt == 0（只有 rdc == 2 到得了这儿 ✓）：组里一个数据帧都没收到 ⇒ 冗余帧**本身就是**
        // 那一帧 ✓（异或空集 = 原样 ✓）⇒ 不能再拿它跟自己异或一遍（那是全 0 ✗）。
        if (c->fec_rxcnt > 0) {
            psrc = (uint32_t*)c->fec_rxbuf; pdst = (uint32_t*)frame->data;
            for (i = 0; i < (frame->size - 2) / sizeof(uint32_t); i++) *pdst++ ^= *psrc++;
        }
        frame->data[0] = type;
        FFRDP_CNT(c, counter_fec_ok);
    } else if (!(c->fec_rxmask & (1 << (fecseq % fecrdc)))) { // update fec_rxbuf
        psrc = (uint32_t*)frame->data; pdst = (uint32_t*)c->fec_rxbuf;
        for (i = 0; i < (frame->size - 2) / sizeof(uint32_t); i++) *pdst++ ^= *psrc++;
        c->fec_rxmask |= 1 << (fecseq % fecrdc); c->fec_rxcnt++;
    }
#endif
    return 0;
}

static long ffrdp_lowlevel_cb(void *cbctx, int type, void *buf, int len);  // lowlevel 的工作线程从这里回到协议
static int  ffrdp_hs_send(FFRDPCONTEXT *ffrdp, FFRDPCONN *c, uint8_t sub); // 握手：定义在 lowlevel 回调之前
static void ffrdp_arm_timer(FFRDPCONTEXT *ffrdp);                          // 定义在协议后段（init 里要用 ✓）
static int  ffrdp_conn_handle(FFRDPCONTEXT *ffrdp, FFRDPCONN *c);
static long ffrdp_emit(FFRDPCONTEXT *ffrdp, int type, int conn, uint64_t addr, void *buf, int len); // 事件出口
static long ffrdp_emit_ex(FFRDPCONTEXT *ffrdp, int type, int conn, uint64_t addr, void *buf, int len, int code); // 带原因位（DISCONN 用）

// 一条连接的默认值：主连接与"按需分配的新连接"共用同一套（方案 A：每连接一个 calloc ✓，
// 每连接仍用固定大小缓冲 ✓ —— 与 ffrdp.h 的设计说明 的"第一阶段不优化"一致 ✓）。
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
    if (c->recv_buff) { free(c->recv_buff); c->recv_buff = NULL; c->recv_cap = 0; }
}

static void ffrdp_conn_init(FFRDPCONN *c, int smss, int sfec)
{
    c->swnd     = FFRDP_DEF_CWND_SIZE;
    c->cwnd     = FFRDP_DEF_CWND_SIZE;
    c->ssthresh = FFRDP_DEF_CWND_SIZE;
    c->rtts     = (uint32_t) -1;
    c->rto      = FFRDP_MIN_RTO;
    c->rmss     = FFRDP_MAX_RX_MSS;   // rmss 是"对端能发多大"（收到的帧是多大）⇒ 用收 MSS ✓，不是我们发多大
    c->smss     = MAX(1, MIN(smss, FFRDP_MAX_MSS));
    c->fec_txredundancy = FFRDP_FEC_ENABLE ? MAX(0, MIN(sfec, FFRDP_FRAME_TYPE_FEC32)) : 0;  // 关掉 ⇒ 帧不加冗余尾 ✓
    // ①-38：**这里不分配大缓冲** ✗ —— 分配交给真正的使用方（下面的 ffrdp_conn_bufs ✓）。
    c->tick_conn_init = NOW_MS();   // 本连接的年龄起点 ⇒ dump 的 `conn age`（与 total_send/total_recv 同源 ✓）
#if FFRDP_PACE_ENABLE
    c->pace_tick  = c->tick_conn_init;
    c->pace_tok16 = 16u * (uint32_t)FFRDP_PACE_BURST_MS;   // 起步先给一桶 ⇒ 不额外拖慢启动 ✓
    c->pace_due   = 0;
    c->pace_fps   = 0;                                     // 还没采样到 RTT ⇒ 先不整形 ✓
#endif
}

// 按需分配一条新连接（方案 A）。**槽位回收 ✓**：取表里第一个空槽（0 号是内嵌主连接，不参与 ✓），
// 并把该槽的代号 +1 ⇒ 句柄 = 代号 * FFRDP_MAXCONN + 槽号。
// 为什么这样还能保住"陈旧句柄安全"：同一个槽第二次复用时代号变了 ⇒ 旧句柄算出来的 table 槽虽然相同，
// 但 `c->handle != handle` ⇒ 查表直接失败（见 ffrdp_conn_lookup ✓），不会打到新连接上 ✓。
static FFRDPCONN *ffrdp_conn_new(FFRDPCONTEXT *ffrdp, uint64_t addr, int *handle)
{
    FFRDPCONN *c;
    int        slot = -1, i;
    for (i = 1; i < ffrdp->maxconn && i < FFRDP_MAXCONN; i++) {
        if (!ffrdp->table[i]) { slot = i; break; }
    }
    if (slot < 0) return NULL;                                        // 表全满 ⇒ 拒（调用方计数 ✓，不静默 ✓）
    // 代号上限：句柄要装进 int ⇒ 代号 < 2^27（槽 16 个）；到顶就拒绝分配（每槽 1.3 亿次复用，够用了 ✓）。
    if (ffrdp->gen[slot] >= 0x08000000u) return NULL;
    if (!(c = calloc(1, sizeof(FFRDPCONN)))) return NULL;
    ffrdp_conn_init(c, ffrdp->conn.smss, ffrdp->conn.fec_txredundancy);
    if (ffrdp_conn_bufs(c) != 0) { ffrdp_conn_bufs_free(c); free(c); return NULL; }   // ①-38：受理的新连接必须有缓冲
    c->peer_addr = addr;
    c->flags    |= FLAG_CONNECTED;  // 服务端：对端就是它（学到即连上 ✓）
    c->handle    = ffrdp->gen[slot] * (uint32_t)FFRDP_MAXCONN + (uint32_t)slot;
    ffrdp->gen[slot]++;
    ffrdp->table[slot]  = c;
    if (slot + 1 > ffrdp->nconn) ffrdp->nconn = slot + 1;   // nconn = **用过的最大槽 + 1**（水位，只用于遍历 ✓）
    ffrdp->live_conn++;
    if (handle) *handle = (int)c->handle;
    return c;
}

// 释放一条连接（主连接是内嵌的 ⇒ 只清空它的链表，不 free 结构体本身 ✓）。
// **槽位回收 ✓**：表项清成 NULL，下一次分配就能再用（代号在分配时 +1 ✓，不在释放时 ✓）。
static void ffrdp_conn_free(FFRDPCONTEXT *ffrdp, FFRDPCONN *c)
{
    int i, found = 0;
    if (!c) return;
    list_free(&c->send_list_head, &c->send_list_tail);
    list_free(&c->recv_list_head, &c->recv_list_tail);
    if (c->cur_new_node) { free(c->cur_new_node); c->cur_new_node = NULL; }
    ffrdp_conn_bufs_free(c);   // ①-38：大缓冲按需分配 ⇒ 这里归还 ✓（内嵌主连接也一样安全 ✓）
    for (i = 0; i < ffrdp->nconn && i < FFRDP_MAXCONN; i++) {
        if (ffrdp->table[i] == c) { ffrdp->table[i] = NULL; found = 1; }
    }
    if (found && ffrdp->live_conn > 0) ffrdp->live_conn--;
    if (c != &ffrdp->conn) free(c);
}

// 句柄 ⇒ 连接。**唯一**的查表入口 ⇒ 陈旧句柄（槽已复用、代号对不上）一律当"无效句柄" ✓。
static FFRDPCONN *ffrdp_conn_lookup(FFRDPCONTEXT *ffrdp, int handle)
{
    int        slot;
    FFRDPCONN *c;
    if (!ffrdp || handle < 0) return NULL;
    slot = handle % FFRDP_MAXCONN;          // 句柄 = 代号 * FFRDP_MAXCONN + 槽号 ✓
    if (slot >= ffrdp->maxconn || slot >= FFRDP_MAXCONN) return NULL;
    c = ffrdp->table[slot];
    if (!c || (int)c->handle != handle) return NULL;   // 代号对不上 ⇒ 这条是"上一个用这个槽的连接" ✗
    return c;
}

// 配置串解析（v2）："ffrdps://ip[:port][,key:val]..." / "ffrdpc://ip[:port][,key:val]..."。
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
#if !FFRDP_FEC_ENABLE
    // FEC 编译掉了 ⇒ 配了也没用：明确说一句、并钉回 0 ✓（否则帧会带着冗余尾发出去、却没人能修 ✗）。
    if (sfec > 0) { printf("<3> ffrdp_init: FEC 已编译掉（FFRDP_FEC_ENABLE=0）⇒ fec:%d 被忽略 ✓\n", sfec); sfec = 0; }
#endif
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
    // 连接表：0 号固定给内嵌主连接（句柄 0 = 代号 0 * 16 + 槽 0 ✓）⇒ 服务端可用槽是 1..maxconn-1 ✓。
    ffrdp->table[0]     = &ffrdp->conn;
    ffrdp->nconn        = 1;       // 水位
    ffrdp->live_conn    = 1;       // 0 号也是活的（客户端的收发全靠它 ✓）

    // 主连接（P4）：单连接阶段协议只跑在它上面；多连接按地址分发是下一步。
    FFRDPCONN *c = &ffrdp->conn;
    ffrdp_conn_init(c, smss, sfec);
    // ①-38：**只有客户端**给内嵌主连接分配那两块大缓冲 ✓；
    // 服务端在握手模式下不使用它（受理进来的连接自己分配 ✓）⇒ 服务端**根本不分配** ⇒ 省 ~152 KB ✓。
    if (!server && ffrdp_conn_bufs(c) != 0) { free(ffrdp); return NULL; }
    ffrdp->tick_ffrdp_dump = NOW_MS();
    ffrdp->tick_ffrdp_init = ffrdp->tick_ffrdp_dump;
    c->tick_rto_backoff    = ffrdp->tick_ffrdp_dump - c->rto; // allow the first timeout to backoff
    // ①-41：应用队列已删除 ⇒ 应用线程在 ffrdp_send 里直接成帧，工作线程只把成好的帧发出去 ✓。

    // 传输层：socket、bind、非阻塞、缓冲区大小、收包线程、定时，全在 lowlevel 里。协议只认
    // send / timer / 回调。服务端把 addr 交给它（bind:<addr>）；客户端交 "none"（不绑定，由内核在
    // 第一次发包时择源端口，与旧版一致），再按同一套约定解析对端。
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
        c->hs_start = NOW_MS();
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

// 退出时的"礼貌关闭"：对已 UP 的连接发 FIN，并等对端的 FIN（**有硬上限** ✓）。
// 为什么值得做：不发 FIN 的话，对端只能靠 5s 看门狗（idle_ms）发现我们走了 —— 那 5 秒里
// 连接槽位和整块接收缓冲都还占着（服务端只有 maxconn-1 个槽 ⇒ 客户端反复进出会撞上限），
// 日志里还会多出 N 条 "keepalive dead"（1018-191039：14 条）。
// 上限是硬性的：本进程正在退出，不能为了礼貌无限等 ⇒ 最多 FFRDP_EXIT_FIN_WAIT_MS；
// 收不到就按老路径本地拆除（对端仍由看门狗兜底 ✓ ⇒ 行为只会比以前好，不会更差 ✓）。
#define FFRDP_EXIT_FIN_WAIT_MS   200   // 最多等这么久（RTT 1~10ms 时通常一个来回就回 ✓）
#define FFRDP_EXIT_FIN_RETRY_MS   50   // FIN 丢了就重发（窗口内最多几次 ✓）
// 收尾崩溃定位：1 = 在 ffrdp_exit 的每个阶段往 stderr 打一行（只在排障构建里开，默认 0 不留噪音 ✓）
#ifndef FFRDP_EXIT_TRACE
#define FFRDP_EXIT_TRACE          0
#endif
#define FFEXIT_STAGE(n) do { if (FFRDP_EXIT_TRACE) { fprintf(stderr, "[exit] stage %d\n", (n)); fflush(stderr); } } while (0)

void ffrdp_exit(void *ctxt)
{
    FFRDPCONTEXT   *ffrdp = (FFRDPCONTEXT*)ctxt;
    FFRDPCONN      *c;
    FFRDP_LOWLEVEL *ll;
    int             i;
    if (!ctxt) return;  // 判空**必须**在最前：下面每句都要用 ctxt ✓
    c = &ffrdp->conn;   // 主连接（内嵌 ✓，由末尾那几行负责 ✓）
    // ── ① 礼貌关闭：先走 FIN，再拆（见上面两个宏的说明）────────────────────────
    // 必须在 closing=1 之前：closing 一置位，收包回调就直接返回了 ⇒ 对端的 FIN 收不到 ✗。
    if (ffrdp->hs_enable && ffrdp->lowlevel) {
        struct timespec t_end;
        int      pending = 0;
        pthread_mutex_lock(&ffrdp->lock);
        // 1) 还 UP 的 ⇒ 发 FIN、进 FIN_WAIT（与 ffrdp_close 同一套状态机 ✓）
        for (i = 0; i < ffrdp->nconn; i++) {
            FFRDPCONN *x = ffrdp->table[i];
            if (!x || x->hs_state != HS_UP) continue;
            x->hs_state = HS_FIN_WAIT;
            x->hs_start = NOW_MS();
            ffrdp_hs_send(ffrdp, x, FFRDP_HS_FIN);
        }
        // 2) 还欠对端一个 FIN 回音的（含应用刚刚 ffrdp_close 过的连接 ✓）⇒ 一起等
        for (i = 0; i < ffrdp->nconn; i++)
            if (ffrdp->table[i] && ffrdp->table[i]->hs_state == HS_FIN_WAIT) pending++;
        if (pending) {
            // ⚠️ 这个上限必须用**真实时间**量 ✗✓：`NOW_MS()` 在仿真里是虚拟时钟（sim_tick），
            // 而等待期间没人推进它 ⇒ `NOW_MS() - t0 < 200` 恒真 ⇒ 退出永不返回（t_recvdata / t_handshake
            // 实测挂死，且与本次改动无关：关掉新旋钮同样挂）。这里改成 CLOCK_REALTIME 的绝对期限 ✓。
            ffrdp_arm_timer(ffrdp);      // 叫醒 lowlevel（收 FIN 的正是它那条线程 ✓）
            clock_gettime(CLOCK_REALTIME, &t_end);
            t_end.tv_sec  += FFRDP_EXIT_FIN_WAIT_MS / 1000;
            t_end.tv_nsec += (long)(FFRDP_EXIT_FIN_WAIT_MS % 1000) * 1000000L;
            if (t_end.tv_nsec >= 1000000000) { t_end.tv_sec++; t_end.tv_nsec -= 1000000000; }
            for (;;) {
                struct timespec ts;
                pending = 0;
                for (i = 0; i < ffrdp->nconn; i++) {
                    FFRDPCONN *x = ffrdp->table[i];
                    if (!x || x->hs_state != HS_FIN_WAIT) continue;   // 收到 FIN ⇒ FAILED/已释放 ✓
                    pending++;
                    if ((int32_t)(NOW_MS() - x->hs_tick) >= FFRDP_EXIT_FIN_RETRY_MS)
                        ffrdp_hs_send(ffrdp, x, FFRDP_HS_FIN);
                }
                if (!pending) break;
                clock_gettime(CLOCK_REALTIME, &ts);
                if (ts.tv_sec > t_end.tv_sec ||
                    (ts.tv_sec == t_end.tv_sec && ts.tv_nsec >= t_end.tv_nsec)) break;   // 到点 ⇒ 按老路径本地拆除 ✓
                ts.tv_nsec += 10 * 1000 * 1000;                  // 10ms 一醒（回调每圈也会 broadcast ✓）
                if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
                pthread_cond_timedwait(&ffrdp->cond, &ffrdp->lock, &ts);
            }
        }
        pthread_mutex_unlock(&ffrdp->lock);
    }
    FFEXIT_STAGE(1);   // 礼貌 FIN 阶段结束
    // 顺序是有讲究的：先让等待中的应用线程醒过来（它们看到 closing 就返回），再停工作线程。反过来的话，
    // 一个卡在 cond 上的应用线程会等一个再也不会有人推进的队列，而 exit 又 join 工作线程 —— 两边互等。
    pthread_mutex_lock(&ffrdp->lock);
    ffrdp->closing = 1;
    pthread_cond_broadcast(&ffrdp->cond);
    ll = ffrdp->lowlevel;
    pthread_mutex_unlock(&ffrdp->lock);
    FFEXIT_STAGE(2);   // lowlevel 句柄已取到
    // 先停线程 + join（socket 是它自己关的，见 lowlevel.h）。**必须在摘掉把手之前**：工作线程此刻可能正在
    // 回调里跑一整段（ffrdp_tick_conn 会经 ffrdp_send_data_frame 用 ffrdp->lowlevel）—— 先前这里先把 lowlevel
    // 置 NULL 再 exit，那把正在跑的 tick 就会在 ffrdp_send_data_frame 里对 NULL 取成员（ffrdp->lowlevel->send ✓），实测 0.17% 的收尾段错误就是这么来的。
    if (ll) ll->exit(ll);
    FFEXIT_STAGE(3);   // lowlevel 线程已停 + join 完
    pthread_mutex_lock(&ffrdp->lock);
    ffrdp->lowlevel = NULL; // 线程已停、handle 也已被它自己释放 ⇒ 现在摘掉才安全
    // ⚠️ 按需分配的连接**必须等在这里**再放 ✗✓：工作线程刚刚可能正跑在
    //    `ffrdp_tick_conn → ffrdp_send_data_frame` 里（手里攥着某条连接 ✓）。
    for (int i = 0; i < ffrdp->nconn; i++) {
        if (ffrdp->table[i] && ffrdp->table[i] != &ffrdp->conn) {
            ffrdp_conn_free(ffrdp, ffrdp->table[i]);
            ffrdp->table[i] = NULL;          // conn_free 已经清过；这里只是让循环的语义显式 ✓
        }
    }
    pthread_mutex_unlock(&ffrdp->lock);
    FFEXIT_STAGE(4);   // 各连接已 free、表已清空
    if (c->cur_new_node) free(c->cur_new_node);
    list_free(&c->send_list_head, &c->send_list_tail);
    list_free(&c->recv_list_head, &c->recv_list_tail);
    ffrdp_conn_bufs_free(c);   // ①-38：内嵌主连接不经 conn_free ⇒ 这里归还它那两块大缓冲 ✓
    FFEXIT_STAGE(5);   // 内嵌主连接的链表/缓冲已归还
    pthread_cond_destroy (&ffrdp->cond);
    pthread_mutex_destroy(&ffrdp->lock);
    FFEXIT_STAGE(6);   // cond/mutex 已销毁
    free(ffrdp);
}

// ①-41：ffrdp_send 现在直接成帧 ⇒ 下面这个成帧器需要前向声明 ✓。
static int ffrdp_frame_message(FFRDPCONTEXT *ffrdp, FFRDPCONN *c, const uint8_t *buf, int len);

// 发送对外只有一个入口，语义没有变：**要么整条接受、要么一个字节都不接受**。
int ffrdp_send(void *ctxt, int conn, char *buf, int len, int timeout_ms)
{
    FFRDPCONTEXT *ffrdp = (FFRDPCONTEXT*)ctxt;
    uint32_t      t0;
    int           rc = FFRDP_EAGAIN;   // 默认：暂时收不下（队列满/在途满）⇒ 稍后重试
    if (!ffrdp || !buf || len <= 0) { if (ffrdp) ffrdp->stat_ref_noarg++; return FFRDP_EINVAL; }          // ①-24（只读 ✓）
    if (!ffrdp_conn_lookup(ffrdp, conn)) { ffrdp->stat_ref_noconn++; return FFRDP_CONN_INVALID; }  // ①-24（只读 ✓）
    pthread_mutex_lock(&ffrdp->lock);
    FFRDPCONN *c = ffrdp_conn_lookup(ffrdp, conn);   // 句柄 ⇒ 连接（唯一入口：代号对不上就算无效 ✓）
    // ⚠️ 锁外那次判空**不够** ✗：上面到这里的几行之间没拿锁 ✓，协议线程可能正好在收 FIN /
    //    keepalive 判死时把这条连接收掉（表槽清空 + free ✓）⇒ 这里再查就是 NULL ✓。
    // ⚠️⚠️ 这里**不能** `goto out` ✗✗：`out:` 里第一句就是 `FFRDP_CNT(c, counter_send_calls)`，
    // 而走到这里时 `c` 恰恰是 NULL ⇒ 空指针解引用 ⇒ SIGSEGV（就是那个"偶发收尾段错误"：
    // lo 上 CPU 高压时约 1/18 命中，git HEAD 同样复现；gdb 下时序一变就不出现 ✗）。
    // 触发条件很日常：上面的锁外预查（①-24）通过之后、这行拿到锁之前，协议线程把这条连接收掉了
    // （对端 FIN / keepalive 判死 / 连接风暴里 HS_FAILED）⇒ 查表返回 NULL。服务端刚 accept 就被
    // 对端关掉时最容易踩到（日志停在 `accept ok: handle 1` 就是这个 ✓）。
    if (!c) { ffrdp->stat_ref_noconn++; pthread_mutex_unlock(&ffrdp->lock); return FFRDP_CONN_INVALID; }   // ①-24（只读 ✓）
    // 受理判据：帧数上限（在途 + 这一条要占的帧）算在这里；wait_snd 在同一把锁下直接读 ✓。
    if (ffrdp->closing) { FFRDP_CNT(c, counter_ref_closing); rc = FFRDP_CLOSED; goto out; }    // 正在关：当作已断（①-24 ✓）
    if (ffrdp->is_server && (c->flags & FLAG_CONNECTED) == 0) { FFRDP_CNT(c, counter_ref_notconn); rc = FFRDP_CLOSED; goto out; } // 对端还没连上（①-24 ✓）
    if (ffrdp->hs_enable) {
        if (c->hs_state == HS_FAILED) { FFRDP_CNT(c, counter_ref_hsfail); rc = FFRDP_CLOSED; goto out; }   // 握手超时 ⇒ 当成已断（①-24 ✓）
        if (!ffrdp->is_server && c->hs_state != HS_UP) { FFRDP_CNT(c, counter_ref_hsdown); rc = FFRDP_EAGAIN; goto out; } // 还在握手 ⇒ 先别发（①-24 ✓）
    }
    // ①-41 受理判据（队列删掉后）：① 未发出帧预算 FFRDP_UNSENT_MAX ② wait_snd < FFRDP_MAX_CWND_SIZE ③ 帧数闸。
    t0 = NOW_MS();
    {   int counted = 0;
        for (;;) {
            uint32_t need = (uint32_t)((len + c->smss - 1) / c->smss);
            // ⚠️ ①-41d：`tx_unsent == 0` 时**总放一条** ✗✓ —— 否则预算比一条消息的帧数还小
            //    （比如 UNSENT=8 而一条 16KB 消息要 13 帧 ✓）时 `tx_unsent + need > FFRDP_UNSENT_MAX` 永远成立
            //    ⇒ 这条消息永远进不来 ✗✗（lo 实测 UNSENT=8 ⇒ 吞吐掉到 1.5KB/s ✗，就是这个 ✓）。
            if ((!FFRDP_UNSENT_MAX || c->tx_unsent == 0 || c->tx_unsent + need <= FFRDP_UNSENT_MAX) &&
                c->wait_snd < (uint32_t)FFRDP_MAX_CWND_SIZE &&
                need + c->wait_snd <= FFRDP_MAX_WAITSND) {
                rc = ffrdp_frame_message(ffrdp, c, (const uint8_t*)buf, len);   // 原子 ✓：整条或一个字节都不收
                if (rc > 0) break;                              // 整条被接受 ✓
                rc = FFRDP_EAGAIN;                              // 节点分配失败 ⇒ 稍后重试（计数由 out: 统一记 ✓）
            } else if (!counted) { FFRDP_CNT(c, counter_refuse_wnd); counted = 1; }
            if (timeout_ms == 0 || ffrdp->closing) break;
            if (timeout_ms > 0 && (int32_t)(NOW_MS() - t0) >= timeout_ms) break;
            // 放不下而调用方愿意等：睡在 cond 上，让工作线程去成帧、去收 ack 腾空间。旧版是在这里自己跑
            // 一轮协议 —— 那正是"应用被迫驱动协议"的最后一个入口，现在协议由它自己的线程驱动。
            {   struct timespec ts;
                int wait_ms = timeout_ms > 0 ? timeout_ms - (int32_t)(NOW_MS() - t0) : FFRDP_COND_WAIT_MS;
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
            c = ffrdp_conn_lookup(ffrdp, conn);
            // 连接没了 ⇒ 直接解锁返回（不能走 out: —— 那里要用 c 记计数 ✓）
            if (!c) { pthread_mutex_unlock(&ffrdp->lock); return FFRDP_CLOSED; }
        }   // ①-41：关闭上面新建的 for (;;)
    }       // ①-41：关闭 { int counted = 0; ... }
out:
    // 兜底：`out:` 下面每一句都要解引用 c。走到这里 c 一定是非 NULL（唯一会变 NULL 的地方已改成
    // 直接 return），但这条防线只要一行 —— 免得以后再加 `goto out` 时又踩同一个坑 ✗。
    if (!c) { pthread_mutex_unlock(&ffrdp->lock); return FFRDP_CONN_INVALID; }
    FFRDP_CNT(c, counter_send_calls);   // ①-35b（只读 ✓）
    // 计数口径与旧版一致：只要这条消息没被接受就记一次（现在 rc 有 0 / -1 / -2 三种，
    // 旧版只有 -1 ⇒ 条件从 rc < 0 改成 rc <= 0 才能保持 fail 这个数不变 ✓）。
    if (rc <= 0) FFRDP_CNT(c, counter_send_failed);
    // 刚排进去的东西不该等下一个期限：叫醒工作线程立刻跑一轮（这是 timer 的第二种用法）。
    if (rc > 0 && ffrdp->lowlevel) {   // ①-35c：叫醒（可合并 ✓）
        int wake = 1;
        if (FFRDP_SENDWAKE_COALESCE && c->tx_woken) { wake = 0; FFRDP_CNT(c, counter_send_wake_skip); }
        if (wake) { FFRDP_CNT(c, counter_send_wakes); c->tx_woken = 1; ffrdp->lowlevel->timer(ffrdp->lowlevel, get_timestamp32_ms()); }
    }
    pthread_mutex_unlock(&ffrdp->lock);
    return rc;
}

// 把一条消息成帧：要么整条被接受，要么一个字节都不接受 ✓。
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
    // 先把这条消息要用的**所有**帧节点分配好再拷字节：任何一次失败就整条放弃 ⇒ 调用方只会看到 len 或 -1。
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
        } else c->cur_new_tick = NOW_MS();
    }
    while (spare) { node = spare; spare = spare->next; free(node); } // cannot happen, but never leak
    return len - n; // with the pre-allocation above this is always len: the call is atomic
}

// 判死用的"静默上限"：配置的 idle_ms，但**不小于 4 个 RTT** ✓。
// 为什么必须跟着 RTT 走 ✗→✓：RTT 被队列 / CPU 拉到秒级时（6 客户端那轮实测 `rtts 2298 ms`），固定 5 s
// 只够一个来回 ⇒ 偶发一次 ACK 丢失就把整条连接判死 ⇒ 反复重连（"连接保持"输给了"数据收发" ✗）。
// 上限 60 s：别让病态 RTT 把连接拖成永远一瘸一拐的僵尸 ✓。
static uint32_t ffrdp_idle_deadline(FFRDPCONTEXT *ffrdp, FFRDPCONN *c)
{
    uint32_t idle = (ffrdp->idle_ms > 0) ? (uint32_t)ffrdp->idle_ms : 5000u;
    if (c->rtts != (uint32_t)-1) {
        uint64_t m = 4ull * (uint64_t)c->rtts;
        if (m > 60000ull) m = 60000ull;
        if (m > (uint64_t)idle) idle = (uint32_t)m;
    }
    return idle;
}

static int ffrdp_isdead_locked(FFRDPCONTEXT *ffrdp, FFRDPCONN *c)
{
    if (c->hs_state == HS_FAILED) return 1;   // 握手超时：对端从未连上 ⇒ 就是"已断"
    // keepalive：UP 且整整一个"静默上限"没听到对端 ⇒ 判死（这是 v2 才有的"空闲也能判死" ✓）。
    if (ffrdp->hs_enable && c->hs_state == HS_UP &&
        (int32_t)(NOW_MS() - c->tick_last_recv) > (int32_t)ffrdp_idle_deadline(ffrdp, c)) return 1;
    if (!c->send_list_head) return 0;
    if (c->send_list_head->flags & FLAG_FIRST_SEND) {
        // 首帧 5 s 没被确认 ⇒ **只有"同时也一直没听到对端"才判死** ✓。
        // 对端还在发帧就说明链路是通的，那个"洞"是重传 / 接收窗口的事，不该拿整条连接去赌 ✗
        // （c4 那轮：`rtts 2298 ms`、`rto 2000` ⇒ 5 s 只够两次 RTO，按老口径就先把它判死了 ✓）。
        uint32_t now = NOW_MS();
        return (int32_t)(now - c->send_list_head->tick_1sts) > FFRDP_DEAD_TIMEOUT &&
               (int32_t)(now - c->tick_last_recv)        > FFRDP_DEAD_TIMEOUT;
    } else {
        return (int32_t)c->tick_send_query - (int32_t)c->tick_recv_ack > FFRDP_DEAD_TIMEOUT;
    }
}

// 接收：有数据就立刻返回（不会为了凑满 len 而等），没数据时按 timeout_ms 等。
int ffrdp_recv(void *ctxt, int conn, char *buf, int len, int timeout_ms)
{
    FFRDPCONTEXT *ffrdp = (FFRDPCONTEXT*)ctxt;
    uint32_t      t0;
    int           ret = FFRDP_EAGAIN;
    if (!ffrdp || !buf || len <= 0) return FFRDP_EINVAL;
    if (!ffrdp_conn_lookup(ffrdp, conn)) return FFRDP_CONN_INVALID;
    pthread_mutex_lock(&ffrdp->lock);
    FFRDPCONN *c = ffrdp_conn_lookup(ffrdp, conn);   // 句柄 ⇒ 连接
    // ⚠️ 与 ffrdp_send 同一个坑 ✗：锁外判过空，锁内这次查询仍可能是 NULL ✓
    //    （两次查表之间协议线程收了 FIN / 判了死 ✓）⇒ 必须再判一次，不能直接解引用 ✓。
    if (!c) { pthread_mutex_unlock(&ffrdp->lock); return FFRDP_CONN_INVALID; }
    t0 = NOW_MS();
    for (;;) {
        ret = MIN(len, c->recv_size);
        if (ret > 0) {
            c->recv_head = ringbuf_read(c->recv_buff, c->recv_cap, c->recv_head, (uint8_t*)buf, ret);
            c->recv_size-= ret; FFRDP_CNT_ADD(c, counter_recv_bytes, ret);
            break;
        }
        // 先判死、再看"要不要等"：已断的连接，即使 timeout=0 也必须报 CLOSED ✓
        //（否则应用在 timeout=0 的轮询里永远学不到"断了"，只会一直拿到 EAGAIN ✗）。
        if (ffrdp_isdead_locked(ffrdp, c)) { ret = FFRDP_CLOSED; break; }
        if (timeout_ms == 0 || ffrdp->closing) { ret = FFRDP_EAGAIN; break; }   // 只是没有数据（还没断）
        if (timeout_ms > 0 && (int32_t)(NOW_MS() - t0) >= timeout_ms) { ret = FFRDP_EAGAIN; break; }
        // 睡在 cond 上等协议送数据来 —— 这一步之后，"等"是真正免费的：不再需要应用去驱动协议。
        {   struct timespec ts;
            int wait_ms = timeout_ms > 0 ? timeout_ms - (int32_t)(NOW_MS() - t0) : FFRDP_COND_WAIT_MS;
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
        c = ffrdp_conn_lookup(ffrdp, conn);
        if (!c) { ret = FFRDP_CLOSED; break; }   // 等的时候连接没了 ⇒ 按"已断"报 ✓（与上面判死同一个口径 ✓）
    }
    pthread_mutex_unlock(&ffrdp->lock);
    return ret;
}

// 排查用（只读 ✓，定义在文件后面）：把 64 位地址打成 "ip:port"。
static const char *ffrdp_addr_text(uint64_t addr, char *buf, int len);

// v2：失联是**每连接**的状态 ⇒ 按句柄问 ✓（v1 只有一个上下文，没法表达"哪条" ✗）。
int ffrdp_close(void *ctxt, int conn)
{
    FFRDPCONTEXT *ffrdp = (FFRDPCONTEXT*)ctxt;
    FFRDPCONN    *c;
    if (!ctxt || conn < 0) return -1;
    pthread_mutex_lock(&ffrdp->lock);
    c = ffrdp_conn_lookup(ffrdp, conn);      // 句柄带代号 ⇒ 不能再按"下标 < FFRDP_MAXCONN"判 ✓
    if (!c) { pthread_mutex_unlock(&ffrdp->lock); return -1; }      // 已经关掉了 / 陈旧句柄 ⇒ 无效 ✓
    if (ffrdp->hs_enable && c->hs_state == HS_UP) {
        // 正常路径：发 FIN，进 FIN_WAIT（重传与超时由 hs_* 那套接管 ✓）；
        // 等对端的 FIN 到了才 DISCONN + 收摊 ✓（那种情况在 ffrdp_hs_rx 里处理 ✓）。
        {   char ab[48];
            printf("[hs] close(handle=%d) peer=%s -> FIN_WAIT t=%u ms\n",
                   conn, ffrdp_addr_text(c->peer_addr, ab, sizeof(ab)), (unsigned)NOW_MS());
        }
        c->hs_state = HS_FIN_WAIT;
        c->hs_start = NOW_MS();
        ffrdp_hs_send(ffrdp, c, FFRDP_HS_FIN);
        ffrdp_arm_timer(ffrdp);
    } else {
        // 还没 UP（握手中/已失败）、或老路径（没有 FIN 这东西 ✗）⇒ 本地拆除 + 报 DISCONN ✓。
        printf("[hs] close(handle=%d) state=%d -> local teardown (no FIN) t=%u ms\n",
               conn, c->hs_state, (unsigned)NOW_MS());
        ffrdp_emit_ex(ffrdp, FFRDP_DISCONN, conn, c->peer_addr, NULL, 0, FFRDP_DISCONN_LOCAL);
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
    if (!ctxt || conn < 0) return -1;
    pthread_mutex_lock(&ffrdp->lock);
    c = ffrdp_conn_lookup(ffrdp, conn);
    ret = c ? ffrdp_isdead_locked(ffrdp, c) : 1;      // 陈旧句柄 = 那条连接已经没了 ⇒ 已断 ✓
    pthread_mutex_unlock(&ffrdp->lock);
    return ret;
}

// 诊断用微秒时钟：只为量"收到帧 → ack 已交给内核"这一段。
static uint64_t ffrdp_us(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)(ts.tv_nsec / 1000);
}

// ══ ack 的**时机**（①-25 起的旋钮；①-41 转正为 1 ✓）═════════════════════════════════
// 1 = 一批帧只回一个 ack：ACK_DUE 留给同一圈的 TIMER 支发（v1.1 形状 ✓）。
//     不额外花一圈 —— lowlevel 每圈"先收活、再取时钟"，回调里 arm 的期限在**同一圈**投递 ✓
//     （lowlevel.h 的约定 ✓），所以 RTT 里不多含一圈。
// 0 = 收到帧就在 RECV 回调里当场回（旧默认 ✗）：批 3 帧就发 3 个 ack。
// 转正依据（lo 实测，docs/ffrdp-optimization.md §3.1）：收包 37.1k/s 里 18.3k 是 ack（49% ✗）、
// 每个 ack 值 2 µs ⇒ 光发 ack 就 3.7% CPU；合并后 ack 数按"圈"收敛（批 3.1 帧 ⇒ −68% ✓）。
#define FFRDP_ACK_COALESCE 1

static void ffrdp_recvdata_and_sendack(FFRDPCONTEXT *ffrdp, FFRDPCONN *c)
{
    FFRDP_FRAME_NODE *p;
    int32_t dist, recv_mack, recv_wnd, recv_wnd_ext, size, i, alen;
    int     stalled;   // 队首就停在交付点上（应用读缓冲满）⇒ 这轮不报位图（§26.104）
    uint8_t data[10];
    uint64_t t_ack0 = ffrdp_us();   // 诊断：回 ack 的**自身**成本（构帧 + 交给内核 ✓）
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
            if (ffrdp_emit(ffrdp, FFRDP_RECVDATA, ffrdp_conn_handle(ffrdp, c),
                           c->peer_addr, c->recv_list_head->data + 4, size) != FFRDP_CONSUMED) {
                c->recv_tail = ringbuf_write(c->recv_buff, c->recv_cap, c->recv_tail, c->recv_list_head->data + 4, size);
                c->recv_size+= size;
            }
            c->recv_seq++; c->recv_seq &= 0xFFFFFF;
            list_remove(&c->recv_list_head, &c->recv_list_tail, c->recv_list_head);
        } else break;
    }
    // 诊断（§26.104）：位图只能报"una 之上**确实收到了**的帧"。交付点 recv_seq 与应用读缓冲绑在一起 ——
    // 应用一时没读、缓冲满（上面那条 `size <= recv_cap - recv_size` 不成立），队首那帧我们**明明收到了**
    // 却交不上去 ✗。此时若照旧按到达队列发位图，等于告诉对端"una 这一帧丢了"：
    // 它会一路快重传同一帧（实测 87 s 里 2086 次），每轮都过一遍拥塞控制 ⇒ cwnd/ssthresh 被钉在 1，
    // 而它的 waste 读到 0.6%（真值 100%）⇒ 连"别再快重传"的自我修正都触发不了 ✗。
    // 判据：队首正好停在 recv_seq 上（= 我们不是有洞，是被应用卡住）⇒ 这一轮**一个位都不报** ✓。
    // 代价只是对端退回 RTO 慢修（它本来就没有零窗口持续探测，快重传在这里没有意义 ✓）。
    stalled = (c->recv_list_head && seq_distance(GET_FRAME_SEQ(c->recv_list_head), c->recv_seq) == 0);
    if (stalled) FFRDP_CNT(c, counter_ack_appstall);
    for (recv_mack = 0, i = 0, p = c->recv_list_head; !stalled && i <= 24 && p; i++, p = p->next) {
        dist = seq_distance(GET_FRAME_SEQ(p), c->recv_seq);
        // dist == 0 只可能是"卡住的队首"（上面已经整段跳过 ✓）；dist < 0 不可能出现（队里按序唯一 ✓）。
        // 旧写法 `dist <= 24` 对 dist == 0 会算 `1 << -1` —— 未定义行为，x86 上按 32 取模置到 bit31，
        // 而 32 位的 mack 里 bit24..31 从来没人读 ⇒ 只是"少报一帧"；真正的坏处是它掩盖了上面那个洞 ✗。
        if (dist > 0 && dist <= 24) recv_mack |= 1 << (dist - 1);
    }
    // ①-43（只读 ✓）：位图只有 24 位。走完 24 个位置后 `p` 还非空 ⇒ **我们已经收下、但这条 ack 报不出去**的帧
    // 还在队里。这个计数是"要不要加宽位图（= 改线上格式，老对端只读前 8/9 字节 ⇒ 必须升版本）"的唯一判据：
    // ≈0 ⇒ 加宽买不到东西，别动（1018-191359 的 `ooo_64reach 99.1%` 也是同一结论 ✓）；真大起来再谈 ✓。
    if (!stalled && p) FFRDP_CNT(c, counter_bm_clip);
    // 这条 ack 报出去的乱序**位数**（与对端的 rx 对照，差额就是被链路吃掉的报告）；一帧可在多条 ack 里置位，所以不是帧数。
    for (i = 0; i < 24; i++) if (recv_mack & (1 << i)) FFRDP_CNT(c, counter_mack_bits_tx);
    // rmss is a divisor learned from whatever arrived (ffrdp_recv_data_frame rejects the lengths that
    // cannot carry a payload; it starts at FFRDP_MAX_MSS). Never divide by it unconditionally.
    recv_wnd = c->rmss ? (c->recv_cap - c->recv_size) / c->rmss : 0;   // ①-38：容量取自字段（值不变 ✓）
    // 窗口扩展（可选第 10 字节）：8 位字段仍是"255 帧封顶"，高出来的部分按 **4 帧/单位** 放进 data[9] ⇒
    // 上限 255 + 4×255 = **1275 帧**。老对端只读前 8/9 字节 ⇒ 看到 min(wnd,255)（比真实值小 ⇒ 安全 ✓，退回今天的行为 ✓）。
    // 取 **floor** 而不是 ceil：宁可少报 3 帧，也不要超报（超报会让对端压爆我们的接收缓冲 ✗）。
    recv_wnd_ext = (recv_wnd > 255) ? MIN((recv_wnd - 255) / 4, 255) : 0;
    recv_wnd = MIN(recv_wnd, 255);
    *(uint32_t*)(data + 0) = (FFRDP_FRAME_TYPE_ACK << 0) | (c->recv_seq << 8);
    *(uint32_t*)(data + 4) = (recv_mack <<  0);
    *(uint32_t*)(data + 4)|= (recv_wnd  << 24);
    // 对端附带的 1 字节独立证据：上次 ack 之后到达的、它早就有的帧数（= 我们白重传了几次）；不带这字节的旧对端留 0，安全。
    if (recv_wnd_ext) {
        data[8] = (uint8_t)MIN(c->dup_seen, 255);   // 有扩展字时这 1 字节必须占位（否则扩展字会落到 dup 的位置 ✗）
        data[9] = (uint8_t)recv_wnd_ext;
        alen = 10;
    } else if (c->dup_seen) { data[8] = (uint8_t)MIN(c->dup_seen, 255); alen = 9; }
    else                    { alen = 8; }
    c->dup_seen = 0;
    // this sendto used to be unchecked, which made an ack the socket refused indistinguishable from an ack
    // the network ate -- and the peer pays for both with a timeout. counting it is the whole change: the
    // ack is still attempted, and nothing here acts on the result.
    if (ffrdp->lowlevel->send(ffrdp->lowlevel, c->peer_addr, data, alen) != 0) FFRDP_CNT(c, counter_send_err); // send ack frame
    // ①：合并之后 ack 改在 TIMER 支里发，原来那段"RECV 回调内"的记账就不存在了 ⇒ 搬到这里 ✓。
    // 读数含义从"收到帧 → 已交给内核"变成"回一个 ack 的自身成本"，dump 里那行因此仍然有意义 ✓。
    {   uint64_t dt = ffrdp_us() - t_ack0;
        c->ack_lat_n++; c->ack_lat_sum_us += dt;
        if (dt > c->ack_lat_max_us) c->ack_lat_max_us = (uint32_t)dt;
    }
}

enum { CEVENT_ACK_OK, CEVENT_ACK_TIMEOUT, CEVENT_ACK_TIMEOUT_MISSED, CEVENT_FAST_RESEND, CEVENT_SEND_FAILED };
static void ffrdp_congestion_control(FFRDPCONN *c, int event)
{
    switch (event) {
    case CEVENT_ACK_OK:
        if (c->cwnd < c->ssthresh) c->cwnd *= 2;
#if FFRDP_CA_RTT_STEP
        // 桩1：标准拥塞避免 —— 一个 RTT 里大约来 cwnd 个 ACK，攒够这么多才涨 1 帧 ⇒ 每 RTT +1 ✓
        else if (++c->cwnd_acc >= c->cwnd) { c->cwnd_acc = 0; c->cwnd++; }
#else
        // 默认：每个 ACK +1。FFRDP_CA_NOQUEUE=1 时只在"没有常驻队列"（rtts 贴着 rtt_min）时才涨 ✓
        else if (!FFRDP_CA_NOQUEUE ||
                 c->rtt_min == 0 || c->rtts == (uint32_t)-1 ||
                 (int32_t)c->rtts <= (int32_t)(c->rtt_min + FFRDP_CA_NOQUEUE_SLACK))
            c->cwnd++;
#endif
        c->cwnd = MIN(c->cwnd, FFRDP_MAX_CWND_SIZE);
        c->cwnd = MAX(c->cwnd, FFRDP_MIN_CWND_SIZE);
        break;
    case CEVENT_ACK_TIMEOUT:
        FFRDP_CNT(c, counter_cwnd_back);
        c->ssthresh = MAX(c->cwnd / 2, FFRDP_SSTHRESH_MIN);
        c->cwnd     = FFRDP_MIN_CWND_SIZE;
        if (c->ssthresh == FFRDP_SSTHRESH_MIN) FFRDP_CNT(c, counter_ssth_floor);   // 由已塌的 cwnd 推出 ⇒ 只剩 +1/ACK ✗
        break;
    case CEVENT_SEND_FAILED:
        FFRDP_CNT(c, counter_cwnd_sfail);
        c->ssthresh = MAX(c->cwnd / 2, FFRDP_SSTHRESH_MIN);
        c->cwnd     = FFRDP_MIN_CWND_SIZE;
        if (c->ssthresh == FFRDP_SSTHRESH_MIN) FFRDP_CNT(c, counter_ssth_floor);   // 由已塌的 cwnd 推出 ⇒ 只剩 +1/ACK ✗
        break;
    case CEVENT_ACK_TIMEOUT_MISSED:
        // 伪超时：等待期间 ack 到过（只是没覆盖这一帧）⇒ 链路没丢，是估计器没看见。
        // FFRDP_PSEUDO_NOCUT=1 ⇒ 不动窗口（证据只留给 rtt_tail/rto）；默认 0 ⇒ 与快速重传同档 ✓。
        if (FFRDP_PSEUDO_NOCUT) { FFRDP_CNT(c, counter_pseudo_nocut); break; }
        // fall through
    case CEVENT_FAST_RESEND:
        FFRDP_CNT(c, counter_cwnd_back);
        c->ssthresh = MAX(c->cwnd / 2, FFRDP_SSTHRESH_MIN);
        c->cwnd     = c->ssthresh;
        if (c->ssthresh == FFRDP_SSTHRESH_MIN) FFRDP_CNT(c, counter_ssth_floor);   // 同上（快速重传/伪超时这一档 ✓）
        break;
    }
#if FFRDP_CA_RTT_STEP
    if (event != CEVENT_ACK_OK) c->cwnd_acc = 0;   // 窗口被减 ⇒ 攒数重新开始 ✓
#endif
}

// 确认等待的原始分布（纯计数 ✓、不参与任何判据 ✗）：桶 16/32/64/128/256/512/1024ms ✓。
static void ffrdp_conf_hist(FFRDPCONN *c, uint32_t conf)
{
    if      (conf <= 16)   FFRDP_CNT(c, counter_conf_16);
    else if (conf <= 32)   FFRDP_CNT(c, counter_conf_32);
    else if (conf <= 64)   FFRDP_CNT(c, counter_conf_64);
    else if (conf <= 128)  FFRDP_CNT(c, counter_conf_128);
    else if (conf <= 256)  FFRDP_CNT(c, counter_conf_256);
    else if (conf <= 512)  FFRDP_CNT(c, counter_conf_512);
    else if (conf <= 1024) FFRDP_CNT(c, counter_conf_1024);
    else                   FFRDP_CNT(c, counter_conf_more);
    if (conf > c->counter_conf_max) c->counter_conf_max = conf;
}

// 一批帧里跨帧累积的东西。旧写法在 recvfrom 循环外累积它；现在由调用方持有：
// 循环整批调用时是"一批一份"，RECV 回调里是"一帧一份" —— 语义相同，只是一次处理几帧的差别。
typedef struct {
    int32_t recv_una;   // 本批内预期的接收序号（洞被填上时往前走）
    int32_t send_una;   // 本批内确认到的最小未确认发送序号
    int32_t send_mack;  // 本批内对方向上报告的接收位图（按 seq 右移合并）
    int     got_data;   // 本批收到过数据帧 ⇒ 要回 ack
    int     got_query;  // 本批收到过查询帧 ⇒ 要回 ack
} FFRDP_RX_BATCH;

// ack 帧：一条 8~9 字节的数据报 ⇒ **就地解析**，别为它分配 1500 字节的节点再拷一遍 ✗（②）。
// 只读 data[0..len-1]，不持有指针 ✓（传输层的缓冲只在回调期间有效 ✓）。线上格式与语义都没变 ✓。
static void ffrdp_on_ack(FFRDPCONN *c, const uint8_t *data, int len, FFRDP_RX_BATCH *b)
{
    int32_t  una, mack, dist, i, j;
    uint32_t w0, w1;

    // 短到读不出 una/window 就丢 ✓（以前读的是节点里的未初始化内存 ✗）。
    // 用 memcpy 取值而不是 `*(uint32_t*)data` ✗：传进来的缓冲不保证 4 字节对齐（栈上 uint8_t[]）。
    if (len < 8) return;
    memcpy(&w0, data + 0, 4); una  = (int32_t)(w0 >> 8);
    memcpy(&w1, data + 4, 4); mack = (int32_t)(w1 & 0xFFFFFFu);

    c->counter_ack_recv++;
    // the peer's count of our retransmits that it had already accepted. this is the one piece of
    // evidence our own counters cannot fake, so it is what the rto floor is driven by.
    if (len >= 9 && data[8]) { FFRDP_CNT_ADD(c, counter_ack_dup, data[8]); c->win_dup += data[8]; }
    // 诊断（§26.103）：对端的 una 除了"不动"还可能**往回走** ✗ —— 那会让发送表把"洞那一帧"当成
    // 已确认删掉，之后只剩洞之上的帧在表里、重传它永远填不上洞（cwnd 被钉在 1 + 快重传自锁的候选根因 ✓）。
    if (c->counter_ack_recv > 1) {
        if ((uint32_t)una == c->ack_una) FFRDP_CNT(c, counter_ack_una_dup);   // the peer did not move
        else if (seq_distance((uint32_t)una, c->ack_una) < 0) FFRDP_CNT(c, counter_ack_una_regress); // moved backwards ✗
    }
    c->ack_una = (uint32_t)una;

    // A′：这条 ack 是不是探针的答复（见 tick 里那段）✓
    if (c->flags & FLAG_QUERY_PROBE) {
        FFRDP_FRAME_NODE *u = NULL;
        for (u = c->send_list_head; u; u = u->next) if (u->flags & FLAG_FIRST_SEND) break;
        c->flags &= ~FLAG_QUERY_PROBE;
        if (u && seq_distance(una, GET_FRAME_SEQ(u)) <= 0) {
            // 对端明确说"还是没有它" ⇒ 别等满 RTO：把到期时刻提前到"现在"，下一圈走原样的重传路径 ✓
            u->tick_timeout = NOW_MS();
            FFRDP_CNT(c, counter_tailprobe_lost);
        }
        // 否则：ack 已经越过那一帧 ⇒ 它**到了**，丢的只是 ack ⇒ 什么都不用做（正常路径正在移除它 ✓）
    }
    if (mack) { // the peer reported frames that arrived after a hole
        FFRDP_CNT(c, counter_ack_mack);
        // counted in bits, to be compared with counter_mack_bits_tx on the other side: a
        // report that never arrived and a report that was never made look identical from here.
        for (j = 0; j < 24; j++) if (mack & (1 << j)) FFRDP_CNT(c, counter_mack_bits_rx);
        for (i = 23; i >= 0 && !(mack & (1 << i)); i--);
        for (j = 0; j < i && (mack & (1 << j)); j++);
        if (j < i) FFRDP_CNT(c, counter_ack_hole); // a gap below the highest acked frame
    }
    dist = seq_distance(una, b->send_una);
    if (dist >= 0) {
        b->send_una  = una;
        b->send_mack = (b->send_mack >> dist) | mack;
        // the peer's window is a buffer size, not a rate: on the measured link it allowed 148
        // frames where the path carried five, and the rest sat in its buffer as a standing queue
        // of about 200ms. cap is what a probe has proven, and what is being tested right now.
        // 窗口：8 位字段 + 可选的第 10 字节（4 帧/单位 ✓）。老对端只发 8/9 字节 ⇒ 扩展按 0 处理，
        // 行为与今天逐字节相同 ✓；反过来它读我们的 10 字节也只认前 8 位（= min(wnd,255) ⇒ 安全地变小 ✓）。
        c->swnd = (uint32_t)data[7] + ((len >= 10) ? (uint32_t)data[9] * 4u : 0u);
        c->peer_wnd_raw = c->swnd;
        // 信用不再夹到 cap：探测驱动的是"在途量"这道真闸，再夹一次就是第二道刹车（真机上会把发送饿死）。
        if (c->swnd > FFRDP_MAX_WAITSND) c->swnd = FFRDP_MAX_WAITSND;
        c->tick_recv_ack = NOW_MS();
    }
}

// 一帧：解析、记账。node 由调用方分配好（data/size 已就绪），这样"复用同一个节点"的旧写法保持
// 不变。返回 1 = 已移交给乱序队列（调用方不要再碰它）；返回 0 = 调用方可以复用或释放它。
static int ffrdp_on_frame(FFRDPCONN *c, FFRDP_FRAME_NODE *node, int len, FFRDP_RX_BATCH *b)
{
    int32_t dist;
    int     taken = 0;

    if (node->data[0] <= FFRDP_FRAME_TYPE_FEC32) { // data frame
        node->size = len; // frame size is what the transport handed over
        if (ffrdp_recv_data_frame(c, node) == 0) {
            dist = seq_distance(GET_FRAME_SEQ(node), b->recv_una);
            if (dist == 0) { b->recv_una++; }
            // out of order arrival = this frame is ahead of a hole. If this counter grows while the
            // peer's acks carry no mack bit, then the hole is never being reported, and the sender
            // has no choice but to wait for timeouts.
            if (dist > 0) {
                FFRDP_CNT(c, counter_recv_ooo);
                if (dist > 23) FFRDP_CNT(c, counter_recv_far);
                // 越过洞的距离只靠"洞被填上"缩小，而那时它已在 una 之下、由累积 ack 覆盖 ⇒ 超过位图最后一 bit 的帧根本报不出来。
                if      (dist <= 24) FFRDP_CNT(c, counter_ooo_d1_24);
                else if (dist <= 64) FFRDP_CNT(c, counter_ooo_d25_64);
                else {   // > 64: the tail one extension word cannot carry, split by how far it goes
                    FFRDP_CNT(c, counter_ooo_dgt64);
                    if (dist <= 128) FFRDP_CNT(c, counter_ooo_d65_128);
                    else             FFRDP_CNT(c, counter_ooo_dgt128);
                }
            }
            // below the range we have already taken in: the peer sent this again although we had it,
            // which is exactly the waste we want to count and report back.
            if (dist < 0) { FFRDP_CNT(c, counter_recv_dup); c->dup_seen++; }
            // further ahead than the window we hand out in every ack: the peer is credit limited to
            // that, so this is a lie or a duplicate. storing it would let anyone who can reach the
            // port spend our memory at one frame per datagram, for ever (see FFRDP_RECV_QUEUE_MAX).
            if (dist >= (int32_t)FFRDP_RECV_QUEUE_MAX) { FFRDP_CNT(c, counter_recv_drop); }
            else if (dist >= 0) {
                // 诊断（§26.104）：队里已经有这一帧 ⇒ 对端重传了一份**它以为丢了、我们其实收到了**的副本 ✗。
                // 旧写法让 list_enqueue 静默 free() ⇒ recv_dup 恒不动、dup_seen 不涨 ⇒ 对端的 waste 读到
                // 0.6%（真值是 100%）⇒ hole_wait 一直是 0 ⇒ 它对同一个"洞"快重传 2086 次、cwnd 钉在 1 ✗。
                // 这里记一次 dup ⇒ 下一次 ack 的 dup_seen 就把这份证据送回对端 ✓。
                // ⚠️ 两种结果都要 taken = 1：重复那份已经被 list_enqueue 内部 free 掉了 ✓ ——
                // 这里若留给调用方（`if (!ffrdp_on_frame(...)) free(node)`）就是**同一块内存 free 两次** ✗
                if (list_enqueue(&c->recv_list_head, &c->recv_list_tail, node)) {
                    FFRDP_CNT(c, counter_recv_dup); c->dup_seen++;
                }
                taken = 1;
            }
            b->got_data = 1;
        }
    } else if (node->data[0] == FFRDP_FRAME_TYPE_ACK) { // ack frame
        ffrdp_on_ack(c, node->data, len, b);            // 语义与以前逐行一致，只是搬走了 ✓（就地解析也走它 ✓）
    } else if (node->data[0] == FFRDP_FRAME_TYPE_QUERY) b->got_query = 1;
    return taken;
}

// RTT 采样 + 地板维护。抽成函数是为了让 `t_cap` 能直接喂"离群样本"—— 伪造一个 ack 做不到 ✓。
// 两条保护都来自 2026-10-17 的真机复测（200 ms 链路，见 notes §26.97）：
//   · `rtt_min` 被一个 60 ms 的假样本钉死后，B2 的"没队列"`rtts <= rtt_min + slack` 恒假 ⇒ 一次都不升 ✗
//   · 同时探测的"排队"`rtts > 3 × rtt_min` 恒真 ⇒ 一路 revert，把 cap 从 64 砍到 40 ✗
static void ffrdp_rtt_sample(FFRDPCONN *c, uint32_t sample)
{
    // ✗→✓（§26.108）样本语义是"这一帧**首发** → 它被确认"，但**排在洞后面的帧**要等洞被修好才被确认：
    // 洞卡 70 s，它的样本就是 70 s —— 那是**修复时间**，不是往返时间 ✗。它也没被重传过，所以 §26.107 的
    // FLAG_RESENT 挡不住它。
    // 物理上根本排不出这种队：窗口只有 128 帧 ≈ 164 KB，1.5 MB/s 下最多排 0.1 s
    // （1018-180434 实测 rttm 9937 / rtts 9490 ms ✗）。被污染的 rtts 会连带把 rto 顶到 2000、
    // hole_wait 顶到 300 ms、probe_hold 顶到 2×rtt+500 ⇒ 修复慢、探测窗口离谱 ⇒ 上限被压到 4~58 ✗。
    // 处理方式**钳而不丢**：真链路变慢时样本仍能一步步涨（每样本最多 4 倍），不会把估计冻结在低值 ✓。
    if (c->rtts != (uint32_t)-1 && c->rtts > 0) {
        uint32_t cap = c->rtts * 4 + FFRDP_RTT_SAMPLE_CLAMP_MS;
#if FFRDP_RTT_CLAMP_BY_MIN
        // ①-45：`cap` 拿 rtts 自我参照 ⇒ 被污染的样本流能一级一级把上限**顶上去**（棘轮：2ms→8→32→…；
        // 1018-191359 实测 c4 rtts 18435 ms / rtt_min 2 ms = 9217 倍，而窗口 128 帧 ≈ 164 KB 在这条
        // 0.5 MB/s 的链上最多排 0.3 s ⇒ 那个数物理上排不出来）。rtts 一旦被顶到秒级就连带把
        // rto 顶到 2000（`reach_maxrto` 34~40）、把整形压到 64 fps 地板、把 hole_wait/probe_hold 放大，
        // 于是**修一个洞要 2 s**（`timeout diag max wait` 16~21 s 就是这么堆出来的）。
        // 所以再拿 rtt_min 兜一个**绝对**上限：物理排队 = 窗口/速率 ≤ 4×rtt_min + CLAMP_MS ✓。
        // rtt_min 本身有"只允许同量级下调"的保护（见下）⇒ 不会被单次噪声带低 ✓，链路真变慢时 rtts
        // 仍能涨到 4×rtt_min + CLAMP_MS（跟着 rtt_min 一起涨）⇒ 不冻结估计 ✓。
        uint32_t cap_min = c->rtt_min * 4 + FFRDP_RTT_SAMPLE_CLAMP_MS;
        if (c->rtt_min > 0 && cap > cap_min) cap = cap_min;
#endif
        if (sample > cap) { sample = cap; FFRDP_CNT(c, counter_rtt_clamp); }
    }
    c->rttm = (int32_t)sample;
    if (c->rtt_min == 0 || sample < c->rtt_min) {
        // 只允许"与平滑值同量级"的新低下调地板：单次离群样本（差 2 倍以上）只当噪声 ✓。
        // 真变快时 rtts 会跟着掉，之后同量级的样本照样能下调 ⇒ 最多滞后几秒，不会被永久钉死 ✓。
        if (c->rtt_min == 0 || c->rtts == (uint32_t)-1 || sample * 2u >= c->rtts) c->rtt_min = sample;
    }
    if (c->rtts == (uint32_t)-1) {
        c->rtts = sample;
        c->rttd = sample / 2;
    } else {
        c->rtts = (7 * c->rtts + 1 * sample) / 8;
        c->rttd = (3 * c->rttd + 1 * (uint32_t)abs((int)sample - (int)c->rtts)) / 4;
    }
}

#if FFRDP_PROBE_QUEUE_BY_MODEL
// ①-46：**我们自己**能造出来的排队深度（ms）= 在途帧数 / 确认速率（帧/秒）。
// 用途：判断"典型 RTT 比地板高出来的那一截"到底是不是我们的窗口排出来的。
//   · out=10 帧、rate=800 fps ⇒ 12 ms：这时候 rtts 比 rtt_min 高 90 ms，多出来的 78 ms 就不是我们的 ✗
//   · out=128 帧、rate=800 fps ⇒ 160 ms：此时 excess 90 ms ≤ 2×160 ⇒ 队列确实可能是我们排的 ✓
// 返回 0 表示速率还没采到（调用方据此退回老的比值判据 ✓）。
static uint32_t ffrdp_probe_q_ours_ms(FFRDPCONN *c, uint32_t out)
{
    if (c->rate_now == 0) return 0;
    return out * 1000u / c->rate_now;
}
#endif /* FFRDP_PROBE_QUEUE_BY_MODEL */

// 处理完一段接收之后要做的：把收到的 ack 应用到发送表 —— 释放已确认的帧、更新 rtt/rto 下限、
// 触发快速重传，并在收到数据帧时回一个 ack。旧写法是"整批一次"，RECV 回调里是"一帧一次"。
static void ffrdp_apply_acks(FFRDPCONN *c, FFRDP_RX_BATCH *b)
{
    FFRDP_FRAME_NODE *p, *t;
    int32_t dist, maxack, i;

    // 不在这里回 ack，只记一笔。lowlevel 的一圈会把当前就绪的包都送上来（有上限），所以"这一轮收到的帧"
    // 就是一个批次；ack 报的是 recv_seq + 位图，第二个 ack 覆盖的帧第一个已经报过了，什么都不多带。
    if (b->got_data || b->got_query) c->flags |= FLAG_ACK_DUE;
    if (c->send_list_head && seq_distance(b->send_una, GET_FRAME_SEQ(c->send_list_head)) >= 0) { // handle frame ack
        for (p = c->send_list_head; p;) {
            dist = seq_distance(GET_FRAME_SEQ(p), b->send_una);
            for (i=23; i>=0 && !(b->send_mack&(1<<i)); i--);
            if (i < 0) maxack = (b->send_una - 1) & 0xFFFFFF;
            else maxack = (b->send_una + i + 1) & 0xFFFFFF;

            if (dist > 24 || !(p->flags & FLAG_FIRST_SEND)) break;
            else if (dist < 0 || (dist > 0 && (b->send_mack & (1 << (dist-1))))) { // this frame got ack
                FFRDP_CNT_ADD(c, counter_send_bytes, frame_payload_size(p)); c->wait_snd--;
                c->tx_stalled = 0;   // ①-41e：ack 来了 ⇒ 槽位/信用可能腾出来了 ⇒ 值得一试 ✓
                ffrdp_congestion_control(c, CEVENT_ACK_OK);
                // ⚠️ 条件里的 `tick_send == tick_1sts`：RTO 触顶那条路径会刷新 `tick_send` 并**清掉**
                // `FLAG_TIMEOUT_RESEND` ⇒ 只看标志挡不住，ack 回来会测出"重传→ack"的假小值 ✗。
                // 采样一律从**首次**发送算起（`tick_1sts` 不再被改写）⇒ 最坏偏大（保守），绝不把地板钉低 ✓。
                // ✗→✓（§26.107）原来的守卫只看 FLAG_TIMEOUT_RESEND + `tick_send == tick_1sts`，
                // 而**快速重传**那条分支既不清 tick_send、又把 FLAG_TIMEOUT_RESEND 也清掉（1979 行）
                // ⇒ 被快重传过的帧照样喂 rtts。它的"首发→确认"里含着等位图/等洞的时间，量级是**秒** ✗：
                // 1018-173619 实测 `rttm 3217 / rtts 3184 ms`（1604 次快重传），于是 rto 顶到 2000、
                // hole_wait 顶到 300 ms、probe_hold 变成 2×3.2s+500ms=6.9 s 且 `holding` 恒真 ⇒ 逐轮探测、
                // 上限被压到 44~64。真实的 RTT 是 1~20 ms —— 这个数字是被自己污染出来的 ✗。
                if (!(p->flags & (FLAG_TIMEOUT_RESEND | FLAG_RESENT)) && p->tick_send == p->tick_1sts) {
                    ffrdp_rtt_sample(c, (uint32_t)((int32_t)NOW_MS() - (int32_t)p->tick_1sts));
                    {   // The peer's duplicate reports are the one piece of evidence on this path that our own
                        // 对端说"这些重传我早就有"的占比高 ⇒ RTO 还在往返分布里，白花的钱在线上 ⇒ 把地板顶上去；地板从当前 RTO 起步，一步即生效。
                        if (c->win_need >= FFRDP_RTO_WIN ||
                            (int32_t)NOW_MS() - (int32_t)c->tick_win >= 2000) {
                            c->tick_win = NOW_MS();
                            if (c->win_need >= FFRDP_WASTE_MIN) {
                                uint32_t waste = c->win_need ? c->win_dup * 100 / c->win_need : 0;
                                // 同一份证据也决定"多大的洞才可信"：对端把我们的修复大多称为无用时，洞多半只是路上被超车 ⇒ 快路要等；修复是真的时候就一次都不等。
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
                            // 封顶每个窗口都做（链路变快后地板要自己降下来，哪怕已不再重传）；但"安静窗口"**不**喂进衰减路径 —— 那多半是地板在正常工作。
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
                    uint32_t conf  = NOW_MS() - p->tick_send;
                    ffrdp_conf_hist(c, conf);   // 只记账 ✓（限速前 ✓，要的就是原始分布 ✓）
                    uint32_t allow = c->rtt_tail ? c->rtt_tail + c->rtt_tail / 2 + 25 : 64;
                    if (allow > (uint32_t)FFRDP_MAX_RTO) allow = (uint32_t)FFRDP_MAX_RTO;
                    if (conf  > allow) conf = allow;
                    if (conf  > c->rtt_tail) c->rtt_tail = conf;
                }
                t = p; p = p->next; list_remove(&c->send_list_head, &c->send_list_tail, t); c->counter_delivered++; continue;
            } else if (seq_distance(maxack, GET_FRAME_SEQ(p)) > 0) {
                // the peer reported something above this frame, so it is either lost or merely late.
                if ((int32_t)NOW_MS() - (int32_t)p->tick_send >= (int32_t)c->hole_wait) {
                    // ⚠️ 这一分支在**每次 ack** 都会重走发送表 ✗ ⇒ 原来同一帧会被反复判减半 ✓；
                    //    守卫从 ①-30 起**默认开** ✓ ⇒ 只认第一次（那一帧的 FLAG_FAST_RESEND 由重传路径清 ✓）。
                    if (!FFRDP_HOLE_CUT_ONCE || !(p->flags & FLAG_FAST_RESEND)) {
                        ffrdp_congestion_control(c, CEVENT_FAST_RESEND);
                        p->flags |= FLAG_FAST_RESEND;
                    }
                }
            }
            p = p->next;
        }
    }
}

// 协议要 lowlevel 什么时候叫它：把所有"下一次要做的事"折算成一个最早时刻。0 = 没有期限，睡到有包为止。
#if FFRDP_PACE_ENABLE
// C1：令牌补充（整数毫秒粒度 ✓，平台不需要微秒时钟）。
// 速率取 **在途上限 / RTT**（= 这条链路按窗口算允许的帧率 ✓），不再用 rate_now：
//   用 rate_now 会掉进反馈陷阱 —— 整形一压，rate_now 跟着降，令牌就更少 ⇒ 变成限速器 ✗（实测 lo 上
//   从 56 MB/s 掉到 0.47 MB/s）。窗口/RTT 是物理量，不受整形影响 ✓。
// 再乘 FFRDP_PACE_RATE_PCT 留余量 ⇒ 只在**形状**上摊平，不砍均值速率 ✓。
// 返回 0 = **不整形**（回环 / RTT 还没采样 / RTT 太小 ✓）。三件事一起做掉：
//   ① 锯齿的周期就是 RTT：rtts 只有几百微秒时，"打一梭子 + 静默"在这个尺度上根本不成问题 ✗；
//   ② 整形的兑现靠 ms 级期限，而平台的**实际唤醒**是几毫秒（WSL/lo 上实测）⇒ 每次唤醒只推一桶
//      （2 ms 的量）⇒ 均值被"桶 ÷ 唤醒周期"限住（lo：47.3 → 35.9 MB/s，−24%，notes §26.99 ✓）；
//   ③ 早期 rtts 未采样（-1）时也不该限速 ⇒ 启动不被拖慢 ✓。
static uint32_t ffrdp_pace_fps(FFRDPCONN *c)
{
    uint32_t rtt = c->rtts, lim = c->inflight_max ? c->inflight_max : (uint32_t)FFRDP_INFLIGHT_MAX, fps;
#if FFRDP_PACE_RTT_CAP_X
    // ①-44：整形速率 = 窗口 / RTT，而 `rtts` 在拥塞塌陷时会被"排在洞后面的帧"撑到 rtt_min 的几十~几千倍
    // （1018-191359：c4 `rtts 18435` vs `rtt_min 2` ⇒ fps 掉到 MIN_FPS 地板 64 = 82 KB/s，与它实测的
    // 0.12 MB/s 对上）⇒ 令牌桶从"摊平形状"变成"二次限速器" ✗。用于整形的 RTT 夹到 rtt_min × CAP_X：
    // 正常链路（rtts ≤ 4×rtt_min + 抖动）逐字不变 ✓，只有塌陷那一段不再跟着塌 ✓。
    if (c->rtt_min > 0 && rtt != (uint32_t)-1) {
        uint32_t cap = c->rtt_min * (uint32_t)FFRDP_PACE_RTT_CAP_X;
        if (cap < (uint32_t)FFRDP_PACE_MIN_RTT) cap = (uint32_t)FFRDP_PACE_MIN_RTT;
        if (rtt > cap) rtt = cap;
    }
#endif
    if (rtt == (uint32_t)-1 || (int32_t)rtt < (int32_t)FFRDP_PACE_MIN_RTT) return 0;
    fps = (uint32_t)(((uint64_t)lim * 1000u) / rtt);
    fps = fps * FFRDP_PACE_RATE_PCT / 100u;
    if (fps < (uint32_t)FFRDP_PACE_MIN_FPS) fps = (uint32_t)FFRDP_PACE_MIN_FPS;
    return fps;
}

static void ffrdp_pace_refill(FFRDPCONN *c, uint32_t now)
{
    uint32_t dt = now - c->pace_tick, per16, burst16, fps = ffrdp_pace_fps(c);
    c->pace_fps = fps;
    if (fps == 0) {                         // 不整形：桶恒满、不设期限 ⇒ 发送循环与 next_due 都当它不存在 ✓
        c->pace_tok16 = 2u * 16u;
        c->pace_tick  = now;
        c->pace_due   = 0;
        return;
    }
    if ((int32_t)dt < (int32_t)FFRDP_PACE_MS) return;
    per16   = (fps * FFRDP_PACE_MS * 16u) / 1000u;                 // 每个周期补的 1/16 帧
    if (per16 < 1) per16 = 1;
    burst16 = per16 * FFRDP_PACE_BURST_MS;
    // 桶深下限 = **2 帧**（32 个 1/16）。少了就是死锁，不是"慢一点"：
    // 慢链路上 per16 很小（rtts 253ms + 96 帧窗口 ⇒ fps≈474 ⇒ per16=7 ⇒ 2 ms 桶 = 14），
    // 而首发要求 tok >= 16 ⇒ tok 被夹在 14 永远够不到 ⇒ 首发全被挡、在途帧重传完就静默，
    // 上层"10s 没字节 ⇒ 对端失联"于是重连（实测 w96p1：每段 ~13s 才送 0.88MB，120s 重连 5 次）。
    // 取 2 帧而不是 1 帧：桶只装 1 帧时，攒 16 个令牌要 3 ms（21-16 白扔 5 个）⇒ 实得速率掉到 2/3。
    if (burst16 < 2u * 16u) burst16 = 2u * 16u;
    c->pace_tok16 += (dt / FFRDP_PACE_MS) * per16;
    if (c->pace_tok16 > burst16) c->pace_tok16 = burst16;   // 睡久了也不能攒出一大坨 ⇒ 夹住 ✓
    c->pace_tick = now;
    c->pace_due  = (c->pace_tok16 >= 16) ? 0 : (now + FFRDP_PACE_MS);
}
#endif

static uint32_t ffrdp_next_due(FFRDPCONTEXT *ffrdp, FFRDPCONN *c)
{
    const FFRDP_FRAME_NODE *q;
    uint32_t now = NOW_MS(), due = 0, x;
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
        if (entry < (int)c->cwnd && (!due || (int32_t)(q->tick_timeout - due) < 0)) { due = q->tick_timeout; }
        // A′：探针期限 = 那一帧的 RTO 到期前 1/4（与 tick 里的判据**同一个式子** ✓）。
        // 不加这一条，静默连接上唯一的叫醒就是 RTO 本身 ⇒ 探针永远晚到 ⇒ 等于没做 ✗。
        if ((q->flags & FLAG_FIRST_SEND) && !(c->flags & FLAG_QUERY_PROBE) &&
            c->rtts != (uint32_t)-1 && c->rto >= 4 * (c->rtts + 2)) {
            x = q->tick_timeout - c->rto / 4;
            if (!due || (int32_t)(x - due) < 0) { due = x; }
        }
    }
    if (c->cur_new_node) {
        // 与 tick 里那条判据**必须同一个值**（IDLE 更小）⇒ 否则周期算不到，半帧要等别的期限才醒来 ✗
        x = c->cur_new_tick + FFRDP_FLUSH_IDLE;
        if (!due || (int32_t)(x - due) < 0) { due = x; }
    }
    x = c->rate_tick + 1000;
    if (!due || (int32_t)(x - due) < 0) { due = x; }
    // 握手期的重传期限（与 tick 里那条重传判据同一个值 ✓）。
    if (c->hs_state == HS_SYN_SENT || c->hs_state == HS_SYN_RCVD || c->hs_state == HS_FIN_WAIT) {
        x = c->hs_tick + FFRDP_HS_RETRY_MS;
        if (!due || (int32_t)(x - due) < 0) { due = x; }
    }
    // keepalive：UP 的连接也要有人来叫醒（否则真 lowlevel 会一直睡到下次有数据 ✗）——
    // 半程探一次（周期跟 idle_ms 走 ⇒ 不会出现"探测周期比超时还长"的自相矛盾组合 ✓）。
    if (c->hs_state == HS_UP) {
        // ⚠️ 2026-10-15（§26.40）：起点要取"最后听到对端"与"最后发 QUERY"里**较晚**的那个 ✓。
        x = MAX(c->tick_last_recv, c->tick_send_query) + (uint32_t)(ffrdp->idle_ms / 2);
        if (!due || (int32_t)(x - due) < 0) { due = x; }
    }
    // 查询窗口的期限：**只有真的欠信用时才需要它** ✓。tick 里那条查询分支长在
    // `c->swnd == 0` 那一段里（没信用、还有帧等着首发才会问对端）⇒ swnd 充足时这条期限根本不可达 ✗
    if (c->tick_send_query && c->swnd == 0) {
        x = c->tick_send_query + FFRDP_QUERY_CYCLE;
        if ((int32_t)(x - now) > 0 && (!due || (int32_t)(x - due) < 0)) { due = x; }
    }
    if (c->probe_next) { x = c->probe_next; if (!due || (int32_t)(x - due) < 0) { due = x; } }
#if FFRDP_PACE_ENABLE
    // C1：被令牌挡住时，下一次叫醒就是"补令牌"那一刻（ms 粒度 ✓）—— 不能报"现在"，否则整圈空转 ✗
    if (c->pace_due)   { x = c->pace_due;   if (!due || (int32_t)(x - due) < 0) { due = x; } }
#endif
    // 还差一条最容易漏、也最贵的：**现在就有发得出去的东西**（应用排了队，或者发送表里有还没首发的帧），
    // 而且发得出去（有信用、在途数没到限）⇒ 立刻叫一次。
    if ((c->tx_unsent > 0 && c->wait_snd < (uint32_t)FFRDP_MAX_CWND_SIZE) ||
        (pending_first && sent_before_pending < (int)c->cwnd)) {
        uint32_t limit = MAX(c->inflight_max ? c->inflight_max : FFRDP_INFLIGHT_MAX, (uint32_t)FFRDP_INFLIGHT_MIN);
        // 用本圈刚数出的 n_sent，不用 c->inflight_used：后者只在 tick 里刷新，而 ack 走另外两条路径 ⇒ 会读到旧值、漏叫醒。
        {   // `now` 就是本函数入口取的那个值（同一次调用里还是同一毫秒 ✓）⇒ 不再多调一次时钟 ✓
            int quiet = (c->tick_recv_ack != 0) && (ffrdp->idle_ms > 0) &&
                        ((int32_t)(now - c->tick_recv_ack) > (int32_t)(ffrdp->idle_ms / 2));
            // ①-41e：上一圈被闸门挡住了（tx_stalled ✓）⇒ 这次别再急着要"现在" ✗ —— 那叫一次也
            // 只是空转 ✗。让它睡到 ack 来敲门（ack 会清掉 tx_stalled ✓，见 ffrdp_apply_acks）✓。
            // C1：被令牌挡住时同样别要"现在" —— 叫醒了也发不出去，只会空转到下一毫秒 ✗
            if (!c->tx_stalled && !FFRDP_PACE_BLOCKED(c) && !quiet && c->swnd > 0 && (uint32_t)n_sent < limit) { due = now; armed_now = 1; }
        }
    }
    // ⚠️ tick 判"到期要重传"用的是 `now - tick_timeout > 0`（**严格**大于 ✓）⇒ tick_timeout 恰好等于
    //    now 的帧，这一圈不会被重传 ✗。若把它当"现在"叫醒，就是在**同一个冻结的 tick 里**空转 ✓
    if (!armed_now && due && (int32_t)(due - now) == 0) {
        due = now + 1;
    }
    if (due && (int32_t)(due - now) < 0) due = now;       // 已经过期 ⇒ 立刻（这也是"快一点"的做法）
    // 兜底：有未确认的帧时最多睡 4ms。上面那条判据修好后它不该再触发；
    // 真机上 lap 最大曾到 59/114/222ms，每漏一次小窗口就少送好几帧，而多醒几次只费一点 CPU。
    if (c->send_list_head) { if (!due || (int32_t)(due - (now + 4)) > 0) due = now + 4; }
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
        if (c->flags & FLAG_ACK_DUE) { x = NOW_MS(); ffrdp->arm_ack_win++; }
        if (!due || (int32_t)(x - due) < 0) { due = x; win = i; }
    }
    // 只读（§26.38 ①-18）：取 min 之后的那一跳 —— 分档 + 赢家 ✓（不改行为 ✗）
    ffrdp->arm_calls++;
    {   int32_t left = (int32_t)(due - NOW_MS());
        if      (!due)      ffrdp->arm_d0++;
        else if (left <= 0) ffrdp->arm_dnow++;
        else if (left <= 4) ffrdp->arm_dnear++;
        else                ffrdp->arm_dfar++;
    }
    if (win >= 0 && win < FFRDP_MAXCONN) ffrdp->arm_win[win]++;
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
    uint32_t s0 = 0;   // 本圈开始时"已发帧数"的快照（①-41e 的 tx_stalled 判据 ✓）
    // ①-41：成帧已经在 ffrdp_send（应用线程）里做完 ⇒ 这里不再 drain ✓。
    // ①-35c：应用喊过"叫醒"、这一轮工作线程已经跑过 ⇒ 允许它下次再喊 ✓（原来的清零点在 drain 里 ✓）。
    c->tx_woken = 0;
    // ── keepalive（v2：idle_ms 终于接上了 ✓）────────────────────────────────
    // 空闲 = 半程没听到对端 ⇒ 主动发一个 QUERY 去敲它（对端的 ack 回来我们就续命 ✓）。
    if (ffrdp->hs_enable && c->hs_state == HS_UP) {
        uint32_t now2 = NOW_MS();
        uint32_t idle = ffrdp_idle_deadline(ffrdp, c);   // 与 isdead 同一个口径：不小于 4 个 RTT ✓
        if ((int32_t)(now2 - c->tick_last_recv) >= (int32_t)idle) {
            {   char ab[48];
                printf("[hs] keepalive dead: handle=%d peer=%s silent=%u ms (deadline=%u, rtts=%u) t=%u ms\n",
                       (int)c->handle, ffrdp_addr_text(c->peer_addr, ab, sizeof(ab)),
                       (unsigned)(now2 - c->tick_last_recv), (unsigned)idle, (unsigned)c->rtts, (unsigned)now2);
            }
            c->hs_state = HS_FAILED;
            ffrdp_emit_ex(ffrdp, FFRDP_DISCONN, ffrdp_conn_handle(ffrdp, c), c->peer_addr, NULL, 0, FFRDP_DISCONN_TIMEOUT);
            if (c != &ffrdp->conn) ffrdp_conn_free(ffrdp, c);
            return;
        }
        if ((int32_t)(now2 - c->tick_last_recv) >= (int32_t)(idle / 2) &&
            (c->tick_send_query == 0 || (int32_t)(now2 - c->tick_send_query) >= ffrdp->idle_ms / 2)) {
            uint8_t q[1];
            q[0] = FFRDP_FRAME_TYPE_QUERY;
            if (ffrdp->lowlevel->send(ffrdp->lowlevel, c->peer_addr, q, 1) != 0) FFRDP_CNT(c, counter_send_err);
            c->tick_send_query = now2;
            FFRDP_CNT(c, counter_send_query);
        }
    }

    // 出口 ② 的判据：**应用停手超过 IDLE**（或应用显式 flush）✓。不是"每 IDLE 冲一次"：
    // 持续供数据时 `cur_new_tick` 一直被刷新 ⇒ 帧总是攒满走出口 ①，这条不成立 ⇒ 吞吐不受影响 ✓。
    if (c->cur_new_node && ((int32_t)NOW_MS() - (int32_t)c->cur_new_tick > FFRDP_FLUSH_IDLE || c->flags & FLAG_FLUSH)) {
        c->cur_new_node->data[0] = FFRDP_FRAME_TYPE_SHORT;
        c->cur_new_node->size    = 4 + c->cur_new_size;
        ffrdp_frame_encrypt(ffrdp, c, c->cur_new_node);   // 出口 ②：空闲/flush 的短帧 ✓
        list_enqueue(&c->send_list_head, &c->send_list_tail, c->cur_new_node);
        c->send_seq++; c->wait_snd++;
        c->cur_new_node = NULL;
        c->cur_new_size = 0;
    }
    // FLAG_FLUSH 是**一次性**请求 ⇒ 这一圈处理完就撤 ✓。原来从不撤销 ✗：调过一次 ffrdp_flush()
    // 之后每条连接都永久变成"有一点点就立刻发"，等于把短帧自动冲刷常开（帧数/CPU 代价全在这 ✗）。
    c->flags &= ~FLAG_FLUSH;

    // ── A′：尾部探测（notes §26.96）───────────────────────────────────────────
    // 要解决的是**尾丢**：洞后面没有更高的序号被收到 ⇒ 快速重传那条证据（对端报了更高序号）永远不来，
    // 只能等满 RTO，而 RTO 一旦超时还要 ×1.5 退避（链式的，这才是真正贵的那部分 ✗）。
    // 做法：在 RTO 的 3/4 处发一个 **QUERY**，让对端立刻回 ack：
    //   ① 对端其实有那一帧（丢的是 ack）⇒ 正常路径就把帧收掉 ⇒ **连重传都省了**，而且不进退避 ✓；
    //   ② 对端没有 ⇒ 把 una 帧的到期时刻提前到"现在"，下一圈走**原样的** RTO 重传路径 ✓（不新增状态）。
    // 为什么是 QUERY 而不是直接重传（那才是标准 TLP）：QUERY 不带数据序号 ⇒ 对端不会把它算成
    // duplicate ⇒ **不污染 waste / rto_floor 那套证据** ✓ —— 这正是 A3 方案过不去的那道坎 ✓。
    // 只在"收益为正"时出手：要求 RTO ≥ 4×(RTT+2) ⇒ 提前问一句省下的 1/4 RTO 一定大于往返成本 ✓；
    // 健康链路（RTO ≈ 1.2×RTT）根本不触发 ⇒ 这一条在没有收益的区间是**零动作** ✓。
    if (!(c->flags & FLAG_QUERY_PROBE) && c->rtts != (uint32_t)-1 &&
        c->rto >= 4 * (c->rtts + 2)) {
        FFRDP_FRAME_NODE *u = NULL;
        for (u = c->send_list_head; u; u = u->next) if (u->flags & FLAG_FIRST_SEND) break;
        // 静默判据：**最近 rto/4 内一个 ack 都没有**。这一条不能写成"这一帧发出去之后没收到过 ack"
        // （`u->ticket == counter_ack_recv`）✗：尾部那一帧正是"前面的帧都被确认、只剩它"的形状 ⇒
        // 它首发之后当然还来过 ack ⇒ 那个条件在真实尾丢里**恒不成立**（实测 probe 恒为 0 ✗）。
        if (u && (int32_t)NOW_MS() - (int32_t)c->tick_recv_ack >= (int32_t)(c->rto / 4) &&
            (int32_t)NOW_MS() - (int32_t)(u->tick_timeout - c->rto / 4) >= 0) {
            data[0] = FFRDP_FRAME_TYPE_QUERY;
            if (ffrdp->lowlevel->send(ffrdp->lowlevel, c->peer_addr, data, 1) != 0) FFRDP_CNT(c, counter_send_err);
            c->flags |= FLAG_QUERY_PROBE;
            c->tick_send_query = NOW_MS();
            FFRDP_CNT(c, counter_send_query);
            FFRDP_CNT(c, counter_tailprobe);
        }
    }

    {   // count what is really on the wire, fresh every pass, so no counter can drift or leak
        FFRDP_FRAME_NODE *q;
        for (c->inflight_used = 0, q = c->send_list_head; q; q = q->next) if (q->flags & FLAG_FIRST_SEND) c->inflight_used++;
    }
    // 诊断（§26.103）：发送表的 head **已经越过**对端的 una ✗ —— 说明"洞那一帧"被误当成已确认删掉了，
    // 表里只剩洞之上的帧，重传它们永远填不上洞（cwnd 钉在 1 + 快重传自锁的候选根因 ✓）。
    // 只统计"已经发出去过"的 head（FLAG_FIRST_SEND 置位 = 首发送过 ✓），否则刚入表的帧必然高于 una，属正常 ✓。
    if (c->send_list_head && (c->send_list_head->flags & FLAG_FIRST_SEND) &&
        seq_distance(GET_FRAME_SEQ(c->send_list_head), c->ack_una) > 0) FFRDP_CNT(c, counter_head_ahead);
#if FFRDP_PACE_ENABLE
    ffrdp_pace_refill(c, NOW_MS());   // C1：先把这一圈的令牌补上（ms 粒度 ✓）
#endif
    s0 = c->counter_send_1sttime + c->counter_resend_rto + c->counter_resend_fast;
    // ⚠️ 这个 `i < cwnd` 边界**别动**（试过"越过 cwnd 修重传"，lo 上 −7~10% 吞吐、waste 翻 3~4 倍，
    //    见文件头 ①-42 那段留档 ✓）：表头就是洞，洞后面的重传是纯重复数据 ✓。
    for (i = 0, p = c->send_list_head; i < (int32_t)c->cwnd && p; i++, p = p->next) {
        if (!(p->flags & FLAG_FIRST_SEND)) { // first send
#if FFRDP_PACE_ENABLE
            // C1：令牌不够 ⇒ 这一帧这圈不发，并记下"补令牌"那一刻（1 ms 后）✓。
            // **不 break**：后面"到点该重传"的帧不吃令牌，必须照走（与 swnd0 那条同理 ✓）。
            // pace_fps == 0（RTT 太小 / 未采样）= 不整形 ⇒ 这条判据整条不生效 ✓
            if (c->pace_fps && c->pace_tok16 < 16) {
                if (c->pace_due == 0) c->pace_due = NOW_MS() + FFRDP_PACE_MS;
                FFRDP_CNT(c, counter_pace_wait);
                continue;
            }
#endif
            // 顺序：先看信用、再看在途闸门。反过来的话，在途一满就 break，谁都到不了下面那条"要窗口"
            // —— 而"没有信用"正是最需要问对端一次的时候（真机上 counter_send_query 恒为 0 就是这么来的）。
            if (c->swnd > 0) {
                // frames are first sent in sequence order, so everything already sent sits in front of this
                // one and breaking here skips nothing that needed attention. retransmits are untouched: this
                // only stops adding new frames to a wire that already holds FFRDP_INFLIGHT_MAX of them.
                if (c->inflight_used >= MAX(c->inflight_max ? c->inflight_max : FFRDP_INFLIGHT_MAX, (uint32_t)FFRDP_INFLIGHT_MIN)) {
                    c->counter_capblock++;
                    break;
                }
                if (ffrdp_send_data_frame(ffrdp, c, p) != 0) {
#if FFRDP_SENDFAIL_CUTS_CWND
                    ffrdp_congestion_control(c, CEVENT_SEND_FAILED);
#else
                    // ✗→✓（§26.105）本地 sendto 被内核拒**不是拥塞证据**。
                    // `lowlevel-udp.c` 的 send 只在 `sendto` 没吃掉整帧时返回 -1，而它最常见的成因是
                    // **非阻塞 socket 的发送缓冲暂时满了（EWOULDBLOCK）**—— 时间尺度是微秒~毫秒 ✗。
                    // 旧写法把它当 RTO 处理（cwnd=1、ssthresh 砸到地板）：6 客户端压满本机时一秒能触发几次 ⇒
                    // 窗口钉在 1 ⇒ 直到攒回若干个 ACK 才动 ⇒ 出现 2 s 空档 ⇒ **应用层判 lost ⇒ 重连**
                    // （1018-163155：13 条连接 / 6 客户端；服务端 conn 6 `cwnd back: 180 (rto/fast) + 4199 (send fail)`，
                    //  `ssthresh floor hits 366`，`cwnd 1` ✗）。
                    // 正确做法与 TCP 遇 ENOBUFS 一致：这一帧**还没首发**（FLAG_FIRST_SEND 未置）⇒ 下一圈原样再发 ✓；
                    // 只记账，不碰窗口 ✓。真丢了由 RTO 那条路负责，那才有"没收到 ack"的证据 ✓。
                    FFRDP_CNT(c, counter_cwnd_sfail);
#endif
                    break;
                }
                p->tick_1sts    = p->tick_send = NOW_MS();
                p->ticket       = c->counter_ack_recv;   // 诊断用：从这一刻起，ack 有没有来过
                p->tick_timeout = p->tick_send + c->rto;
                p->flags       |= FLAG_FIRST_SEND; if (c->tx_unsent) c->tx_unsent--;   // ①-41：上过线了 ⇒ 还回未发出额度 ✓
                c->swnd--; c->counter_send_1sttime++; c->inflight_used++;
#if FFRDP_PACE_ENABLE
                if (c->pace_tok16 >= 16) c->pace_tok16 -= 16;   // C1：真发出去才扣令牌 ✓
#endif
            } else {
                c->counter_swnd0++;                        // 有帧等着首发、却没信用（这一项按帧计）
                if (c->tick_send_query == 0 || (int32_t)NOW_MS() - (int32_t)c->tick_send_query > FFRDP_QUERY_CYCLE) { // query remote receive window size
                    data[0] = FFRDP_FRAME_TYPE_QUERY; if (ffrdp->lowlevel->send(ffrdp->lowlevel, c->peer_addr, data, 1) != 0) FFRDP_CNT(c, counter_send_err);
                    c->tick_send_query = NOW_MS(); FFRDP_CNT(c, counter_send_query);
                    break;
                }
                // 没信用、又还没到该问的时候：**不 break** —— 这个节点只是发不出去，后面那些"到点该重传"
                // 的帧不吃信用（重传那一段不碰 swnd），必须照走（原逻辑如此，别在这里提前收口）。
            }
        } else if ((p->flags & FLAG_FIRST_SEND) && ((int32_t)NOW_MS() - (int32_t)p->tick_timeout > 0 || (p->flags & FLAG_FAST_RESEND))) { // resend
            if (!(p->flags & FLAG_FAST_RESEND)) {   // 只统计真正的 RTO 超时，快速重传不算
                uint32_t toage = NOW_MS() - p->tick_1sts;
                if (toage > c->counter_to_age_max) c->counter_to_age_max = toage;
                if (p->ticket == c->counter_ack_recv) FFRDP_CNT(c, counter_to_noack); else FFRDP_CNT(c, counter_to_hadack);
                // 这里原来按"una 已越过这一帧"采尾部 —— 真机四份日志里 rtt_tail 恒为 0 ⇒ 那个条件几乎
                // 不成立（帧被确认时就从表里移除，"una 越过它"与"它超时"很难同时发生）。采样改到确认
                // 到达的那一刻，见下面那个"被重传过的帧"的兄弟块。
            }
            // 只对真 RTO 分辨，快速重传保持原样（它不吃这一刀 ✓）。
            {
                int rto_real = !(p->flags & FLAG_FAST_RESEND);
                int pseudo   = rto_real && (p->ticket != c->counter_ack_recv);
                ffrdp_congestion_control(c, pseudo ? CEVENT_ACK_TIMEOUT_MISSED : CEVENT_ACK_TIMEOUT);
            }
            if (ffrdp_send_data_frame(ffrdp, c, p) != 0) break;
            p->flags |= FLAG_RESENT;   // 无论 RTO 还是快速，重传过的帧就不再做 RTT 采样了（§26.107）✓
            if (!(p->flags & FLAG_FAST_RESEND)) {
                if (p == c->send_list_head) FFRDP_CNT(c, counter_rto_head); else FFRDP_CNT(c, counter_rto_chain);
                // was this frame already reported as received (una passed it) when the timer fired?
                // stale: we were told, but had not removed it -> the retransmit is pure waste.
                if (seq_distance(GET_FRAME_SEQ(p), (uint32_t)c->ack_una) < 0) FFRDP_CNT(c, counter_rto_stale);
                else FFRDP_CNT(c, counter_rto_behind);
                {   // ①-28 A（只读 ✓）：这一帧距 una 多远（= ack 的 24 位位图能不能盖到它 ✓）
                    int32_t d2 = seq_distance(GET_FRAME_SEQ(p), (uint32_t)c->ack_una);
                    if      (d2 <= 7)  FFRDP_CNT(c, counter_to_d0_7);
                    else if (d2 <= 15) FFRDP_CNT(c, counter_to_d8_15);
                    else if (d2 <= 23) FFRDP_CNT(c, counter_to_d16_23);
                    else if (d2 <= 63) FFRDP_CNT(c, counter_to_d24_63);
                    else               FFRDP_CNT(c, counter_to_dM);
                    if (d2 > (int32_t)c->counter_to_dmax) c->counter_to_dmax = (uint32_t)d2;
                }
                if (c->rto == FFRDP_MAX_RTO) {
                    p->tick_send = NOW_MS();
                    p->flags    &=~FLAG_TIMEOUT_RESEND;
                } else p->flags |= FLAG_TIMEOUT_RESEND;
                if ((int32_t)NOW_MS() - (int32_t)c->tick_rto_backoff >= (int32_t)c->rto) { // rto backs off once per round, not once per frame
                    c->tick_rto_backoff = NOW_MS();
                    if (c->rto == FFRDP_MAX_RTO) FFRDP_CNT(c, counter_reach_maxrto);
                    c->rto += c->rto / 2;
                    c->rto  = MIN(c->rto, FFRDP_MAX_RTO);
                    FFRDP_CNT(c, counter_rto_backoff);
                }
                c->counter_resend_rto++;
                c->win_need++;
                got_timeout = 1;
                // 已经真的超时并重传了 ⇒ 上一轮的探针结果不再有用（探针本身丢了也会走到这里）✓
                c->flags &= ~FLAG_QUERY_PROBE;
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
    {   // ①-41e（无队列版的空转修复 ✓）：这一圈"有活却被闸门挡住、一帧没发" ⇒ tx_stalled=1 ⇒
        uint32_t d = (c->counter_send_1sttime + c->counter_resend_rto + c->counter_resend_fast) - s0;
        // next_due 别再急着要"现在" ✓。判据：d==0（没发 ✓）且 p 非空（循环被闸门**打断**，
        // 不是表走完了 ✓ —— 表走完是没活，那不叫被挡 ✓）。有进展（d>0）⇒ 解除 ✓。
        if (d == 0 && p) c->tx_stalled = 1;
        else if (d > 0)  c->tx_stalled = 0;
    }
    if (got_timeout) FFRDP_CNT(c, counter_timeout_evt);

    // 这里原来是"等 socket 可读 + 把所有待收的包一次收完 + 处理收到的 ack"。收包搬走了：lowlevel 一圈把
    // 当前就绪的包都回调上来（一帧一次），帧的解析在 ffrdp_on_frame、ack 的应用在 ffrdp_apply_acks 里；
    // 而应答本身由下面这一句在这一段结束时统一发出去 —— 位置与旧版一致（发送段之后、速率/探测之前）。
    if (c->flags & FLAG_ACK_DUE) { c->flags &= ~FLAG_ACK_DUE; ffrdp_recvdata_and_sendack(ffrdp, c); }  // 只读（④-14）

    {   // delay probe: take half the in-flight away for a moment and see whether the round trip follows.
        // 延迟降了而确认速率没掉 ⇒ 队列是我们的，保留更小的窗口；延迟没降 ⇒ 延迟属于链路，窗口原样还回去（丢包链路过不了第一关，保持原样）。
        uint32_t now = NOW_MS();
        FFRDP_FRAME_NODE *q;
        if (!c->probe_gap) c->probe_gap = FFRDP_PROBE_GAP_MS;
        // the probe now drives the real brake (frames on the wire), not the peer's credit: the credit is
        // handed out again on every ack, so limiting it never bound anything and the experiment could not
        // change the round trip even in principle. without a proven cap the ceiling stays at its default.
        c->inflight_max = c->probe_state ? c->probe_cap
                            : (c->cap_kept ? c->cap_kept : FFRDP_INFLIGHT_MAX);
        c->cap = c->inflight_max;
        if ((int32_t)now - (int32_t)c->rate_tick >= 1000) {
            uint32_t resend = c->counter_resend_rto + c->counter_resend_fast;
            uint32_t firsts = c->counter_send_1sttime;
            c->rate_now     = c->counter_delivered - c->rate_del_ref;
            // 每秒结算一次"这一秒健不健康"：waste 与"没有队列"都要满足 ✓
            {
                uint32_t idle_now = (c->rtt_min && c->rtts != (uint32_t)-1 &&
                                     c->rtts < c->rtt_min + FFRDP_PROBE_IDLE_SLACK(c->rtt_min)) ? 1u : 0u;
                if (c->probe_waste < FFRDP_PROBE_HEALTHY_PCT && idle_now) c->giveback_idle++;
                else                                                      c->giveback_idle = 0;
            }
            c->rate_tick    = now;
            if (c->rtt_tail) c->rtt_tail -= c->rtt_tail / 16;   // 约 16 秒忘掉旧的尾部
            c->rate_del_ref = c->counter_delivered;
            // 触发要整整一秒的证据：三十帧里一次重传 = 3%，那是采样噪声不是浪费；计数还可能被 dump 清零，倒退就当没有证据。
            c->probe_waste = (resend >= c->probe_resend_seen && firsts >= c->probe_first_seen)
                               ? (resend - c->probe_resend_seen) * 1000 / MAX(firsts - c->probe_first_seen, 1)
                               : 0;
            c->probe_resend_seen = resend;
            c->probe_first_seen  = firsts;
            // 见过的最好一秒（升就立刻记，降就每秒衰减 1/16，约 16 秒忘掉旧高水位）。keep 的速率参考要用它：
            // 只跟紧邻的一秒比，第一刀之后参考自己也掉，于是每一刀都通过，闸门被一路 keep 到地板。
            // ✗→✓（§26.106）这里原来写的是 /4（每秒 −25%，约 4 秒就把高水位忘干净了 ✗），与上面那句
            // "每秒衰减 1/16，约 16 秒"矛盾。后果不只是"参考偏低"，而是整条自愈闭环失效：
            //   · keep 的 rate_kept 拿 rate_best 当参考 ⇒ 高水位紧跟当前值 ⇒ 砍多少都"速率没掉" ⇒ 每一刀都 keep；
            //   · 下面那段"事后修复/giveback"（rate_now < rate_best × 85% 就 +25% 还回去）同理恒不成立 ✗。
            // 实测（1018-172440 不限速）：上限被棘轮到 4 帧，`cap-block 142567 ticks`、吞吐被自己卡死 ✗。
            // 改回 1/16 后，高水位能记约 16 秒 ⇒ 小窗口真卡住速率时 rate_kept 会变假、giveback 会开始还 ✓。
            if (c->rate_now >= c->rate_best) c->rate_best = c->rate_now;
            else                             c->rate_best -= c->rate_best / 16;
            // 事后修复：被 keep 的闸门把确认速率拖到最好值的某个比例以下，就按秒翻倍还回去。
            if (c->cap_kept && c->cap_kept < FFRDP_INFLIGHT_CEIL && c->rate_best &&
                c->rate_now * 100 < c->rate_best * FFRDP_PROBE_RATE_KEEP_PCT &&
                (int32_t)NOW_MS() - (int32_t)c->last_giveback >= (int32_t)FFRDP_PROBE_GAP_MS) {
                // 原来这里是 *2：从被证明的 4 翻到 32 只要 3 秒 ✗ ⇒ 一步就能追上下探（探测每 10 秒才问一次 ✗），
                // 于是"刚降下来又被抬回去"。改成 +25%：4→5→6→8→10→13→16→20→25→32 约十秒 = 一个探测周期 ✓
                // ⚠️ 这里的上限必须是 CEIL 而不是 MAX：被 B2 升到 64 以上的 cap_kept 一旦 ≥ MAX，
                //    原来那条 `cap_kept < FFRDP_INFLIGHT_MAX` 就恒假 ⇒ 事后修复与 giveback 双双失效 ✗
                c->cap_kept = MIN(c->cap_kept + MAX(c->cap_kept / 4, 1), FFRDP_INFLIGHT_CEIL);
                c->last_giveback = NOW_MS();   // 记时刻 ⇒ 两处共用同一个"还的预算" ✓
                FFRDP_CNT(c, counter_giveback);
            }
            // ── B2：闸门能升（`ffrdp-optimization.md` §8 的 #4）────────────────────
            // 上面几条往上走的路都要求 `cap_kept` 非零（= 之前至少赢过一次）⇒ "从没赢过"的连接永远停在
            // 默认 64 帧。而在**高 BDP 且不排队**的链路上，64 帧就是硬天花板：仿真里干净 200 ms 链路
            // 把上限从 64 抬到 128，交付量 +102%、重传率反降 ⇒ 这条缺口是真的会限吞吐 ✓。
            // 升的三条前提必须**都由测量给出**，缺一条就清零重攒：
            //   · 顶死：在途刚好就是上限（否则升了也用不上）✓
            //   · 没队列：rtts 贴着 rtt_min（与 giveback 同一个判据、同一个 slack ✓）
            //   · 没浪费：probe_waste 低于健康线（有浪费说明是链路在丢，不是我们的窗口不够 ✗）
            // 再加两道频率闸：探测刚 revert 过一个周期内不升（它刚说过"小窗口没赚"✗）；
            // 每 FFRDP_PROBE_GAP_MS 最多升一次（升得比探测问得还快，就会自己打自己 ✓）。
            {
                uint32_t out = 0, next;
                // ⚠️ 算术前提（2026-10 定档之后值得写下来 ✓）：要升，必须**同时**满足"顶死"
                //    out >= inflight_max 与"窗口吃得下" swnd >= next = 1.25 × inflight_max；
                //    而顶死时 swnd = 窗口 − 在途 ⇒ 只有 **窗口 ≥ 2.25 × inflight_max** 才可能升 ✓。
                //    所以默认档（inflight_max = 窗口 = FFRDP_INFLIGHT_MAX = 128）下这段永远进不去 ——
                //    不是坏了，是"已经没有可升的空间" ✓（`tools/sim/t_cap.c` 的 A/E 两段把这件事钉住 ✓）。
                for (q = c->send_list_head; q; q = q->next) if (q->flags & FLAG_FIRST_SEND) out++;
                next = c->inflight_max + MAX(c->inflight_max / 4, 1);
                if (next > FFRDP_INFLIGHT_CEIL) next = FFRDP_INFLIGHT_CEIL;
                if (c->probe_state == 0 && c->inflight_max < FFRDP_INFLIGHT_CEIL && out >= c->inflight_max &&
                    c->rtt_min && c->rtts != (uint32_t)-1 &&
                    c->rtts <= c->rtt_min + FFRDP_PROBE_IDLE_SLACK(c->rtt_min) &&
                    c->probe_waste < FFRDP_PROBE_HEALTHY_PCT &&
                    c->swnd >= next &&                     // 对端窗口吃不下 ⇒ 升了也发不出去 ✗
                    (int32_t)now - (int32_t)c->tick_cap_revert >= (int32_t)FFRDP_PROBE_GAP_MS &&
                    (int32_t)now - (int32_t)c->tick_cap_rise   >= (int32_t)FFRDP_PROBE_GAP_MS) {
                    if (++c->cap_idle >= FFRDP_CAP_RISE_IDLE_S) {
                        c->cap_kept      = next;
                        c->cap_idle      = 0;
                        c->tick_cap_rise = now;
                        FFRDP_CNT(c, counter_cap_rise);
                    }
                } else {
                    c->cap_idle = 0;
                }
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
                // 值得做实验的是"链路正压着一个队列"，而 rtt_min 让它直接可见（典型 RTT 远高于地板就是队列）；重传计数只是给"地板已经很低"的链路留的第二条入口。
                cap_want = MAX(out / 2, (uint32_t)FFRDP_INFLIGHT_MIN);
                // 同一个"链路在压队列吗"用了两把尺子：比值（rtts/rtt_min）适合**开始**实验，不适合**结束**它 ——
                // giveback 改用毫秒尺度（队列真的没了），与它要撤销的 keep 判据同尺度。
                holding = (c->rtt_min && c->rtts != (uint32_t)-1 &&
                           c->rtts > FFRDP_PROBE_QUEUE_RATIO * c->rtt_min) ? 1u : 0u;
                // ①-46：比值判据在"地板 1~4 ms、多余延迟其实是别人排的"链路上恒真（1018-202512：cap 被棘轮
                // 到 5~34 帧）。改成物理量：多余延迟必须**能被我们自己的窗口解释**，且这个量本身够深 ✓。
                int window_full = 1;
#if FFRDP_PROBE_NEEDS_FULL_WINDOW
                window_full = (out * 4u >= c->inflight_max * 3u) ? 1 : 0;   // 在途没顶到 cap 的 3/4 ⇒ 砍半是误伤 ✗
#endif
#if FFRDP_PROBE_QUEUE_BY_MODEL
                {   uint32_t q_ours = ffrdp_probe_q_ours_ms(c, out);
                    if (q_ours > 0) {   // 速率还没采到 ⇒ 退回老的比值判据 ✓
                        uint32_t excess = (c->rtts != (uint32_t)-1 && c->rtts > c->rtt_min) ? c->rtts - c->rtt_min : 0;
                        holding = (holding && q_ours >= FFRDP_PROBE_QUEUE_MIN_MS &&
                                   excess <= 2u * q_ours + FFRDP_PROBE_RTT_DROP_MS) ? 1u : 0u;
                    }
                }
#endif
                // ①-46：老判据"想探"、新判据"挡住"的次数 —— 真机上这个数很大而 cap 不再往下掉，就说明改对了 ✓
                if ((c->rtt_min && c->rtts != (uint32_t)-1 &&
                     c->rtts > FFRDP_PROBE_QUEUE_RATIO * c->rtt_min) &&
                    (!window_full || holding == 0) && cap_want < c->inflight_max)
                    FFRDP_CNT(c, counter_probe_block);
                idle    = (c->rtt_min && c->rtts != (uint32_t)-1 &&
                           c->rtts < c->rtt_min + FFRDP_PROBE_IDLE_SLACK(c->rtt_min)) ? 1u : 0u;
                // ①-39：**内存视角的触发** ✓ —— 上限比"这一圈真正在线上"高一倍以上就值得试 ✓
                uint32_t mem_want = (FFRDP_MEMPROBE_ENABLE && c->rate_best > 0 && out >= FFRDP_MEMPROBE_MIN_OUT &&
                                     cap_want < c->inflight_max) ? 1u : 0u;
                if (FFRDP_PROBE_ENABLE && out > FFRDP_INFLIGHT_MIN && cap_want < c->inflight_max &&
                    window_full && (holding || waste >= FFRDP_PROBE_WASTE_PM || mem_want)) {
                    uint32_t rtt = (c->rtts == (uint32_t)-1 || c->rtts < FFRDP_MIN_RTO) ? FFRDP_MIN_RTO : c->rtts;
                    c->probe_state   = 1;
                    c->probe_tick    = now;
                    c->probe_hold    = 2 * rtt + FFRDP_PROBE_HOLD_MS;
                    c->probe_cap     = cap_want;
                    // 参考用**典型** RTT，而实验跟踪同一个估计 ⇒ 相减的两个数来自同一个滤波器，2ms 的差才读得出来；rttm 只作"还没有典型值"时的兜底。
                    c->probe_rtt_ref = c->rtts != (uint32_t)-1 ? c->rtts : c->rttm;
                    c->probe_rate_ref= c->rate_now;
                    c->probe_best    = 0xFFFFFFFF;
                    c->probe_del_ref = c->counter_delivered;
                    c->probe_res_ref = c->counter_resend_rto + c->counter_resend_fast;
                    c->probe_waste_ref = c->probe_waste;
                    c->cap           = c->probe_cap;
                    FFRDP_CNT(c, counter_probe);
                    if (mem_want && !holding && waste < FFRDP_PROBE_WASTE_PM) FFRDP_CNT(c, counter_probe_mem);
                    // ⚠️ 探测**进行中**时，`probe_next` 必须跟着推到未来 ✗：刷新它的两处都在"探测结束"
                    // （keep → now + probe_hold ✓ / revert → now + probe_gap ✓），而这段时间外层
                    // `if (c->probe_state == 0)` 又把整段跳过 ⇒ 它会一直停在启动那一刻（过去）✗
                    c->probe_next = now + c->probe_hold;
                } else if (waste < FFRDP_PROBE_HEALTHY_PCT && c->cap_kept && idle &&
                           c->giveback_idle >= FFRDP_PROBE_GIVEBACK_IDLE_S &&
                           (int32_t)now - (int32_t)c->last_giveback >= (int32_t)FFRDP_PROBE_GAP_MS) {
                    // 不再浪费、且队列真的没了（用上面那个**绝对**判据，不是贴着阈值的比值）⇒ 还一点窗口，免得被旧路径上证明过的上限永久压住；
                    // giveback 会把下次探测推远一个周期，所以不能在还有队列时还。
                    c->cap_kept   = MIN(c->cap_kept + MAX(c->cap_kept / 4, 1), FFRDP_INFLIGHT_MAX);
                    c->cap        = c->cap_kept;
                    c->probe_next = now + FFRDP_PROBE_GAP_MS;
                    c->last_giveback = now;
                    c->giveback_idle = 0;      // 还过一次就重新攒 ✓
                    FFRDP_CNT(c, counter_giveback);
                } else {
                    // 队列判据不满足 ⇒ 等满一个退避再看。更早看试过、被否：会把第一次探测挪进连接爬升期，两个速率采样都在涨 ⇒ 速率守卫误判"速率掉了"。
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
                // keep 判据：典型 RTT 至少降 FFRDP_PROBE_RTT_DROP_MS（绝对差）。缺参考 = 还没有典型值 ⇒ 什么都没证明，**绝不在缺测量时保留小窗口**（probe_best 的 0xFFFFFFFF 要先挡掉）。
                uint32_t rtt_down  = c->probe_best != 0xFFFFFFFF &&
                                     c->probe_rtt_ref > c->probe_best &&
                                     c->probe_rtt_ref - c->probe_best >= FFRDP_PROBE_RTT_DROP_MS;
                // 参考取"探测开始前一秒"与"见过的最好一秒"里的较大者。只跟紧邻的前一秒比是不够的：
                // 真机实测里第一刀砍下去之后参考本身跟着掉，于是每一刀都"速率没降"，闸门一路被 keep 到地板。
                uint32_t rate_ref  = MAX(c->probe_rate_ref, c->rate_best);
                uint32_t rate_kept = rate_held * 100 >= rate_ref * FFRDP_PROBE_RATE_KEEP_PCT;
                // 第二条证据：**重传强度**没变差。慢 AP 上真正的收益就在这里，而且是直接量到的 ✓
                uint32_t resend_during = (c->counter_resend_rto + c->counter_resend_fast) - c->probe_res_ref;
                uint32_t del_during    = c->counter_delivered - c->probe_del_ref;
                uint32_t waste_during  = del_during ? (uint32_t)(1000.0 * resend_during / del_during) : 0;
                uint32_t waste_down    = waste_during + waste_during / 4 < c->probe_waste_ref;   // 低 20% 以上才认 ✓
                // 2026-10-15 定稿（真机三轮 8/11/20ms ✓，见 notes §26.18）：保留更小窗口**不再要求"看到 RTT 下降"** ✗。
                // ✗→✓（§26.105）但那条例外只对"链路本身是瓶颈"成立。**应用/CPU 才是瓶颈时**（本机 6 客户端就是），
                // 把在途砍半交付速率照样不掉 ⇒ 每一刀都 rate_kept ⇒ keep ⇒ 上限一路棘轮到地板（实测 7 / 15 帧）✗，
                // 窗口小了却什么都没买来：它只会让单条连接的带宽时延积装不下（10~14 ms 链路上尤其 ✗）。
                // 所以保留必须**有凭证**：① RTT 真的跟着降了（说明队列确实是我们的）且速率没掉 ✓；或 ② 重传强度真的降了
                // （慢 AP 那条路径 ✓，原意保留）。两条都不成立就 revert，上限留在原处 ✓（不是"降到地板再爬回来"）。
                // FFRDP_PROBE_KEEP_NEEDS_EVIDENCE=0 可退回旧口径用于 A/B ✓。
#if FFRDP_PROBE_KEEP_NEEDS_EVIDENCE
                // 证据②（重传强度降了）还要过两道统计关：窗口太短或降幅小于噪声，就不算证据 ✗。
                // 500 ms 窗口里每千帧 25% 的重传，其统计噪声约 19 pm ⇒ 25 pm 只是"不像掷骰子"的底线 ✓。
                uint32_t waste_drop = (c->probe_waste_ref > waste_during) ? c->probe_waste_ref - waste_during : 0;
                // 两条候选都必须带 rate_kept（"没伤到速率"）✓：它原来只挂在①上，②（重传强度降了）可以**独立**
                // 保留更小的窗口 ⇒ 于是出现了"用吞吐换重传"的坏交易：上限砍到 4 帧、重传少几个点，但
                // `cap-block` 十几万次、管子被自己掐住 ✗（1018-172440 服务端那条）。
                // 慢 AP 那条路不受影响：那里本来就是"速率没掉、只是重传降了"，rate_kept 一样成立 ✓。
                if (rate_kept &&
                    (rtt_down ||
                     (waste_down && del_during >= FFRDP_PROBE_EVID_MIN_DEL && waste_drop >= FFRDP_PROBE_EVID_DROP_PM))) {
#else
                if (rate_kept || waste_down) {
#endif
                    c->cap_kept  = c->probe_cap;
                    c->probe_gap = FFRDP_PROBE_GAP_MS;
                    // 刚证明还有得赚 ⇒ 不等满退避，现在继续往下走（自我限制：第一次空回答就再退避翻倍）⇒ 没人量过的链路几秒收敛，也不用事先挑值。
                    c->probe_next = now + c->probe_hold;
                    FFRDP_CNT(c, counter_probe_keep);
                } else {
                    // the delay did not follow, or it did but the confirming rate fell with it: either way
                    // the smaller window bought nothing, so go back to what was proven and wait longer.
                    if (rtt_down) FFRDP_CNT(c, counter_probe_narrow);
                    c->probe_gap  = MIN(c->probe_gap * 2, (uint32_t)FFRDP_PROBE_GAP_MAX_MS);
                    c->probe_next = now + c->probe_gap;
                    c->tick_cap_revert = now;   // B2：它刚说过"小窗口没赚" ⇒ 一个周期内别升 ✗
                    c->cap_idle = 0;
                    FFRDP_CNT(c, counter_probe_revert);
                }
                c->probe_state = 0;
                c->cap         = c->cap_kept;
            }
        }
    }
}

// lowlevel 的入口：它的工作线程在这里回到协议里。同一时刻只会有一路，锁保护整个回调，协议自己的状态
// 因此仍然只被一个线程碰。
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
// code 只对 FFRDP_DISCONN 有意义（FFRDP_DISCONN_xxx）：应用据此把"对端主动关"与"超时判死"分开 ✓。
static long ffrdp_emit_ex(FFRDPCONTEXT *ffrdp, int type, int conn, uint64_t addr, void *buf, int len, int code)
{
    FFRDP_EVENT ev;
    if (!ffrdp || !ffrdp->app_cb) return FFRDP_NOT_CONSUMED;
    ev.type = type;
    ev.conn = conn;
    ev.code = code;
    ev.addr = addr;
    ev.buf  = buf;
    ev.len  = len;
    return ffrdp->app_cb(ffrdp->app_cbctx, &ev);
}

static long ffrdp_emit(FFRDPCONTEXT *ffrdp, int type, int conn, uint64_t addr, void *buf, int len)
{
    return ffrdp_emit_ex(ffrdp, type, conn, addr, buf, len, 0);
}

// 一条连接的句柄（= 代号*FFRDP_MAXCONN + 槽号，分配时就定好了 ✓）。c 为空时返回 -1。
static int ffrdp_conn_handle(FFRDPCONTEXT *ffrdp, FFRDPCONN *c)
{
    (void)ffrdp;
    return c ? (int)c->handle : -1;
}

// 排查用（只读 ✓）：把 64 位地址打成 "ip:port"，与 dump 块头同一套取法。
// 用途：FIN / keepalive 判死这类"到底谁关的、关的是谁"的问题，日志里没有地址就只能猜 ✗。
static const char *ffrdp_addr_text(uint64_t addr, char *buf, int len)
{
    uint32_t ip = 0;
    uint16_t port = 0;
    if (!addr) { snprintf(buf, (size_t)len, "(none)"); return buf; }
    ffrdp_addr_split(addr, &ip, &port);
    snprintf(buf, (size_t)len, "%u.%u.%u.%u:%u",
             (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF, (unsigned)port);
    return buf;
}

// 发一个握手帧：data[0] = 35，data[1] = 子类型。照 QUERY 的写法：小栈缓冲 + lowlevel->send()
//（不走 frame_node_new ⇒ 不参与数据 ARQ、不带 FEC 尾巴 ✓）。调用时持锁。
static int ffrdp_hs_send(FFRDPCONTEXT *ffrdp, FFRDPCONN *c, uint8_t sub)
{
    uint8_t data[2];
    if (!ffrdp->lowlevel || !c || !c->peer_addr) return -1;
    data[0] = FFRDP_FRAME_TYPE_HANDSHAKE;
    data[1] = sub;
    // 排查"谁把连接关了"：FIN 只会由 ffrdp_close（我方主动关）或 ffrdp_hs_rx（收到对端 FIN 后回一个）发出，
    // 两处都有日志 ⇒ 一条连接为什么断，看这三行就够了 ✓（其余握手帧不打，避免刷屏）。
    if (sub == FFRDP_HS_FIN) {
        char ab[48];
        printf("[hs] FIN tx -> %s  handle=%d state=%d t=%u ms\n",
               ffrdp_addr_text(c->peer_addr, ab, sizeof(ab)), (int)c->handle, c->hs_state, (unsigned)NOW_MS());
    }
    c->hs_tick = NOW_MS();   // 成败都记：失败也要按 FFRDP_HS_RETRY_MS 的节奏再试 ✓
    if (ffrdp->lowlevel->send(ffrdp->lowlevel, c->peer_addr, data, 2) != 0) {
        FFRDP_CNT(c, counter_send_err);
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
            c->tick_last_recv = NOW_MS(); // 半开也算听到了 ✓
            c->hs_start = NOW_MS();
            ffrdp_hs_send(ffrdp, c, FFRDP_HS_SYN_ACK);
            return;
        }
        if (sub == FFRDP_HS_SYN && c->hs_state == HS_SYN_RCVD) { // SYN 丢了 ⇒ 重发 SYN-ACK
            ffrdp_hs_send(ffrdp, c, FFRDP_HS_SYN_ACK);
            return;
        }
        if (sub == FFRDP_HS_ACK && c->hs_state == HS_SYN_RCVD) { // 握手完成
            c->hs_state = HS_UP;
            c->tick_last_recv = NOW_MS(); // keepalive 计时从这里起 ✓（刚听到对端 ✓）
            c->flags   |= FLAG_CONNECTED;
            ffrdp_emit(ffrdp, FFRDP_ACCEPT, ffrdp_conn_handle(ffrdp, c), addr, NULL, 0);
            return;
        }
        if (sub == FFRDP_HS_FIN) { // 对端要关
            // 四次挥手：我方还没发过 FIN ⇒ **回一个** ✓（否则对端永远不知道我们收到了 ✗）；
            // 然后本地收摊 ✓（不做半关 ✓）。
            {   char ab[48];
                printf("[hs] FIN rx (server) from %s  handle=%d state=%d t=%u ms\n",
                       ffrdp_addr_text(addr, ab, sizeof(ab)), (int)c->handle, c->hs_state, (unsigned)NOW_MS());
            }
            if (c->hs_state != HS_FIN_WAIT) ffrdp_hs_send(ffrdp, c, FFRDP_HS_FIN);
            ffrdp_emit_ex(ffrdp, FFRDP_DISCONN, ffrdp_conn_handle(ffrdp, c), c->peer_addr, NULL, 0, FFRDP_DISCONN_PEER_FIN);
            ffrdp_conn_free(ffrdp, c);
            return;
        }
        return;
    }

    // 客户端：只有主连接，等 SYN-ACK。
    c = ffrdp->table[0];
    if (!c) return;
    // ⚠️ 客户端也要认 FIN ✗✓：对端主动关时，回 FIN 是**两边**都要做的事 ✓。
    if (sub == FFRDP_HS_FIN) {
        // ⚠️ 这一支**不看 addr**（客户端只有主连接）⇒ 任何来源的 FIN 都会被当成"服务器要关"。
        //    排查时这行日志最关键：地址对不上（不是 c->peer_addr）就说明收到的是别人的帧 ✗。
        {   char ab[48], pb[48];
            printf("[hs] FIN rx (client) from %s  expect=%s handle=%d state=%d t=%u ms\n",
                   ffrdp_addr_text(addr, ab, sizeof(ab)), ffrdp_addr_text(c->peer_addr, pb, sizeof(pb)),
                   (int)c->handle, c->hs_state, (unsigned)NOW_MS());
        }
        if (c->hs_state != HS_FIN_WAIT) ffrdp_hs_send(ffrdp, c, FFRDP_HS_FIN);  // 对端先关 ⇒ 回一个 ✓
        ffrdp_emit_ex(ffrdp, FFRDP_DISCONN, ffrdp_conn_handle(ffrdp, c), c->peer_addr, NULL, 0, FFRDP_DISCONN_PEER_FIN);
        // 主连接**不能**从表里摘掉 ✗（表槽永不回收 ✓；send/recv 靠 HS_FAILED 报 CLOSED ✓）
        c->hs_state = HS_FAILED;
        return;
    }
    if (sub == FFRDP_HS_SYN_ACK && c->hs_state == HS_SYN_SENT) {
        c->hs_state = HS_UP;
        c->tick_last_recv = NOW_MS();       // 同上 ✓
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
    ffrdp_now_ms = get_timestamp32_ms(); ffrdp_now_frozen = 1;   // 整圈冻结：下面几十处 NOW_MS() 都读它 ✓
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
        // ⚠️ "听到对端"**不能刷在这里** ✗：这里 `c` 是主连接，真正收数据的是下面按地址解出的 `cc` —— 刷错地方会让服务端的 accept 连接被 idle 误判死。
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
            if (cc && ffrdp->hs_enable) cc->tick_last_recv = NOW_MS();
            if (!cc) {
                FFRDP_CNT(c, counter_rx_foreign);
            } else if (ffrdp->hs_enable && cc->hs_state != HS_UP) {
                FFRDP_CNT(cc, counter_rx_foreign);   // 半开连接上的数据帧：握手没完成 ⇒ 丢
            } else if (!ffrdp->is_server || (cc->flags & FLAG_CONNECTED)) {
                int n = msg->len;
                const uint8_t  *fb = (const uint8_t*)msg->buf;
                FFRDP_FRAME_NODE *node = NULL;
                // ② ack（8~9 字节）/ query（4 字节）这类小控制帧**就地解析** ✓：
                //    它是主路径上数量过半的包（实测 lo：收包 37.1k/s 里 18.3k 是 ack ⇒ 49% ✗），
                //    却要为它 calloc 一个 1500 字节的节点再 memcpy 一遍 ⇒ 纯亏 ✗。
                //    线上格式、语义、计数一律不变 ✓（ack 的分支逐行搬到 ffrdp_on_ack ✓）。
                int inplace = (fb[0] == FFRDP_FRAME_TYPE_ACK || fb[0] == FFRDP_FRAME_TYPE_QUERY);
                if (!inplace) {
                    node = frame_node_new(FFRDP_FRAME_TYPE_FEC2, FFRDP_MAX_RX_MSS);
                    if (!node) ret = -1;
                    else {
                        // 传输层一帧最大 FFRDP_LOWLEVEL_FRAME_MAX，接收节点按**收** MSS 建：夹一下，别越界。
                        if (n > (int)node->size) n = (int)node->size;
                        memcpy(node->data, msg->buf, (size_t)n);
                    }
                }
                if (inplace || node) {
                    FFRDP_RX_BATCH b;
                    b.recv_una  = cc->recv_seq;
                    b.send_una  = cc->send_list_head ? GET_FRAME_SEQ(cc->send_list_head) : 0;
                    b.send_mack = 0; b.got_data = 0; b.got_query = 0;
                    if (inplace) {
                        if (fb[0] == FFRDP_FRAME_TYPE_ACK) ffrdp_on_ack(cc, fb, n, &b);
                        else                               b.got_query = 1;
                    } else if (!ffrdp_on_frame(cc, node, n, &b)) free(node);  // 没被乱序队列接管 ⇒ 归我们
                    ffrdp_apply_acks(cc, &b);
                    // ack 的时机见 FFRDP_ACK_COALESCE（=1 时欠着，留给同一圈的 TIMER 支发一个 ✓）。
                    // ack 的耗时记账在 ffrdp_recvdata_and_sendack 里 ⇒ 两条时机下都能量到 ✓。
                    if (!FFRDP_ACK_COALESCE && (cc->flags & FLAG_ACK_DUE)) {
                        cc->flags &= ~FLAG_ACK_DUE;
                        ffrdp_recvdata_and_sendack(ffrdp, cc);
                    }
                }
            }
        }
    } else if (type == FFRDP_LOWLEVEL_TYPE_TIMER) {
        // 逐连接：别的到期工作都没有、只是欠着对端一个 ack 时，就别跑整段了。整段的开销是给
        // "发送/超时/探测/速率"付的，收到一批帧就付一次，实测把回环吞吐压掉了两成。
        for (int i = 0; i < ffrdp->nconn; i++) {
            FFRDPCONN *cc = ffrdp->table[i];
            uint32_t   d;
            if (!cc) continue;
            if (ffrdp->hs_enable && (cc->hs_state == HS_SYN_SENT || cc->hs_state == HS_SYN_RCVD ||
                                     cc->hs_state == HS_FIN_WAIT)) {
                uint32_t now = NOW_MS();
                if ((int32_t)(now - cc->hs_tick) >= FFRDP_HS_RETRY_MS)
                    ffrdp_hs_send(ffrdp, cc, cc->hs_state == HS_SYN_SENT ? FFRDP_HS_SYN :
                                                cc->hs_state == HS_SYN_RCVD ? FFRDP_HS_SYN_ACK : FFRDP_HS_FIN);
                if ((int32_t)(now - cc->hs_start) >= (int32_t)ffrdp->handshake_ms) {
                    if (ffrdp->is_server) {
                        // 半开连接计时封顶：没人来完成握手就把这条连接放掉（**槽位随即回收** ✓）。
                        ffrdp_emit_ex(ffrdp, FFRDP_DISCONN, ffrdp_conn_handle(ffrdp, cc), cc->peer_addr, NULL, 0, FFRDP_DISCONN_HS_TIMEOUT);
                        ffrdp_conn_free(ffrdp, cc);
                    } else {
                        cc->hs_state = HS_FAILED; // 客户端：此后 send/recv 按"已断"处理
                        ffrdp_emit_ex(ffrdp, FFRDP_DISCONN, ffrdp_conn_handle(ffrdp, cc), cc->peer_addr, NULL, 0, FFRDP_DISCONN_HS_TIMEOUT);
                    }
                    continue;
                }
            }
            d = ffrdp_next_due(ffrdp, cc);
            {   // 只读（①-6）：期限模型这一圈给的是哪一档 ✓ —— 放在分支之前 ⇒ cc 一定有效 ✓
                uint32_t dn = NOW_MS();
                if (!d)                          FFRDP_CNT(cc, counter_fire_d0);
                else if ((int32_t)(d - dn) <= 0) FFRDP_CNT(cc, counter_fire_dnow);
                else if ((int32_t)(d - dn) <= 4) FFRDP_CNT(cc, counter_fire_dnear);
                else                             FFRDP_CNT(cc, counter_fire_dfar);
            }
            if ((cc->flags & FLAG_ACK_DUE) && (!d || (int32_t)(d - NOW_MS()) > 0)) {
                cc->flags &= ~FLAG_ACK_DUE;
                FFRDP_CNT(cc, counter_fire_ackonly);                          // 只读（①-6）
                ffrdp_recvdata_and_sendack(ffrdp, cc);
            } else {
                FFRDP_CNT(cc, counter_fire_tick);                             // 只读（①-6）
                ffrdp_tick_conn(ffrdp, cc);
                // ⚠️ tick 里可能刚刚把这条连接**判死并收摊**（keepalive 超时 ⇒ 表槽清空 + free ✓）
                //    ⇒ 这里必须重新查表，绝不能再碰 cc 这个旧指针 ✗
                if (ffrdp->table[i] != cc) continue;
            }
        }
    }
    ffrdp_arm_timer(ffrdp);
    pthread_cond_broadcast(&ffrdp->cond); // 可能有应用线程正等着空间或数据
out:
    ffrdp_now_frozen = 0;   // 出回调即解冻（下一次真钟读数照旧）✓
    pthread_mutex_unlock(&ffrdp->lock);
    return ret;
}

void ffrdp_flush(void *ctxt, int conn)
{
    FFRDPCONTEXT *ffrdp = (FFRDPCONTEXT*)ctxt;
    int i, n = 0;
    if (!ffrdp) return;
    pthread_mutex_lock(&ffrdp->lock);
    // conn < 0 = 对全部连接生效 ✓；conn >= 0 = **只对那一条** ✓（ffrdp.h 的承诺）。
    // ⚠️ 原来固定取 table[0] ✗：服务端的主连接占 0 号、accept 出来的客户端是 1..N ⇒ 对客户端调
    //    flush 一个字节都刷不出去，只能等 FFRDP_FLUSH_IDLE 的空闲冲刷兜底 ⇒ 多连接时这条 API 形同虚设 ✗。
    for (i = 0; i < ffrdp->nconn; i++) {
        FFRDPCONN *c;
        if (conn >= 0 && i != conn) continue;
        c = ffrdp->table[i];
        if (!c) continue;
        c->flags |= FLAG_FLUSH;
        n++;
    }
    // 工作线程可能正睡在"下一个期限"上：叫醒一次，这些帧才不用等到那时候 ✓
    if (n && ffrdp->lowlevel) ffrdp->lowlevel->timer(ffrdp->lowlevel, NOW_MS());
    pthread_mutex_unlock(&ffrdp->lock);
}

// 逐连接 dump：conn < 0 ⇒ 表里每条打一块 ✓；conn >= 0 ⇒ **只打那一条** ✓（这就是 ffrdp.h 的承诺 ✓）。
#if FFRDP_DIAG
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

// rtts 没采到样时是 (uint32_t)-1（算法内部就这么用）⇒ 直接打会是 4294967295 ✗，这里打 "-"✓。
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
    secs = ((int32_t)NOW_MS() - (int32_t)ffrdp->tick_ffrdp_dump) / 1000;
    secs = secs ? secs : 1;

    // 块头：句柄 + 对端地址 + 握手状态（多连接时没有它分不出哪块是哪条）。
    if (c->peer_addr) {
        ffrdp_addr_split(c->peer_addr, &ip, &port);
        snprintf(addr, sizeof(addr), "peer %u.%u.%u.%u:%u",
                 (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF, (unsigned)port);
    } else snprintf(addr, sizeof(addr), "peer not set (not learned yet)");

    // 从未通信过的槽（服务端的 0 号内嵌主连接）只打一行：每项都是 0，打满只会把真在跑的那条挤没。
    if (c->hs_state == 0 && !(c->flags & FLAG_CONNECTED) &&
        c->send_seq == 0 && c->recv_seq == 0 &&
        c->counter_send_bytes == 0 && c->counter_recv_bytes == 0) {
        printf("\n---- conn %d   %s   idle: never talked about AND ran no lap (only this line) ----\n", handle, addr);
        goto done;
    }

    printf("\n---- conn %d   %s   %s%s ----\n", handle, addr, dump_hs_name(c->hs_state),
           (c->flags & FLAG_CONNECTED) ? " (connected)" : "");
    printf("total_send, total_recv: %.2fMB, %.2fMB\n"    , c->counter_send_bytes / (1024.0 * 1024), c->counter_recv_bytes / (1024.0 * 1024));
    // secs 是距上一次 dump 的秒数：刚起来的第一次 dump 可能不足一秒，别拿 0 当除数。
    printf("averg_send, averg_recv: %.2fKB/s, %.2fKB/s\n", c->counter_send_bytes / (1024.0 * MAX(secs, 1)), c->counter_recv_bytes / (1024.0 * MAX(secs, 1)));
    printf("rttm: %u, rtts: %s, rttd: %u, rto: %u\n", c->rttm, dump_u32(c->rtts, b1, (int)sizeof(b1)), c->rttd, c->rto);
    printf("rtt clamp           : %u samples clamped to 4x rtts + %u ms (a frame queued behind a hole reports the repair time, not a round trip)\n",
           c->counter_rtt_clamp, (unsigned)FFRDP_RTT_SAMPLE_CLAMP_MS);
    // 编译时间戳：比它旧的二进制一眼看出不是刚编的（免得把过期可执行文件当成新的）。
    printf("version & build     : %s probe %d, queue ratio %u, inflight default %u, compiled %s %s\n",
           FFRDP_VERSION, FFRDP_PROBE_ENABLE, (uint32_t)FFRDP_PROBE_QUEUE_RATIO, FFRDP_INFLIGHT_MAX, __DATE__, __TIME__);
    // rttm = 最新一次采样（名字有误导），rtt_min = 区间最低，rtts = 平滑典型延迟，rttd = 抖动；rto 是定时器不是延迟。
    printf("rtt_min, rtts, rttd : %u, %s, %u (min / typical / jitter, ms)\n",
           c->rtt_min, dump_u32(c->rtts, b2, (int)sizeof(b2)), c->rttd);
    // elapsed = 自 init 起；括号里那个是"自上次清零"（clearhistory=1 才推进）⇒ 旧标签 dump interval 会误读。
    printf("send refused before lookup: no-arg/bad-arg %u, bad-handle-or-table %u (①-24; per-connection ones are below)\n",
           ffrdp->stat_ref_noarg, ffrdp->stat_ref_noconn);
    printf("elapsed             : %d s (since init) / %d s (since last clear)\n", ((int32_t)NOW_MS() - (int32_t)ffrdp->tick_ffrdp_init) / 1000, secs);
    // ⚠️ 分母必须与 `total_send/total_recv` **同源**：那两个是 c->counter_*（本连接累计 ✓），而 `elapsed` 是上下文级的 ✗
    //    ⇒ 重连过、或服务端比这条连接先启动时，用 elapsed 当分母会把 MB/s 系统性压低（实测 128 s / 24 s = 5.3 倍 ✗）。
    printf("conn age            : %d s (since this connection was created; the right denominator for total_send/total_recv)\n",
           ((int32_t)NOW_MS() - (int32_t)c->tick_conn_init) / 1000);
    printf("recv_size           : %d\n"  , c->recv_size           );
    printf("flags               : %x\n"  , c->flags               );
    printf("send_seq            : %u\n"  , c->send_seq            );
    printf("recv_seq            : %u\n"  , c->recv_seq            );
    printf("wait_snd            : %u\n"  , c->wait_snd            );
    printf("rmss, smss          : %u, %u\n"    , c->rmss, c->smss);
    printf("swnd, cwnd, ssthresh: %u, %u, %u\n", c->swnd, c->cwnd, c->ssthresh);
#if FFRDP_PACE_ENABLE
    printf("pace (ms bucket)    : tok %u (1/16 frame), burst %d ms, waited %u frames, shaping %u fps%s\n",
           c->pace_tok16, FFRDP_PACE_BURST_MS, c->counter_pace_wait, c->pace_fps,
           c->pace_fps ? "" : " (off: rtts < FFRDP_PACE_MIN_RTT)");
#endif
    // 三个判据一行打完（drain 早退 / cwnd 被乱序误判打回 / 被内核丢包打回）；计数是累计的 ⇒ 看增量。
    printf("ssthresh floor hits : %u (cwnd already at the floor, still recomputed => +1/ACK from here)\n", c->counter_ssth_floor);
    // cwnd 被护栏改写过的那批：RTO/快速重传打回 ✓ + "内核没收下这一包"的次数（§26.105 起**不再改窗口** ✓，
    // 它只是本地发送缓冲暂时满的计数；FFRDP_SENDFAIL_CUTS_CWND=1 时才重新计入打回 ✓）。
    printf("cwnd back           : %u (rto/fast) + %u (sendto refused, local, no cut)\n", c->counter_cwnd_back, c->counter_cwnd_sfail);

    printf("timeout diag        : no-ack-arrived %u, acks-arrived-but-missed %u, max wait %u ms\n",
           c->counter_to_noack, c->counter_to_hadack, c->counter_to_age_max);
    printf("peer wnd raw/clamped: %u, %u\n", c->peer_wnd_raw, c->swnd);
    printf("send starved/blocked: swnd0 %u frames, cap-block %u ticks\n", c->counter_swnd0, c->counter_capblock);
    // 被哪一道闸门拒（配 counter_send_failed 看总数）；conn-state 那几条是没有 c 的出口（①-24）。
    printf("send refused by gate: frame-count %u | conn-state: closing %u, not-connected %u, hs-failed %u, hs-down %u (see counter_send_failed for the total)\n",
           c->counter_refuse_wnd,
           c->counter_ref_closing, c->counter_ref_notconn, c->counter_ref_hsfail, c->counter_ref_hsdown);
                printf("ack latency         : %u us avg, %u us max (%u acks)\n",
           c->ack_lat_n ? (uint32_t)(c->ack_lat_sum_us / c->ack_lat_n) : 0,
           c->ack_lat_max_us, c->ack_lat_n);
    {   // 探测：线上真有多少帧、生效的上限是多少、实验报了什么（看 rtt ref->best）。
        FFRDP_FRAME_NODE *q; uint32_t out = 0;
        for (q = c->send_list_head; q; q = q->next) if (q->flags & FLAG_FIRST_SEND) out++;
        printf("inflight, cap       : %u, %u (kept %u; probes %u keep %u revert %u too-far %u giveback %u rise %u)\n",
               out, c->cap, c->cap_kept, c->counter_probe, c->counter_probe_keep,
               c->counter_probe_revert, c->counter_probe_narrow, c->counter_giveback, c->counter_cap_rise);
        // ①-46：老比值判据"想探"、被"排队不是我们造的 / 窗口没顶死"挡住的次数（配 q_ours = out/rate 看）。
        printf("probe blocked       : %u (ratio said queue, %s)\n",
               c->counter_probe_block, FFRDP_PROBE_QUEUE_BY_MODEL ? "physical q_ours gate" : "model off");
        // 真正管住线上的那道闸：限的是在途帧数，对端信用不参与（它从来没限制住过什么）。
        printf("in-flight limit     : %u frames (default %u; the probe lowers it when it proves a win)\n",
               c->inflight_max ? c->inflight_max : FFRDP_INFLIGHT_MAX,
               FFRDP_INFLIGHT_MAX);
        // 探测触发时看到的量：waste（‰/秒）、out（在途）、排队判据（典型 RTT 对地板）；尺寸闸读作两个数 —— out/2 必须落在下一行那个上限之下。
        printf("probe trigger       : waste %u pm, rtts/rtt_min %u/%u, out %u (needs %u pm, >%ux, OR ①-39 mem: out >= %u and out/2 under the limit above)\n",
               c->probe_waste, c->rtts, c->rtt_min, out,
               FFRDP_PROBE_WASTE_PM, FFRDP_PROBE_QUEUE_RATIO, (uint32_t)FFRDP_MEMPROBE_MIN_OUT);
        // 1x 就是地板本身，探测赢的时候看它往下掉；rtts 是跨区间平滑、rtt_min 是本区间最低 ⇒ 平滑值可以低于地板，打 0x 像故障，所以 <= 地板一律显示 1x。
        printf("queue test          : rtts %u vs rtt_min %u -> %ux (probe needs >%ux, or waste >= %u pm)\n",
               c->rtts, c->rtt_min,
               (c->rtt_min && c->rtts != (uint32_t)-1 && c->rtts > c->rtt_min)
                   ? c->rtts / c->rtt_min : 1,
               FFRDP_PROBE_QUEUE_RATIO, FFRDP_PROBE_WASTE_PM);
        // 两个数来自同一个估计（探测前的典型 RTT 与探测期间的最低值）⇒ 差就是探测要找的证据：至少降 FFRDP_PROBE_RTT_DROP_MS 且确认速率没掉才 keep。
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
    // "发不出去"的另一半：ffrdp_send 记库的拒绝，这里记内核的（sendto 没接下）。非 0 ⇒ 套接字缓冲对突发不够；受影响的帧被重试过，代价只在这里可见。
    printf("counter_send_err    : %u\n"  , c->counter_send_err    );
    printf("counter_send_query  : %u\n"  , c->counter_send_query  );
    printf("counter_tailprobe     : %u\n", c->counter_tailprobe       );
    printf("counter_tailprobe_lost: %u\n", c->counter_tailprobe_lost  );
    printf("counter_resend_rto  : %u\n"  , c->counter_resend_rto  );
    printf("counter_resend_fast : %u\n"  , c->counter_resend_fast );
    // 下面三个比值各自的分母都可能合理地是 0（还没发过东西的发送方 / 只收的方向 / 全是短帧的稀疏发送方）。
    {   uint32_t resend = c->counter_resend_rto + c->counter_resend_fast;
        // 比值后面都跟着 (分子 / 分母)：分母小的时候一眼看出这个百分比不能当稳态读（如限速时 6/20 会印成 30%）。
        if (c->counter_send_1sttime) printf("counter_resend_ratio: %.2f%% (%u / %u)\n",
                                            100.0 * resend / c->counter_send_1sttime, resend, c->counter_send_1sttime);
        else                         printf("counter_resend_ratio: n/a\n");
        if (resend)                  printf("counter_resend_waste: %.2f%% (%u / %u)\n",
                                            100.0 * c->counter_ack_dup / resend, c->counter_ack_dup, resend);
        else                         printf("counter_resend_waste: n/a\n");
        if (c->counter_send_bytes)   printf("counter_resend_perMB: %.1f\n"  , resend * 1048576.0 / c->counter_send_bytes);
        else                         printf("counter_resend_perMB: n/a\n");
    }
    printf("counter_reach_maxrto: %u\n"  , c->counter_reach_maxrto);
    printf("counter_ack_recv    : %u\n"  , c->counter_ack_recv    );
    printf("counter_ack_mack    : %u\n"  , c->counter_ack_mack    );
    printf("counter_ack_hole    : %u\n"  , c->counter_ack_hole    );
    printf("counter_recv_ooo    : %u\n"  , c->counter_recv_ooo    );
    printf("counter_recv_far    : %u\n"  , c->counter_recv_far    );
    // 乱序帧相对洞的位置与我们报出去的比例；三档之和 = counter_recv_ooo，reach 比值回答"加宽位图前唯一要紧的问题"。
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
    // ①-43：24 位位图盖不住的 ack 次数。≈0 ⇒ "加宽位图"买不到东西（别改线上格式）；真大起来才有依据 ✓。
    printf("counter_bm_clip     : %u of %u acks (%.1f%%) -- 24-bit bitmap could not cover every frame we already held\n",
           c->counter_bm_clip, c->ack_lat_n,
           c->ack_lat_n ? 100.0 * (double)c->counter_bm_clip / (double)c->ack_lat_n : 0.0);
    printf("counter_mack_bits_tx: %u\n"  , c->counter_mack_bits_tx);
    printf("counter_mack_bits_rx: %u\n"  , c->counter_mack_bits_rx);
    printf("counter_rto_backoff : %u\n"  , c->counter_rto_backoff );
    printf("counter_timeout_evt : %u\n"  , c->counter_timeout_evt );
    printf("counter_ack_una_dup : %u\n"  , c->counter_ack_una_dup );
    printf("counter_ack_una_regr: %u\n"  , c->counter_ack_una_regress);
    printf("counter_head_ahead  : %u\n"  , c->counter_head_ahead  );
    printf("counter_ack_appstall: %u\n"  , c->counter_ack_appstall);
    printf("counter_rto_head    : %u\n"  , c->counter_rto_head    );
    printf("counter_rto_chain   : %u\n"  , c->counter_rto_chain   );
    printf("counter_rto_stale   : %u\n"  , c->counter_rto_stale   );
    printf("counter_recv_dup    : %u\n"  , c->counter_recv_dup    );
    // 丢掉而不是处理的：bad = 长度不成话的碎片，drop = 越过洞比通告窗口还远的正常帧。健康时两个都是 0，非 0 = 有人在探端口。
    printf("counter_rx_bad      : %u\n"  , c->counter_rx_bad      );
    printf("counter_recv_drop   : %u\n"  , c->counter_recv_drop   );
    printf("counter_ack_dup     : %u\n"  , c->counter_ack_dup     );
    // rtt_tail 是"让 RTO 自己够到延迟尾部"的学习量（见 FFRDP_MIN_RTO）；应收敛到本链路的确认延迟尾部，链路变好后每秒衰减 1/16。
    printf("confirm-wait hist ms: <=16 %u, <=32 %u, <=64 %u, <=128 %u, <=256 %u, <=512 %u, <=1024 %u, more %u (max %u)\n",
           c->counter_conf_16, c->counter_conf_32, c->counter_conf_64, c->counter_conf_128,
           c->counter_conf_256, c->counter_conf_512, c->counter_conf_1024, c->counter_conf_more,
           c->counter_conf_max);
    printf("rto_floor           : %u, rtt_tail %u ms\n"  , c->rto_floor, c->rtt_tail); // 白费率见上面的 counter_resend_waste（同一个量，不再打两遍）
    printf("counter_rto_behind  : %u\n"  , c->counter_rto_behind  );
    // TIMER 走了哪一支（ack-only 不跑整圈 ⇒ fire 可以远多于 lap）。
        printf("timer branch        : ack-only %u, full-tick %u (ack-only runs no lap, so fires can far exceed laps)\n",
           c->counter_fire_ackonly, c->counter_fire_tick);
    printf("timer due given     : zero %u, <=now %u, now+1..4 %u, >now+4 %u (the '<=now' one asks to go now)\n",
           c->counter_fire_d0, c->counter_fire_dnow, c->counter_fire_dnear, c->counter_fire_dfar);
    printf("timeout dist ①-28A  : <=7 %u, 8-15 %u, 16-23 %u, 24-63 %u, >=64 %u, max %u (distance from ack una; the ack bitmap covers 24)\n",
           c->counter_to_d0_7, c->counter_to_d8_15, c->counter_to_d16_23, c->counter_to_d24_63, c->counter_to_dM, c->counter_to_dmax);
    printf("send wake ①-35      : calls %u, wakes %u, skipped %u (wakes/call %.2f; coalesce %s)\n",
           c->counter_send_calls, c->counter_send_wakes, c->counter_send_wake_skip,
           c->counter_send_calls ? (double)c->counter_send_wakes / (double)c->counter_send_calls : 0.0,
           FFRDP_SENDWAKE_COALESCE ? "ON" : "off");
    printf("footprint ①-35      : sizeof(conn) %u, sizeof(context) %u, sizeof(frame node) %u bytes (static part; each in-flight frame costs one node + smss)\n",
           (unsigned)sizeof(FFRDPCONN), (unsigned)sizeof(FFRDPCONTEXT), (unsigned)sizeof(FFRDP_FRAME_NODE));
    printf("memprobe ①-39       : %s (FFRDP_MEMPROBE_ENABLE), fired %u of %u probes; floor %u frames, kept %u of ceiling %u (peak in-flight ~%u KB)\n",
           FFRDP_MEMPROBE_ENABLE ? "ON" : "off", c->counter_probe_mem, c->counter_probe,
           (unsigned)FFRDP_INFLIGHT_MIN, c->inflight_max ? c->inflight_max : FFRDP_INFLIGHT_MAX, FFRDP_INFLIGHT_MAX,
           (unsigned)(((c->inflight_max ? c->inflight_max : FFRDP_INFLIGHT_MAX) * (sizeof(FFRDP_FRAME_NODE) + 4u + FFRDP_MAX_MSS + 2u)) / 1024));
        printf("pseudo-nocut ①-28B  : %s (FFRDP_PSEUDO_NOCUT), times it would have halved: %u\n",
           FFRDP_PSEUDO_NOCUT ? "ON (pseudo-timeouts do not halve cwnd)" : "off (pseudo-timeouts halve, as before)", c->counter_pseudo_nocut);
    printf("gate knobs ①-27     : inflight %u, max_cwnd %u, waitsnd %u (compile-time macros: FFRDP_INFLIGHT_MAX / FFRDP_MAX_CWND_SIZE / FFRDP_MAX_WAITSND)\n",
           FFRDP_INFLIGHT_MAX, FFRDP_MAX_CWND_SIZE, FFRDP_MAX_WAITSND);
    printf("unsent ①-41         : budget %u frames (FFRDP_UNSENT_MAX; 0 = off), now %u, wait_snd %u (this is what took over the app queue's burst shaping)\n",
           FFRDP_UNSENT_MAX, c->tx_unsent, c->wait_snd);
    printf("ack mode            : %s (FFRDP_ACK_COALESCE=%d; 0 = per-frame on receive, 1 = one per lap)\n",
           FFRDP_ACK_COALESCE ? "coalesced (one per lap)" : "immediate (on receive)", FFRDP_ACK_COALESCE);
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
        ffrdp->tick_ffrdp_dump = NOW_MS();
        memset(&c->counter_send_bytes, 0, (uint8_t*)&c->reserved - (uint8_t*)&c->counter_send_bytes);
    }
}

static void ffrdp_dump_locked(FFRDPCONTEXT *ffrdp, int conn, int clearhistory)
{
    int i;
    if (conn >= 0) {
        // 指定连接：只这一条 ✓（空槽 / 越界在 ffrdp_dump 那层已经被拦掉 ✓）。
        FFRDPCONN *c = ffrdp_conn_lookup(ffrdp, conn);
        if (c) ffrdp_dump_conn(ffrdp, c, conn, clearhistory);
        return;
    }
    for (i = 0; i < ffrdp->nconn; i++) {
        FFRDPCONN *c = ffrdp->table[i];
        // ①-38d：服务端那块内嵌 0 号槽在还没有连接时不整表打出来（它每圈被空跑、计数在涨 ⇒ 躲不过"完全空闲"那道门）。
        if (c && ffrdp->is_server && c == &ffrdp->conn && c->peer_addr == 0) continue;
        // 打**句柄**（不是槽号 ✓）：槽位回收之后两者已经不是一个东西 ✗（句柄 17 = 代号 1、槽 1 ✓）
        if (c) ffrdp_dump_conn(ffrdp, c, (int)c->handle, clearhistory);
    }
    // ①-38：只读读数 —— 那两块"按需分配"的大缓冲**实际分配了多少字节** ✓。
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

void ffrdp_dump(void *ctxt, int conn, int clearhistory)
{
    FFRDPCONTEXT *ffrdp = (FFRDPCONTEXT*)ctxt;
    if (!ffrdp) return;
    // conn >= 0 ⇒ 只打那一条；conn < 0 ⇒ 表里每条一块。
    if (conn >= 0 && !ffrdp_conn_lookup(ffrdp, conn)) return;   // 句柄带代号 ⇒ 只能查表判有效 ✓
    // 打印的全是协议状态，而协议跑在工作线程里：拿住同一把锁再读 ⇒ 打出来的不会是被拆散的一半。
    pthread_mutex_lock(&ffrdp->lock);
    if (conn < 0) printf("%s---- connection table: %d slots, one block each below ----\n", (ffrdp->nconn > 1 ? "\n" : ""), ffrdp->nconn);
    // 握手被拒：只有服务端会遇到（表满 / 内存不足），而且**原来没有任何出口** ⇒ 出了这事日志一片干净 ✗
    // （6 客户端那轮：服务端槽位用满后 SYN 全被丢、c4/c6 干等 10 s 握手超时，日志上看不出原因 ✓）。
    // 槽位是**回收复用**的 ✓ ⇒ 拒连只可能是"此刻一条空槽都没有"（live == maxconn）或是内存不足 ✓。
    if (conn < 0) printf("---- hs refused (no free slot): %u | live conns %d of maxconn %d ----\n",
                         ffrdp->table[0] ? ffrdp->table[0]->counter_hs_refused : 0,
                         ffrdp->live_conn, ffrdp->maxconn);
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
#else
/* DIAG=0：公共入口保留（调用方不用改 ✓），但一条都不打 ✓。 */
void ffrdp_dump(void *ctxt, int conn, int clearhistory) { (void)ctxt; (void)conn; (void)clearhistory; }
#endif
