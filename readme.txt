﻿ffrdp 是一个基于 udp + arq + fec 的快速可靠协议

（500 行代码实现了完整的 udp + arq + fec，简洁高效。github 上目前还找不到更简洁的）

livedesk + fanplayer 使用 ffrdp 实现了 100ms 以内低延迟的 wifi 无线音视频实时传输


rto 计算：
初始：
rtts = rttm;
rttd = rttm / 2;

迭代：
rtts = (1 - alpha) * rtts + alpha * rttm; // alpha = 1 / 8
rttd = (1 - beta ) * rttd + beta  * abs(rttm - rtts); // beta = 1 / 4

正常：
rto  = rtts + r * rttd;

超时：
rto  = 1.5 * rto;


帧定义：
data frame:
data_full  frame: 0x00 seq0 seq1 seq2 data ...
data_short frame: 0x01 seq0 seq1 seq2 data ...
data_fec2  frame: 0x02 seq0 seq1 seq2 data ... fec_seq0 fec_seq1
data_fec3  frame: 0x03 seq0 seq1 seq2 data ... fec_seq0 fec_seq1
data_fec4  frame: 0x04 seq0 seq1 seq2 data ... fec_seq0 fec_seq1
... ...
data_fec32 frame: 0x3E seq0 seq1 seq2 data ... fec_seq0 fec_seq1

ack   frame: 0x40 una0 una1 una2 mack0 mack1 mack2 rwnd
query frame: 0x41

data_full  frame 为不带 fec 的 data 长帧
data_short frame 为不带 fec 的 data 短帧
data_fecN 为每 N 帧带一个 fec 帧（N >= 2 && N <= 32）


协议特点：
选择重传、快速重传、非延迟 ACK、UNA + MACK、非退让流控、FEC 前向纠错


协议说明：
seq una 长度为 24bit，recv_win_size 为 16bit
ack 帧包含了 una, mack 和 rwnd size 信息
mack 24bit 是一个 bitmap, 包含了 una 之后，但又已经被 ack 的帧号
query 命令用于查询 ack
fec_seq 长度为 16bit 用于 FEC

例如：una: 16, mack: 0x000003 这个应答代表
ack 1  1  1  1  0  1  1  0  0  0  0
seq 12 13 14 15 16 17 18 19 20 21 22 ...
这些帧已经被接收方收到并应答

una+mack 的方式被用于选择重传和快速重传


FEC 说明：
采用异或方式实现 FEC
针对 full frame 即帧长度为 MTU 的帧，进行 FEC 纠错
data frame 的最后两个字节用作 FEC 的 seq.


rockcarry
2020-9-1



