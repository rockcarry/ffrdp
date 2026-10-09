ffrdp 是一个基于 udp + arq + fec 的快速可靠协议

（原始实现约 750 行（ffrdp.c）+ 200 行测试程序（test.c），实现了完整的 udp + arq + fec，简洁高效。）

当前 ffrdp.c 为 1068 行：多出的约 330 行是针对"缓冲膨胀"链路的优化（原理与实测见 OPTIMIZATION.md），
不是协议本身变复杂了。

rto 计算
--------

测量：
rttm = 当前时间 - 报文发送时间
（关键限制：不能对重传报文段测量 rttm，因为收到 ACK 时无法判断它确认的是原始报文还是重传报文）

另单独记录：
rtt_min = 历史最小 rttm（不被滑动平均稀释）
它代表"这条路最快能有多快"。当前往返明显高于 rtt_min 时，说明路上堆积了队列。
（队列的来源可能是我们自己放出去的在途帧，见"在途量闸门 + 延迟探测"）

初始：
rtts = rttm;
rttd = rttm / 2;

迭代：
rtts = (1 - alpha) * rtts + alpha * rttm; // alpha = 1 / 8
rttd = (1 - beta ) * rttd + beta  * abs(rttm - rtts); // beta = 1 / 4

正常：
rto = rtts + 4 * rttd;

超时：
rto = 1.5 * rto;  // 每往返退让一次（不是每帧一次）

其中 rtts 恒 <= rto：Karn 规则只统计"从未超时"的帧，所以想用 rtts / rttd 把 rto 抬上去是无效的。

约束：
rto = max(rto, rto_floor);
rto = max(rto, FFRDP_MIN_RTO);  // 16   ms
rto = min(rto, FFRDP_MAX_RTO);  // 2000 ms


rto_floor —— 由"对端证据"驱动的下限
------------------------------------

问题：这条链路的往返分布有很长的尾巴（帧是迟到，不是丢了），固定 4 倍 rttd 落在分布中部，
      于是产生大量伪超时 —— 重传的帧其实早就到了对端。

证据：ACK 的可选第 9 字节（dups，见下）—— 对端告诉我们"你的重传里有多少我早就收到了"。
      这是链路上唯一不会被本端计数污染的判据（本端只知道自己重传了几次，不知道对端是否早就有）。

做法：以证据窗口为单位（FFRDP_RTO_WIN = 128 次重传，约 1 秒），计算
        waste = 对端重复上报数 / 本端重传数 * 100

      waste >= 80%  => rto_floor = min(rto_floor * 5 / 4, 1000)  // 抬高下限，减少伪超时
      waste <  40%  => rto_floor = 0                             // 重传是真修复，立刻归还下限
      其余          => rto_floor = rto_floor * 9 / 10            // 中间地带，缓慢让步

      每个窗口之后再施加相对上限：rto_floor = min(rto_floor, max(3 * rtts, 100))
      （链路变快时下限会自动回落）

注意：安静窗口不触发衰减 —— 在限速链路上"没有重传"往往正是下限在起作用，
      对它衰减会让环路自我拆台。


快重传纪律（hole_wait）
-----------------------

问题：对端报的"洞"多数是乱序，不是丢包 => 立刻快重传 = 白费（实测占重传的 65%）。

做法：等待时长 hole_wait 同样由对端重复上报决定：
      waste >= 80% => 等 3 * rtt（最多 300ms）
      waste <  40% => 0（立刻重传，与原来的行为完全一致）
      其余         => rtt

理由：与 rto_floor 同一个原则 —— 不用有歧义的信号（丢包 / 乱序分不清），用接收端的报告。


在途量闸门 + 延迟探测
---------------------

背景：对端广告的 rwnd 是"它还有多少接收缓冲区"，不是"这条链路能跑多少"。
      实测这条链路只需要几帧（BDP），而 rwnd 有 148~256 帧 => 多出来的帧全部堆在路径设备的
      缓冲里，形成约 200ms 的常驻队列（排队时间 = 积压量 / 链路速率）。
      用 ping 可见：旧版跑传输时 ping 220ms，传输一停立刻回到 4~7ms。

在途量 = 已首发、未确认的帧数（重传不计入、也不受限制 => 修复路径永不被挡）。

延迟探测（默认开启，FFRDP_PROBE_ENABLE）：
  触发：rtts > 4 * rtt_min（说明有我们撑住的队列）且至少 8 帧在途；
        失败后探测间隔翻倍（10s -> 60s），不反复打扰同一条链路
  动作：把在途上限砍一半，保持 2 * rtt + 500ms
  判定：① 期间最低往返 < 探测前的 85%，且 ② 期间对端确认速率 >= 探测前的 85%
        两条都满足 => 保留该上限（下次可再砍一半，逐步逼近地板）
        否则       => 立刻还原（延迟没掉 = 不是我们的；速率掉了 = 砍过头）
  安全网：还原逻辑保证"砍错"只损失一次 1.5 秒的半窗口。

为什么必须是"实验"而不是"持续观测"：常驻队列让"历史最小往返"本身就含队列
=> "现在 vs 最小"永远看不出差异 => 纯观测方案（延迟梯度一类）在这条链路上是瞎的。
实验比较的是"砍之前 vs 砍之后"，不依赖任何干净基线。


帧定义：
data frame:
data_full  frame: 0x00 seq0 seq1 seq2 data ...
data_short frame: 0x01 seq0 seq1 seq2 data ...
data_fec2  frame: 0x02 seq0 seq1 seq2 data ... fec_seq0 fec_seq1
data_fec3  frame: 0x03 seq0 seq1 seq2 data ... fec_seq0 fec_seq1
data_fec4  frame: 0x04 seq0 seq1 seq2 data ... fec_seq0 fec_seq1
... ...
data_fec32 frame: 0x20 seq0 seq1 seq2 data ... fec_seq0 fec_seq1

ack   frame: 0x21 una0 una1 una2 mack0 mack1 mack2 rwnd [dups]
query frame: 0x22

data_full  frame 为不带 fec 的 data 长帧
data_short frame 为不带 fec 的 data 短帧
data_fecN 为每 N 帧带一个 fec 帧（N >= 2 && N <= 32）
帧类型值与 N 相同 => 数据帧占 0x00~0x20，控制帧从 0x21 起

（seq: 帧序号，una: 未确认的帧序号）

协议特点：
选择重传、快速重传（带等待纪律）、非延迟 ACK、UNA + MACK、非退让流控、FEC 前向纠错、
对端证据驱动的 RTO 下限、在途量闸门 + 延迟探测


协议说明：
seq una 长度为 24bit，rwnd 为 8bit（单位：帧，即"对端接收缓冲还能装多少帧"，0~255）
ack 帧基本长度 8 字节，包含了 una, mack 和 rwnd 信息
mack 24bit 是一个 bitmap, bit i (i = 0..23) 对应帧号 una + 1 + i
  即包含了 una 之后、但又已经被 ack 的帧号
  （因此 mack 只能报出 una 之后 24 帧以内的乱序，更远的乱序它表达不了）
rwnd = 剩余接收缓冲 / rmss，上限 255
可选第 9 字节 dups：自上一个 ACK 以来，对端收到了多少个它早就有的重复帧
  发送端据此判断"我的重传有多少是白费的"
  没有重复帧时该字节不发，ACK 仍是 8 字节；老对端不发这一字节 => 发送端读到 0
  => 行为退回原样（向后兼容）
query 命令用于查询 ack
fec_seq 长度为 16bit，位于数据帧的最后两个字节

例如：una: 16, mack: 0x000003 这个应答代表
帧号 16 尚未收到（una 就是"第一个还缺的帧"）
帧号 17、18 已经收到（bit0 = una + 1，bit1 = una + 2）

una+mack 的方式被用于选择重传和快速重传


FEC 说明：
采用异或方式实现 FEC
针对 full frame 即帧长度为 MTU 的帧，进行 FEC 纠错
data frame 的最后两个字节用作 FEC 的 seq.
冗余度 N 由帧类型 data_fecN 携带（N = 2..32），默认 0（不启用）

实测：这条链路真丢包仅 ~0.2%，而 FEC 只救"丢"不救"迟到"，
      5 倍空口换来更多超时（重传率反而升到 7.9%）=> 默认关闭。


主要参数（ffrdp.c 顶部）：
FFRDP_MAX_MSS         1500 - 8 = 1492   // 4 字节帧头 + 2 字节 FEC seq
FFRDP_MIN_RTO         16   ms
FFRDP_MAX_RTO         2000 ms
FFRDP_RTO_FLOOR_MAX   1000 ms           // rto_floor 的上限
FFRDP_MAX_WAITSND     256  帧           // 应用侧发送队列上限
FFRDP_RECVBUF_SIZE    128  帧（≈186KB） // 接收缓冲
FFRDP_QUERY_CYCLE     500  ms
FFRDP_RTO_WIN         128  次重传       // 证据窗口
FFRDP_WASTE_HI / LO   80 / 40 %
FFRDP_MAX_CWND_SIZE   64   帧
FFRDP_INFLIGHT_MAX    64   帧           // 在途量默认上限（= 优化前行为）；实际值由探测得出


参考资料：
OPTIMIZATION.md  优化原理、决策依据、被否掉的路线、实测数据
NOTES.md         按时间顺序的完整开发记录（含每次上机的原始 log）
tools/sim/README.md  仿真工具用法：差分 A/B、17 个内置场景、输出解读与使用纪律


2026-10 更新：
  * 帧类型取值按代码订正：ACK 0x40 -> 0x21，QUERY 0x41 -> 0x22，FEC32 0x3E -> 0x20
  * ACK 长度与 rwnd 位宽按代码订正：基本长度 8 字节，rwnd 为 8bit（原写 16bit）
  * 补充：rto_floor、hole_wait、在途量闸门 + 延迟探测（协议行为的一部分）
  * mack 的示例按代码修正（原例与 una=16 不自洽）
  * 补充参数表与参考资料


rockcarry@163.com