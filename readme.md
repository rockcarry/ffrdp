# ffrdp

ffrdp 是一个基于 **UDP + ARQ + FEC** 的快速可靠传输协议。目标是 **低延迟** + **弱网下带宽稳定**。

单文件实现（`ffrdp.c`）+ 一个测试程序（`test.c`），除 `pthread` 外**无第三方依赖**，
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

与 v1.0.0 相比增长的部分，几乎全部是针对缓冲膨胀链路的优化（并发控制与延迟探测），**不是协议本身变复杂了**
（帧格式只多了一个可选字节，见下）。**规模参考**（`ffrdp.c`，协议核心）：v1.0.0 **670 行** ✓、v1.1.0 **1316 行** ✓；
v1.2.0 起传输层独立成文件（`lowlevel-udp.c` 约 380 行，另加 `socket.c` / `utils.c` 若干 ✓）。

---

## 二、协议版本

| 版本 | **分支** | 帧格式 | 内容 |
|---|---|---|---|
| **v1.0.0** | `v1.0.x` | 4 字节帧头；ACK 8 字节 | 基础 ARQ（选择重传/快重传/UNA+MACK/rwnd）+ FEC |
| **v1.1.0** | `v1.1.x` | **未变** ✓ | 协议**逻辑**优化：ACK 回传白费计数（唯一新增的可选字节）、对端证据驱动的 RTO 下限、快重传等待纪律、在途量闸门 + 延迟探测 |
| **v1.2.0**（当前）| `v1.2.x` | **未变** ✓ | **传输层重构**（`lowlevel` 接口 + 工作线程驱动）与随后的协议修正（唤醒判据、当圈回 ack、窗口定标）；参数与规范见 [`docs/ffrdp-protocol.md`](docs/ffrdp-protocol.md) §3.1 |

**怎么认版本** ✓：看 dump 里的 `build : … compiled <日期>` 行，再对上编译时的 commit ✓。
（`version : v1.2.0` 这一行是**当前工作树**才有的 —— `ffrdp.h` 的 `FFRDP_VERSION` ✓；
三个分支提交与更早编出来的二进制都**没有**这一行 ✗。）

**帧格式兼容性结论**：v1.1.0 **没有引入新帧类型、没有改动任何既有字段的宽度与含义**，
唯一变化是 ACK 帧尾部多了一个**可选**字节（`dups`，仅在"有内容可报"时发送）。**v1.2.0 同样未改动帧格式与 ACK 语义** ✓，但 **`ffrdp.h` 的 API 有三处签名变化** ✗（见 §3.1.6 ✓）：`ffrdp_init()` 改收地址字符串、`send` / `recv` 各多一个 `timeout_ms`、`ffrdp_update()` 删除。

⇒ 新老两端可以互相对接（双向兼容，推理与代码证据见
[`docs/ffrdp-protocol.md`](docs/ffrdp-protocol.md) §2.1 与 §3.1）。

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
- **不限速时的速率由测试程序自己决定**（framing + crc32 + 伪随机填充的 CPU 开销 ✓），远高于真实链路 ✓；
- 不带任何参数时只打印用法，然后退出；
- **退出请用 `quit` / `exit`** ✓ —— 只有这种方式会**先打印上面的 `verify:` 汇总**再退出 ✓；
- 内部固定 `ffrdp_init(addr, NULL, NULL, server, 1280, 0)`，`addr` 由 `peer->ip` + `*peer->port` 拼成
  `"ip:port"`
  ⇒ **MSS 1280 字节、FEC 冗余度 0（关闭）**，需要别的组合请改 `test.c`。

### lowlevel 一致性测试（`tools/test-lowlevel-udp.sh`）

v1.2.0 起协议层与 socket 解耦，底座是 `lowlevel.h` 定义的传输端点（发 + 收 + 定时器，
自带工作线程）。这一层有自己的契约测试，只走 127.0.0.1、端口按 pid 取，不需要网络与配置：

```bash
bash tools/test-lowlevel-udp.sh            # 普通构建（-Wall -Wextra -Werror）
ASAN=1 bash tools/test-lowlevel-udp.sh     # 打开地址消毒器（含泄漏检测）
```

逐节覆盖：参数解析 ✓、收包消息与来源地址 ✓、回发（含回调内 `send` 与主线程 `send`）✓、
载荷边界（含 1500 截断）✓、节流与突发负载 ✓、双端点与并发 ✓、生命周期（70 次 init/exit 的
fd/线程零泄漏 ✓、无期限时 `exit` 及时返回 ✓）、定时器（期限精度 / 已过即立刻 / 覆盖式 /
回调内重设 / 跨线程催发 / `RECV` 与 `TIMER` 不交错 / 回调不嵌套）✓。
设计取舍与实测数字见 [`docs/notes.md`](docs/notes.md) 第 13 节。

### 真机 A/B 对测（`test.sh`）

把一轮真机对照压成一条命令 ✓：**上传二进制 → 远端起服务端 → 本地起客户端 → 取回日志 → 出摘要** ✓。

```bash
./test.sh <本地二进制> <user@host> [密码] [时长秒]
export SESSION=z5-run1        # 一批扫描共用一个标识，便于成组比较（等价于 STAMP）
./test.sh dist/test_ffrdp_w5a4 chenk@192.168.0.213 abc 120
column -s, -t logs/summary.csv | tail -10     # 摘要表
```

- 没装 `sshpass` 时它会开一条 **SSH 复用连接** ⇒ 整轮只输一次密码 ✓（`KEEP_MUX=1` 可连跑多轮只输一次 ✓）；
- **两侧不同版本**做交叉对照：`REMOTE_BIN=<对端二进制> ./test.sh <本端二进制> <user@host> …` ✓
  （本地仍用第 1 个参数，远端用 `REMOTE_BIN` ✓ —— 用来分辨"某个方向被掐"是本地发送还是对端确认的问题 ✓）；
- 其它开关：`DUR` / `PORT` / `LOGDIR` ✓、`CPUWATCH=1`（采样客户端 CPU ✓）、`CLEAN=1`（跑完清远端 ✓）、`DRY=1`（只打印计划、不碰网络 ✓）；
- 每轮结果追加进 **`$LOGDIR/summary.csv`** ✓（`stamp,tag,role,resend_pct,waste_pct,rtts_ms,send_recv_MBps` ✓）⇒ 扫完一串二进制直接看表 ✓；
- 读判据请对着协议文档：`rttm/rtts`、`counter_resend_ratio`、`confirmed rate`，以及 **`lap period avgmax` 的最大值**（唤醒是否正常 ✓）
  —— 见 [`docs/ffrdp-protocol.md`](docs/ffrdp-protocol.md) §3.2.4 ✓。

---

## 五、目录结构

```
.
├── ffrdp.c                 协议实现（单文件：ARQ + FEC + 流控/并发控制 + 诊断）
├── ffrdp.h                 对外 API（7 个函数，见下；v1.2.0 有三处签名变化）
├── lowlevel.h              lowlevel 接口（tx + rx + timer，自带工作线程）—— v1.2.0 起
├── lowlevel-udp.c          lowlevel 的 UDP 实现（绑定 + 收发 + timer + 唤醒 socket）
├── socket.c / socket.h     socket 薄封装（初始化、选项、超时、非阻塞）
├── utils.c / utils.h       时间戳、线程 setname/getname、模块线程状态机
├── test.c                  测试程序（服务端/客户端共用一个参数化的对等端循环 + 每 10 秒 dump）
├── test.sh                 **真机 A/B 对测脚本**（上传 → 远端服务端 → 本地客户端 → 收日志 → 出摘要；用法见 §四）
├── build.sh                构建脚本（静态库 + 测试程序 + pkg-config）
├── readme.md               本文件：概述、版本、编译、测试、目录
├── LICENSE
├── docs/
│   ├── ffrdp-protocol.md   协议说明：v1.0.0 / v1.1.0 / v1.2.0 三部分（帧格式、参数、改动与依据）
│   ├── sim-test.md         差分仿真工具（tools/sim）使用说明
│   └── notes.md            开发与调试记录（按时间顺序，含每次上机的原始 log）
└── tools/                  辅助工具（不逐个列 ✓：差分仿真 `tools/sim/`、敌意输入、在途上限扫描、lowlevel 契约测试、日志提取 —— 以目录实际内容为准 ✓）
```

### 对外 API（`ffrdp.h`）—— **v1.2.0** ✓
> ⚠️ `ffrdp.h` 现在是 **v2.0.0** ✓（`FFRDP_VERSION` ✓）：签名见 [`docs/ffrdp-v2.md`](docs/ffrdp-v2.md) ✓；
> 下面列的是 **v1.2.x 分支**的签名 ✓，作为旧接口留档 ✓。注意 `hs` **默认已翻成开** ✓
> ⇒ 想沿下面这条老路走，params 里要显式 `hs:0` ✓。

⚠️ **下面是 v1.2.0 的签名** ✓；相对 v1.0.0 / v1.1.0 **有三处变化** ✗（`ffrdp_init` 改收地址字符串、
`send` / `recv` 多一个 `timeout_ms`、`ffrdp_update` 删除）⇒ 细节与理由见
[`docs/ffrdp-protocol.md`](docs/ffrdp-protocol.md) §3.1.6 ✓。

```c
void* ffrdp_init  (char *addr, char *txkey, char *rxkey, int server, int smss, int sfec);   // addr = "ip:port" / "ip"（端口默认 8000）
void  ffrdp_free  (void *ctxt);
int   ffrdp_send  (void *ctxt, char *buf, int len, int timeout_ms);  // 原子：len = 整条接受；-1 = 一个字节都没接受
int   ffrdp_recv  (void *ctxt, char *buf, int len, int timeout_ms);  // 取应用层数据（有序可靠**字节流**，无消息边界）
int   ffrdp_isdead(void *ctxt);                       // 对端是否已失联
void  ffrdp_flush (void *ctxt);                       // 把未填满的帧立刻发出（不要攒着）
void  ffrdp_dump  (void *ctxt, int clearhistory);     // 打印诊断
```

- **`timeout_ms`** ✓：**`0` = 不等待** ✓（旧行为 —— 既有调用方加一个 `, 0` 即完全等价 ✓）；
  **`> 0` = 最多等这么多毫秒** ✓（等待期间协议照常跑 ✓）；**`< 0` = 等到有数据 / 能接受为止** ✓。
- **`ffrdp_recv`** ✓：有数据就立刻返回 ✓，不为凑满 `len` 而等 ✓。
- **`ffrdp_update()` 已删除** ✓：协议跑在 lowlevel 的工作线程上 ✓，应用侧只等 `ffrdp_recv` 的 timeout ✓。
- **`addr` 的写法与解析** ✓（`utils.h` 的共享约定）：见 `docs/notes.md` 第 21、22 节 ✓。

---

## 六、文档索引

| 想了解 | 看这里 |
|---|---|
| 帧格式、mack、FEC、RTO 计算、通信流程 | [`docs/ffrdp-protocol.md`](docs/ffrdp-protocol.md) 第一部分（v1.0.0）|
| 每个改动的动机、被否掉的路线、当时的上机原始 log | [`docs/notes.md`](docs/notes.md) |
| 没有真机时怎么验证"没有回归" | [`docs/sim-test.md`](docs/sim-test.md) |
| v2.0.0 的接口（params / 句柄 / 事件 / 加密回调 / 握手）| [`docs/ffrdp-v2.md`](docs/ffrdp-v2.md) |

---

## 七、作者

rockcarry@163.com
