# ffrdp

ffrdp 是一个基于 **UDP + ARQ + FEC** 的快速可靠传输协议。目标是 **低延迟** + **弱网下带宽稳定**。

单文件实现（`ffrdp.c`）+ 约 200 行测试程序（`test.c`），除 `pthread` 外**无第三方依赖**，
不需要内核改动，也不改变 UDP 语义。

---

## 一、协议概述（它和 TCP 的不同）

| 特点 | 说明 |
|---|---|
| 自己完整实现 ARQ | 确认、超时、重传、窗口、流控全在用户态，可以按链路形态决定策略，不被内核固定策略绑住 |
| 选择重传 + 非延迟 ACK | 用 **UNA + MACK 位图**精确表达"到底缺哪一帧"（而不是"从第几号往后全丢"）；收到数据尽快回 ACK，避免延迟确认白吃一个往返 |
| 快速重传（带等待纪律） | 对端报的"洞"多数其实是**乱序**而不是丢包 ⇒ 等待时长由**对端证据**决定（v1.1.0 起），而不是一律立即重传 |
| 前向纠错 FEC | 异或方式实现，每 N 帧附一个 FEC 帧，N 由帧类型携带（应用可选，默认关闭） |
| 面向"缓冲膨胀"链路的延迟治理 | v1.1.0 的核心：把**同时在途的帧数**当成真正的刹车，并用**一次可还原的实验**确定它该是多少 |
| 对端证据驱动 | RTO 下限、快重传等待时长都由"对端重复上报"决定 —— 这是链路上唯一不会被本端计数污染的判据 |
| 诊断友好 | `ffrdp_dump()` 输出延迟三分量、在途量、重传、白费比例等；`build :` 行自证二进制版本，避免"跑错二进制" |

规模参考：`ffrdp.c` 当前约 **1080 行**；v1.0.0 是 **671 行**。
多出来的部分几乎全部是针对缓冲膨胀链路的优化（并发控制与延迟探测），**不是协议本身变复杂了**
（帧格式只多了一个可选字节，见下）。

---

## 二、协议版本

| 版本 | 范围 | 帧格式 | 内容 |
|---|---|---|---|
| **v1.0.0** | 提交 `55ead4a` 及之前 | 4 字节帧头；ACK 8 字节 | 基础 ARQ（选择重传/快重传/UNA+MACK/rwnd）+ FEC |
| **v1.1.0** | `55ead4a` 之后（`58167b3` 起）到现在 | **未变** ✓ | 协议**逻辑**优化：ACK 回传白费计数（唯一新增的可选字节）、对端证据驱动的 RTO 下限、快重传等待纪律、在途量闸门 + 延迟探测 |

**帧格式兼容性结论**：v1.1.0 **没有引入新帧类型、没有改动任何既有字段的宽度与含义**，
唯一变化是 ACK 帧尾部多了一个**可选**字节（`dups`，仅在"有内容可报"时发送）。

⇒ 新老两端可以互相对接（双向兼容，推理与代码证据见
[`docs/ffrdp-protocol.md`](docs/ffrdp-protocol.md) 第 8 节）。

---

## 三、编译

需要 `gcc`（或交叉工具链）+ `pthread`。构建脚本是 `build.sh`：

```bash
TOOLCHAIN_NAME=gcc ./build.sh                                  # 本机编译
TOOLCHAIN_NAME=arm-linux CROSS_COMPILE=arm-linux- ./build.sh   # 交叉编译
./build.sh clean                                               # 清理产物
```

产物（`BUILD_PACKAGE_OUTDIR` 可覆盖，默认 `./out`）：

```
out/$TOOLCHAIN_NAME/lib/libffrdp.a                      静态库
out/$TOOLCHAIN_NAME/include/ffrdp.h                     头文件
out/$TOOLCHAIN_NAME/lib/pkgconfig/libffrdp.pc           pkg-config 描述文件
./test_ffrdp                                            测试程序（留在源码目录）
```

`build.sh` 的编译参数是 `-Wall -Werror`（**警告即错误**），链接时加 `-lpthread`
（Windows/MinGW 目标额外加 `-lws2_32 -lwinmm`）。

不想用脚本时的最小手工编译：

```bash
gcc -Wall -Werror -c ffrdp.c -o ffrdp.o
ar  rcs libffrdp.a ffrdp.o
gcc -Wall -Werror test.c libffrdp.a -lpthread -o test_ffrdp
```

---

## 四、测试程序 test_ffrdp

`test_ffrdp` 在两个线程里各跑一个 ffrdp 实例，双方**不停互发随机长度的数据**，
每 10 秒自动 `ffrdp_dump()` 一次，用来观察延迟、重传、在途量等指标。

```bash
./test_ffrdp --server=0.0.0.0:8000            # 一端做服务端
./test_ffrdp --client=192.168.1.10:8000       # 另一端做客户端
./test_ffrdp --server --client                # 本机自测（两端在同一个进程里）
```

| 参数 | 含义 | 默认 |
|---|---|---|
| `--server[=ip:port]` | 作为服务端，并在该地址绑定 | `0.0.0.0:8000` |
| `--client[=ip:port]` | 作为客户端，并连接该地址 | `127.0.0.1:8000` |
| `--server_max_send_size=N` | 服务端**每条消息**的最大字节数（含 framing 头尾 12B）| 16384 |
| `--client_max_send_size=N` | 客户端**每条消息**的最大字节数（含 framing 头尾 12B）| 16384 |
| `--send_speed=N` | **发送限速**：把本进程的发送速率限制在 N KB/s | `0`（不限速）|

### 消息校验（每次运行都会做）

`ffrdp_recv()` 是**有序可靠字节流**（没有消息边界），所以测试程序自己给每条消息加了 framing：

```
[4B 总长 len][4B seq][payload...][4B crc32]      crc32 覆盖 len + seq + payload
```

接收端把字节流拼回消息并逐条校验：**seq 连续性**（有没有丢/乱 ✓）、**crc32**（内容有没有被破坏 ✓）、
**长度合法性**（流有没有失步 ✓）；发送端只在 `ffrdp_send()` 返回整条长度时才 `seq++` ✓
（`ffrdp_send()` 是**原子**的：要么整条被接受、要么返回 `-1` 什么都不接受 ✓）。

出错会打印 `VERIFY ...` 日志（同类错误最多逐条打 3 次，其余只计数 ✓），退出时打印汇总：

```
[client] verify: sent 285 msgs / 1638311 B, recv 285 msgs / 1638311 B | seq_gap 0 (missing 0), crc_err 0, len_err 0, partial_err 0
[client] verify: OK —— 收到的 285 条消息 seq 连续、crc 全部正确 ✓
```

说明：

- `--server` 与 `--client` **可以同时给**（同进程自测）；此时 `--send_speed` 对两端都生效；
- **`--send_speed` 用来像 iperf3 那样控制发送速率**：`--send_speed=100` 时，
  每秒交给 `ffrdp_send()` 的数据量就约 100KB（实测 **100.37KB/s** ✓；
  1KB/s、20KB/s、500KB/s 同样贴合 ✓）。
  实现是令牌桶：每 1 秒补满 1 秒的额度，并按剩余额度**选**这一条消息的大小
  （消息是原子的，不能裁剪 ✓）；不想限速就传 `0` 或不传 ✓；
- **不限速时的速率由测试程序自己决定**：本机回环约 **90~100MB/s** ✓
  （framing + crc32 + 伪随机填充的 CPU 开销 ✓），远高于真实链路 ✓；
- 不带任何参数时只打印用法，然后退出；
- **退出请用 `quit` / `exit`** ✓ —— 只有这种方式会**先打印上面的 `verify:` 汇总**再退出 ✓；
- 内部固定 `ffrdp_init(ip, port, NULL, NULL, server, 1280, 0)`
  ⇒ **MSS 1280 字节、FEC 冗余度 0（关闭）**，需要别的组合请改 `test.c`。

---

## 五、目录结构

```
.
├── ffrdp.c                 协议实现（单文件：ARQ + FEC + 流控/并发控制 + 诊断）
├── ffrdp.h                 对外 API（init/free/send/recv/update/flush/isdead/dump）
├── test.c                  测试程序（服务端/客户端共用一个参数化的对等端循环 + 每 10 秒 dump）
├── build.sh                构建脚本（静态库 + 测试程序 + pkg-config）
├── readme.md               本文件：概述、版本、编译、测试、目录
├── LICENSE
├── docs/
│   ├── ffrdp-protocol.md   协议说明：v1.0.0 帧格式与基本协议 + v1.1.0 优化（原理/证据/被否路线）
│   ├── sim-test.md         差分仿真工具（tools/sim）使用说明
│   └── notes.md            开发与调试记录（按时间顺序，含每次上机的原始 log）
└── tools/
    ├── sim/                差分仿真：虚拟时钟 + 假 socket，把两版代码放在同一条确定假链路上 A/B
    └── keylog.sh           从 test_ffrdp 的输出里只留关键行，并打印"最后一块汇总 + 逐块趋势"
```

### 对外 API（`ffrdp.h`）

```c
void* ffrdp_init  (char *ip, int port, char *txkey, char *rxkey, int server, int smss, int sfec);
void  ffrdp_free  (void *ctxt);
int   ffrdp_send  (void *ctxt, char *buf, int len);   // 原子：返回 len = 整条接受；-1 = 什么都没接受（队列满）
int   ffrdp_recv  (void *ctxt, char *buf, int len);   // 取应用层数据（有序可靠**字节流**，无消息边界）
int   ffrdp_isdead(void *ctxt);                       // 对端是否已失联
void  ffrdp_update(void *ctxt);                       // 必须被周期性调用（收发/计时/重传都在里面）
void  ffrdp_flush (void *ctxt);                       // 把未填满的帧立刻发出（不要攒着）
void  ffrdp_dump  (void *ctxt, int clearhistory);     // 打印诊断
```

> `ffrdp_update()` 需要应用**周期性调用**（`test.c` 里是主循环每轮调用一次），
> 协议的所有定时行为都以它被调用的时刻为准。

---

## 六、文档索引

| 想了解 | 看这里 |
|---|---|
| 帧格式、mack、FEC、RTO 计算、通信流程 | [`docs/ffrdp-protocol.md`](docs/ffrdp-protocol.md) 第一部分（v1.0.0）|
| 缓冲膨胀、"这 200ms"、v1.1.0 改了什么、为什么这么改 | 同一份文档第二部分（v1.1.0）|
| 每个改动的动机、被否掉的路线、当时的上机原始 log | [`docs/notes.md`](docs/notes.md) |
| 没有真机时怎么验证"没有回归" | [`docs/sim-test.md`](docs/sim-test.md) |

---

## 七、作者

rockcarry@163.com
