ffrdp 是一个基于 udp 的快速可靠协议

rto 计算：
初始：
rtts = rttm
rttd = rttm / 2;

迭代：
rtts = (1 - alpha) * rtts + alpha * rttm; // alpha = 1 / 8
rttd = (1 - beta ) * rttd + beta  *(rttm - rtts); // beta = 1 / 4

正常：
rto  = rtts + r * rttd;
超时：
rto  = 1.5 * rto

帧定义：
data frame: 0x00 seq0 seq1 seq2 data ...
ack  frame: 0x01 una0 una1 una2 mack0 mack1 wind0 wind1
win0 frame: 0x02
win1 frame: 0x03 wind0 wind1
bye  frame: 0x04

协议特点：
选择性重传、快速重传、非延迟 ACK、UNA + MACK、非退让流控


