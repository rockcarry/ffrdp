ffrdp 是一个基于 udp 的快速可靠协议

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
data frame: 0x00 seq0 seq1 seq2 data ...
ack  frame: 0x01 una0 una1 una2 mack0 mack1 wind0 wind1
win0 frame: 0x02
win1 frame: 0x03 wind0 wind1
bye  frame: 0x04


协议特点：
选择重传、快速重传、非延迟 ACK、UNA + MACK、非退让流控


协议说明：
seq una 长度为 24bit，recv_win_size 为 16bit
ack 帧包含了 una, mack 和 recv_win_size 信息
mack 16bit 是一个 bitmap, 包含了 una 之后，但又已经被 ack 的帧号
win0 和 win1 命令用于查询和应答对方的 recv_win_size
bye 命令用于客户端跟服务器端说再见

例如：una: 16, mack: 0x0003 这个应答代表
ack 1  1  1  1  0  1  1  0  0  0  0
seq 12 13 14 15 16 17 18 19 20 21 22 ...
这些帧已经被接收方收到并应答

una+mack 的方式被用于选择重传和快速重传


rockcarry
2020-9-1



