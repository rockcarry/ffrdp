# ffrdp

> [English](readme.md) | **中文** ✓
>
> **语言** ✓：本文件是**中文版** ✓ —— GitHub 默认显示英文版 [`readme.md`](readme.md) ✓；
> `docs/` 下的文档**暂无其它语言版本** ✗。需要英文 / 日文 / … 时，请**用 AI 自行翻译** ✓
> （把文档整段交给任意大模型即可 —— 记得让它**保留宏名、命令、代码块与 `✓ / ✗` 符号原文** ✓）。

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
| 诊断友好 | `ffrdp_dump()` 输出延迟三分量、在途量、重传、白费比例等；`version & build` 行自证二进制版本，避免"跑错二进制"（**v1.1.0 起**它与 version 合成一行 ✓；只有 v1.0.0 是单独的 `version` 行 ✓）|
| **握手与连接管理**（v2.0.0）| 三次握手建立（SYN / SYN-ACK / ACK）、**FIN 四次挥手**收尾；握手与 FIN 走**独立帧类型**（35）⇒ 不占数据 ARQ 的序号空间 ✓ |
| **服务端多客户端**（v2.0.0）| 服务端按对端地址维护**连接表**（`maxconn`，默认 16 ✓），一条连接一个**句柄**；未握手的来源一律丢弃 ✓ |
| **加密交给应用**（v2.0.0）| 协议只把**载荷原样**交给 `ENCRYPT` / `DECRYPT` 回调（原地改、长度不变 ✓）⇒ 算法与密钥全在应用侧 ✓，整个库**连 openssl 都不再需要** ✓ |
| **空闲不等于断开**（v2.0.0）| 半程没听到对端就发一个探活 `QUERY` 去确权 ✓；对端真哑了才在 `idle_ms` 内（默认 **5 s**）判死并报 `DISCONN` ✓ |

与 v1.0.0 相比增长的部分，几乎全部是针对缓冲膨胀链路的优化（并发控制与延迟探测），**不是协议本身变复杂了**
（帧格式只多了一个可选字节，见下）。**规模参考**：v1.0.0 的 `ffrdp.c` **670 行** ✓、v1.1.0 **1316 行** ✓；
v1.2.0 起传输层独立成文件；v2.0.0 起连接模型与接口重构 —— 现在 `ffrdp.c` **2240 行** ✓、`lowlevel-udp.c` **550 行** ✓、`socket.c` **85 行** ✓、`utils.c` **156 行** ✓、`test.c` **667 行** ✓（合计 **3698 行** ✓，全部标准 C + `pthread` ✓）。

---

## 二、协议版本

| 版本 | **分支** | 帧格式 | 内容 |
|---|---|---|---|
| **v1.0.0** | `v1.0.x` | 4 字节帧头；ACK 8 字节 | 基础 ARQ（选择重传/快重传/UNA+MACK/rwnd）+ FEC |
| **v1.1.0** | `v1.1.x` | **未变** ✓ | 协议**逻辑**优化：ACK 回传白费计数（唯一新增的可选字节）、对端证据驱动的 RTO 下限、快重传等待纪律、在途量闸门 + 延迟探测 |
| **v1.2.0** | `v1.2.x` | **未变** ✓ | **传输层重构**（`lowlevel` 接口 + 工作线程驱动）与随后的协议修正（唤醒判据、当圈回 ack、窗口定标）；参数与规范见 [`docs/ffrdp-v1.md`](docs/ffrdp-v1.md) §3.1 |
| **v2.0.0**（当前）| `master` | v1 帧格式 **未变** ✓ + 新增**握手帧类型 35**（不进数据 ARQ ✓）| **接口与连接模型重构**：句柄式 `send`/`recv` + 事件回调、三次握手与四次挥手、多客户端 `accept`、`keepalive`、加密回调 —— 见 [`docs/ffrdp-v2.md`](docs/ffrdp-v2.md) |

**怎么认版本** ✓：看 dump 里的 `version & build : … compiled <日期> <时刻>` 行（**v1.1.0 起把 `version` 与 `build`
合成了一行** ✓；只有 **v1.0.0** 还是单独的 `version : …` 一行 ✓），再对上编译时的 commit ✓。
（`FFRDP_VERSION` 自 v1.0.0 起就有 ✓ —— 三个 v1 分支与 v2 的 `ffrdp.h` 里都在 ✓；但**更早编出来的二进制**可能没有它 ✗，
那时只能靠 `build` 行 + commit 认 ✓。v2 的 dump 还多了
**按连接**这一层 ✓ —— `ffrdp_dump(ctxt, conn, …)`：`conn < 0` 打总览（每条一块 ✓）、`conn >= 0` **只打那一条** ✓；
每块的**块头**写明句柄 + 对端地址 + 握手状态 ✓；从未通信过的槽只占一行 ✓。）

**帧格式兼容性结论**：v1.1.0 **没有引入新帧类型、没有改动任何既有字段的宽度与含义** ✓，唯一变化是 ACK 帧尾部多了一个**可选**字节（`dups`，仅在"有内容可报"时发送 ✓）。**v1.2.0 同样未改动帧格式与 ACK 语义** ✓，但 **`ffrdp.h` 的 API 有三处签名变化** ✗（见 §3.1.6 ✓）：`ffrdp_init()` 改收地址字符串、`send` / `recv` 各多一个 `timeout_ms`、`ffrdp_update()` 删除。
**v2.0.0 的数据面与 v1 仍然同构** ✓（帧头、ACK、FEC 一个字没改 ✓），**多出来的只有握手帧**：新增帧类型 **35** ✓，而 v1 的解析只认 ≤34 ⇒ 老实现收到会**静默丢弃** ✓（细节见 [`docs/ffrdp-v2.md`](docs/ffrdp-v2.md)）。⇒ 互通与否**由握手开关决定** ✓：`hs:0`（老路径）两个方向都能通 ✓；**默认 `hs:1` 与 v1 不互通** ✓（这是有意的：v2 的连接模型与 v1 不是一回事 ✓）。

⇒ **这件事有一条可执行的判据** ✓（不再靠"读代码觉得一样" ✗）：`V1_REF=<v1.2.x 二进制> ./test.sh <v2 二进制> x`（本地跑，不走 ssh ✓）——
① v1 服务端 ← v2 客户端（两侧 `hs:0`）**必须通** ✓、② v2 服务端 ← v1 客户端（两侧 `hs:0`）**必须通** ✓、③ v2 服务端默认（`hs:1`）← v1 客户端**必须不通** ✓。`tools/check.sh` 的第 5 步就是它 ✓。

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

`build.sh` 的编译参数是 `-Wall -Werror -Wextra -Wunused-macros`（**警告即错误**：多余参数、隐式落空、未用宏都当错 ✓），
并默认带上 **dev / 判据口径**的两个功能宏：`-DFFRDP_DIAG=1`（保留完整诊断 —— `ffrdp_dump` 与只给它用的计数器 ✓）与
`-DFFRDP_FEC_ENABLE=1`（FEC 全功能 ✓）；**出货构建不传这两个宏** ✗ ⇒ dump 子系统、诊断计数器与 FEC 收发缓冲整块编译掉 ✓。
链接时加 `-lpthread`（Windows/MinGW 目标额外加 `-lws2_32`）。`-DLINUX` 由 `build.sh` 在非 mingw 目标上自动加 ✓ —— 少了它，`socket.c` / `utils.c` / `lowlevel-udp.c` 的实现会被 `#ifdef LINUX` 挡掉 ⇒ 编出来一堆空函数 ✓。

不想用脚本时的最小手工编译：

```bash
gcc -Wall -Werror -DLINUX -c ffrdp.c        -o ffrdp.o
gcc -Wall -Werror -DLINUX -c lowlevel-udp.c -o lowlevel-udp.o
gcc -Wall -Werror -DLINUX -c socket.c       -o socket.o
gcc -Wall -Werror -DLINUX -c utils.c        -o utils.o
ar  rcs libffrdp.a ffrdp.o lowlevel-udp.o socket.o utils.o
gcc -Wall -Werror -DLINUX test.c libffrdp.a -lpthread -o test_ffrdp
```

> ⚠️ 上面这条是**最小 / 出货形状**的编译 ✓ —— 没有 `-DFFRDP_DIAG=1` / `-DFFRDP_FEC_ENABLE=1` ⇒ 诊断与 FEC 两样都被编译掉 ✓
> （这是**另一种合法口径** ✓）。要与 `build.sh` 的产出逐字一致 ✓，请直接用 `build.sh`，或自行补上那两个功能宏与加严的告警开关 ✓。

---

## 四、测试程序 test_ffrdp

`test_ffrdp` 在两个线程里各跑一个 ffrdp 实例（服务端 / 客户端共用同一段参数化的对等端循环 ✓），双方**不停互发随机长度的数据**，
每 10 秒自动 `ffrdp_dump()` 一次，用来观察延迟、重传、在途量等指标。**服务端按 `FFRDP_ACCEPT` 事件接纳多个客户端** ✓（一条连接一个句柄 ✓），每个客户端的字节流各自独立校验 ✓。

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
[client] verify: OK -- received 285 messages, seq contiguous and crc all correct
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
- **退出请用 `quit` / `exit`** ✓ —— 只有这种方式会**先打印上面的 `verify:` 汇总**再退出 ✓（`FFRDP_CLOSE=1` 时还会先走一遍 `ffrdp_close()` 的**四次挥手** ✓）；
- 内部固定 `"ffrdps://<ip:port>,mss:1280,fec:0"`（服务端）/ `"ffrdpc://<ip:port>,mss:1280,fec:0"`（客户端）
  ⇒ **MSS 1280 字节、FEC 冗余度 0（关闭）** ✓，需要别的组合请改 `test.c` ✓；
- **三个环境开关**（**都不设 ⇒ 与以前完全一样** ✓）：`FFRDP_HS=0` 关掉握手、走 v1 那条老路 ✓；
  `FFRDP_IDLE=<秒>` 让应用**一个字节都不发** ⇒ 专验"空闲不被误杀" ✓；`FFRDP_CLOSE=1` 退出前用 `ffrdp_close()` 主动关 ⇒ 验**四次挥手** ✓。

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
./test.sh <本地二进制> <user@host> [密码] [时长秒]     # 第 1 个参数：相对 / 绝对 / 裸名字都行 ✓
export SESSION=z5-run1        # 一批扫描共用一个标识，便于成组比较（等价于 STAMP）
./test.sh dist/test_ffrdp_w5a4 chenk@192.168.0.213 abc 120
column -s, -t logs/summary.csv | tail -10     # 摘要表
```

- 没装 `sshpass` 时它会开一条 **SSH 复用连接** ⇒ 整轮只输一次密码 ✓（`KEEP_MUX=1` 可连跑多轮只输一次 ✓）；
- **两侧不同版本**做交叉对照：`REMOTE_BIN=<对端二进制> ./test.sh <本端二进制> <user@host> …` ✓

**一条命令跑完全部判据**（改协议后先跑它 ✓，尤其适合性能优化那种改动 ✓）：

```bash
bash tools/check.sh              # 编译 + 仿真(14 测试/3 场景组) + ASAN + 本机两进程四条路径 + v1 互通
QUICK=1 bash tools/check.sh      # 跳过 ASAN 与 e2e（约 1 分钟）✓
```
- 六步：真机口径 `build.sh` ✓、仿真默认模型（HEAD 与工作区逐条对照 ✓）、仿真 **ASAN** ✓、
  本机两进程（默认 / `FFRDP_HS=0` 老路径 / `FFRDP_IDLE=1 FFRDP_CLOSE=1` 空闲+挥手 / 多客户端 ✓）、
  **v1 互通**（`hs:0` 两个方向必须通、默认 `hs:1` 必须不通 ✓）；
- 全绿 exit 0，任何一项红 exit 1 ⇒ 可直接挂 `pre-push` 或流水线 ✓；日志在 `logs/check-<时间戳>/` ✓。
  （本地仍用第 1 个参数，远端用 `REMOTE_BIN` ✓ —— 用来分辨"某个方向被掐"是本地发送还是对端确认的问题 ✓）；
- 其它开关：`DUR` / `PORT` / `LOGDIR` ✓、`CLIENTS=N`（本地起 N 个客户端打同一个服务端 ⇒ 验多客户端 ✓；上限 = 服务端 `maxconn` − 1 ✓）、`CPUWATCH=1`（采样客户端 CPU ✓）、`PINGWATCH=1`（整轮 1 秒一次 ping 对端 ⇒ 把链路本身量成 min/avg/max + 丢包 ✓，用来分辨"退化是代码还是链路"✓）、`CLEAN=1`（跑完清远端 ✓）、`DRY=1`（只打印计划、不碰网络 ✓）、`FFRDP_IDLE=1` / `FFRDP_CLOSE=1`（空闲与挥手验收 ✓）、`V1_REF=<v1 二进制>`（本地跑 v2 ↔ v1.2.x 互通矩阵 ✓，见 §二的兼容性）；
- 每轮结果追加进 **`$LOGDIR/summary.csv`** ✓（`stamp,tag,role,resend_pct,waste_pct,rtts_ms,send_recv_MBps` ✓）⇒ 扫完一串二进制直接看表 ✓；
- 读判据请对着协议文档：`rttm/rtts`、`counter_resend_ratio`、`confirmed rate`、`inflight, cap`、`ack latency`
  —— 见 [`docs/ffrdp-v1.md`](docs/ffrdp-v1.md) §3.2.4 ✓。⚠️ **dump 的字段名跨版本变过** ✗（例如 v1.2.0 曾有过的
  `lap period avgmax` 已在代码瘦身时删掉 ✗）⇒ 认字段之前先对一下手上那版 dump 的实际输出 ✓。

---

## 五、目录结构

```
.
├── ffrdp.c                 协议实现（单文件：ARQ + FEC + 流控/并发控制 + 诊断）
├── ffrdp.h                 对外 API（v2.0.0：**8 个函数**，见下 —— 句柄式收发 + 事件回调）
├── lowlevel.h              lowlevel 接口（tx + rx + timer，自带工作线程）—— v1.2.0 起
├── lowlevel-udp.c          lowlevel 的 UDP 实现（绑定 + 收发 + timer + 唤醒 socket）
├── socket.c / socket.h     socket 薄封装（初始化、选项、超时、非阻塞）
├── utils.c / utils.h       时间戳、线程 setname/getname、模块线程状态机
├── test.c                  测试程序（服务端/客户端共用一个参数化的对等端循环 + 每 10 秒 dump）
├── ffcrypto.c / ffcrypto.h 给 ENCRYPT/DECRYPT 回调用的**参考加密实现**（ChaCha20，零依赖；刻意**不进** libffrdp —— 见 [`docs/ffrdp-crypto.md`](docs/ffrdp-crypto.md)）
├── test.sh                 **真机 A/B 对测脚本**（上传 → 远端服务端 → 本地客户端 → 收日志 → 出摘要；用法见 §四）
├── build.sh                构建脚本（静态库 + 测试程序 + pkg-config）
├── readme.md               本文件的**英文版**（GitHub 默认显示 ✓；与中文版同步维护 ✓）
├── readme.zh.md            本文件（中文）：概述、版本、编译、测试、目录
├── LICENSE
├── docs/
│   ├── ffrdp-v1.md   协议说明：v1.0.0 / v1.1.0 / v1.2.0 三部分（帧格式、参数、改动与依据）
│   ├── ffrdp-v2.md    v2.0.0 说明：接口（params / 句柄 / 事件）、握手与四次挥手、多客户端、加密回调、keepalive；§九 v2.x 相对 v1.1.x 的协议改进
│   ├── ffrdp-crypto.md     加密接入指南：ENCRYPT/DECRYPT 契约与取舍、内置 ChaCha20 方案（ffcrypto）与上层写法、TLS 架在 ffrdp 之上（OpenSSL）、test.c 的 `--key` 怎么编怎么跑
│   ├── sim-test.md         差分仿真工具（tools/sim）使用说明
│   ├── ffrdp-latency.md 音视频延迟预算与调优：公式、慢/快 AP 的数字、丢包代价、MTU/分片、怎么量
│   └── notes.md            开发与调试记录（按时间顺序，含每次上机的原始 log）
└── tools/                  辅助工具（不逐个列 ✓：**一条命令跑完判据** `tools/check.sh`、**编译期内存账** `tools/size.sh`、差分仿真 `tools/sim/`、敌意输入、在途上限扫描、lowlevel 契约测试、日志提取 —— 以目录实际内容为准 ✓）
```

### 对外 API（`ffrdp.h`）—— **v2.0.0** ✓

```c
void* ffrdp_init (void *params, FFRDP_CALLBACK callback, void *cbctx);   // params 见下（角色 + 配置都在串里）
void  ffrdp_exit (void *ctxt);                                          // 关掉全部连接并释放（内部 join 传输层线程）
int   ffrdp_send (void *ctxt, int conn, char *buf, int len, int timeout_ms);  // 原子：len = 整条接受；0 = 已断；-2 = 队列满且等不到
int   ffrdp_recv (void *ctxt, int conn, char *buf, int len, int timeout_ms);  // 有序可靠**字节流**（无消息边界），有数据就返回
void  ffrdp_flush(void *ctxt, int conn);                                // 把未填满的那一帧立刻发出（conn < 0 = 全部连接）
void  ffrdp_dump (void *ctxt, int conn, int clearhistory);              // conn < 0 = 总状态；conn >= 0 = 该连接的详细状态
int   ffrdp_isdead(void *ctxt, int conn);                               // **这条连接**是否已被判定失联
int   ffrdp_close(void *ctxt, int conn);                                // 主动关：发 FIN，收到对端 FIN 才报 DISCONN（v2 新增 ✓）
```

```text
params（字符串配置，解析失败返回 NULL 并把原因打出来 ✓）：
  "ffrdps://0.0.0.0:8000"                                   服务器（bind 到该地址）
  "ffrdpc://192.168.0.213:8000"                             客户端（连该地址）
  "ffrdps://0.0.0.0:8000,maxconn:10"                        最大连接数（默认 16，上限 FFRDP_MAXCONN）
  "ffrdps://0.0.0.0:8000,handshake_ms:10000,idle_ms:5000"   握手超时 / 空闲(keepalive)超时
  ",mss:1280,fec:0"                                         本端 MSS 与 FEC 冗余（v1 的 smss / sfec）
  ",hs:0"                                                   关掉握手 ⇒ 走 v1 那条老路（与 v1.x 互通时用）
⚠️ 时间类键名**自带单位**（`_ms` ✓）—— 与 `timeout_ms` 同一套风格，不留"配置是秒、内部是毫秒"的换算点 ✗。
```

- **句柄式** ✓：`conn` 是连接句柄（服务端在 `FFRDP_ACCEPT` 事件里拿到 ✓），只当**不透明 int** 用 ✓；**槽位回收、句柄不复活** ✓ ⇒ 陈旧句柄查表失败（`FFRDP_CONN_INVALID` / `isdead` = 1 ✓），不会打到新连接上 ✓。
- **事件回调** ✓：`FFRDP_ACCEPT` / `CONNECT` / `DISCONN` / `ENCRYPT` / `DECRYPT` / `RECVDATA` —— 语义、回调返回值（`FFRDP_CONSUMED` / `NOT_CONSUMED`）与加密约定见 [`docs/ffrdp-v2.md`](docs/ffrdp-v2.md) ✓。
- **返回码** ✓：`> 0` = 字节数 ✓、`FFRDP_CLOSED(0)` = 对端已断 ✓、`FFRDP_EAGAIN(-2)` = 暂时没有 / 等不到 ✓、`FFRDP_CONN_INVALID(-1)` = 句柄无效（含**已关掉的**句柄 ✓）。
- **`timeout_ms`** ✓：`0` = 不等待（旧行为 ✓）、`> 0` = 最多等这么多毫秒 ✓、`< 0` = 一直等到能收 / 能发为止 ✓。
- **握手默认开** ✓（`hs:0` 关掉 ⇒ v1 行为 ✓）；**空闲探活**由 `idle_ms` 决定 ✓（默认 5000 ms ✓）。

> **旧接口留档（`v1.2.x` 分支）** ✓ —— 下面这份只在 v1.2.x 分支上是当前接口 ✓；v2 的 `ffrdp.h` 里已经没有它了 ✗。
> 三条签名差异（`ffrdp_init` 改收地址串、`send` / `recv` 多一个 `timeout_ms`、`ffrdp_update` 删除）见 [`docs/ffrdp-v1.md`](docs/ffrdp-v1.md) §3.1.6 ✓。

```c
void* ffrdp_init  (char *addr, char *txkey, char *rxkey, int server, int smss, int sfec);   // addr = "ip:port" / "ip"（端口默认 8000）
void  ffrdp_free  (void *ctxt);
int   ffrdp_send  (void *ctxt, char *buf, int len, int timeout_ms);  // 原子：len = 整条接受；-1 = 一个字节都没接受
int   ffrdp_recv  (void *ctxt, char *buf, int len, int timeout_ms);  // 取应用层数据（有序可靠**字节流**，无消息边界）
int   ffrdp_isdead(void *ctxt);                       // 对端是否已失联
void  ffrdp_flush (void *ctxt);                       // 把未填满的帧立刻发出（不要攒着）
void  ffrdp_dump  (void *ctxt, int clearhistory);     // 打印诊断
```

---

## 六、文档索引

| 想了解 | 看这里 |
|---|---|
| 帧格式、mack、FEC、RTO 计算、通信流程 | [`docs/ffrdp-v1.md`](docs/ffrdp-v1.md) 第一部分（v1.0.0）|
| 每个改动的动机、被否掉的路线、当时的上机原始 log | [`docs/notes.md`](docs/notes.md) |
| 没有真机时怎么验证"没有回归" | [`docs/sim-test.md`](docs/sim-test.md) |
| **一条命令跑完所有判据**（编译 / 仿真 / ASAN / 本机两进程四条路径 / v1 互通）| `tools/check.sh`（用法见 §四）|
| 传输层（收发包 / 定时器 / 唤醒）的契约测试 | `tools/test-lowlevel-udp.sh`（用法见 §四）|
| v2.0.0 的接口（params / 句柄 / 事件 / 加密回调 / 握手与挥手 / keepalive）| [`docs/ffrdp-v2.md`](docs/ffrdp-v2.md) |
| v2.0.0 的设计取舍与边界条件（连接表 / 句柄代号与槽位回收 / 半开上限 / 加密约定）| [`docs/ffrdp-v2.md`](docs/ffrdp-v2.md) |
| **加密怎么接**：`ENCRYPT`/`DECRYPT` 契约与它排除了什么、内置 ChaCha20 方案（`ffcrypto`：盐记录、句柄不是下标）、TLS 架在 ffrdp 之上（OpenSSL 代码骨架）、`test.c --key`（编译 / 命令行 / 期望输出 / 重连回归）| [`docs/ffrdp-crypto.md`](docs/ffrdp-crypto.md) |
| **音视频（H.265 / VBR）延迟预算与调优**：延迟公式、慢/快 AP 的数字、丢包代价、MTU 与 IP 分片、怎么量 | [`docs/ffrdp-latency.md`](docs/ffrdp-latency.md) |
| **v2.x 相对 v1.1.x 的协议改进**（含验收口径、已知边界、已否路线与仍开放方向）| [`docs/ffrdp-v2.md`](docs/ffrdp-v2.md) §九 |

---

## 七、作者

rockcarry@163.com
