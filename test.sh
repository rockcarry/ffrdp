#!/usr/bin/env bash
# test.sh —— 一条命令跑完一轮真机对测（上传 → 服务端 → 本地客户端 → 收回日志 → 出摘要）
#
#   用法:  ./test.sh <本地二进制> <user@host> [密码] [时长秒]
#   例子:  ./test.sh dist/test_ffrdp_cap16 chenk@192.168.0.213 abc 120
#
#   第 1 个参数（本地二进制）可以是**相对路径 / 绝对路径 / 裸名字**（如 test_ffrdp）✓ —— 脚本
#   会先把它转成绝对路径 ✓。必须如此：后面用 env / stdbuf 执行它，而 stdbuf 收到**不含 "/" 的
#   名字**时会去 PATH 里找 ⇒ 直接写 "./test.sh test_ffrdp …" 会因为
#   "stdbuf: failed to run command 'test_ffrdp': No such file or directory"（退出码 127）
#   而整轮失败 ✗（症状是"没连上、日志里没有 verify 行"）。REMOTE_BIN / V1_REF 两个路径同理 ✓。
#
#   第 3 个参数（密码）只有当本机装了 sshpass 时才会被用上；没装的话脚本会开一条
#   SSH 复用连接，整个过程（scp 上行 + 远端起服务 + scp 下行）只让你输一次密码。
#
#   可调环境变量:
#     DUR=120        测试时长（秒），也可用第 4 个参数
#     PORT=8111      端口
#     LOGDIR=./logs  本地日志目录（跑完在这里写 summary.csv，可直接 column -s, -t 看表）
#     SESSION=xxx    给这一轮/这一批一个标识（等价于 STAMP；扫一串二进制时用它成组比较）
#     REMOTE_BIN=…   远端用另一个二进制（本地仍用第 1 个参数）—— 做"两侧不同版本"的交叉对照
#     FFRDP_HS=0/1   透传给**两侧**（v2 二进制用它强制老路径/握手；v1 二进制不认 ⇒ 无影响）
#     FFRDP_IDLE=1   空闲验收：应用**一个字节都不发**，看 keepalive 会不会误杀（本地继承 / 远端 ssh 显式传 ✓）
#     FFRDP_CLOSE=1  退出前 ffrdp_close ⇒ 走一遍四次挥手（FIN 往返 ⇒ 两侧各自 DISCONN ✓）
#     TEST_STAMP=1   逐消息延迟样本（A1）。**两端都必须开** ⇒ 这里会同时透传给远端服务端 ✓，
#                    缺一侧的话时间戳会被当载荷读出垃圾（日志里 lat_bad 计数非 0 ✗）。
#     TEST_APP_BURST=N 应用侧每次 recv 处理 N 条（同上，两端一起透传；默认 0 = 旧行为 ✓）。
#     TEST_PEER_SILENCE_MS=N 应用层多久收不到字节就判对端失联并重连（同上，两端一起透传；默认 2000）。
#     SPEED=N        应用侧发送限速 KB/s（**每连接**，两端一起透传 ⇒ 两侧都限；默认 0 = 不限速 ✓）。
#                    用途：分辨"协议/窗口算法不行"和"应用灌太猛把链路队列顶满"——
#                    限速后若各档都不再重连，根因就是排队自激，不是拥塞窗口逻辑 ✗。
#                     ⚠️ 200 ms 级链路建议给 10000：2 s 太紧，一次长静默就会重连，把长跑切成两段（口径随之错位）。
#     UP_TOTAL=N     客户端**总上行** KB/s（按 CLIENTS 均分；0 = 用 SPEED）。限速到"链路容量以内"必须
#     DOWN_TOTAL=N   用总量表达：容量是一个数（真机实测 上行 290~340KB/s / 下行 680~780KB/s，随时段与
#                    AP 而异），而 SPEED 是每进程的
#                    ⇒ 15 个客户端各自 100KB/s 时实际总供给 1.5MB/s = 5× 超卖（1019-122211 的崩溃来源 ✓）。
#                    两者分开是因为真链路不对称；只给一侧时另一侧保持 SPEED 的行为 ✓。
#                    例：UP_TOTAL=300 DOWN_TOTAL=780 CLIENTS=15 ⇒ 每连接 20/52 KB/s。
#     CLIENTS=N      本地起 N 个客户端打同一个服务端（默认 1 ✓；上限 = 服务端 maxconn - 1 ✓）
#                    日志：第 1 个是 <tag>_client_<stamp>.log ✓、第 2..N 个是它加 _c2.._cN ✓；
#                    互通自测那几条用 ASCII 名（v1ref_v1srv_v2cli_hs0_* ✓），漂亮的 ①②③ 只在终端 ✓
#     CPUWATCH=1     同时采样本地客户端进程的 CPU（单核百分比），写进 *.cpu
#     PINGWATCH=1    同时量**链路本身**：整轮 1 秒一次 ping 对端，写 min/avg/max + 丢包到 *.ping
#                    （真机每轮之间能差 100 倍 ✗：2026-10-15 实测同一份二进制 rtts 12ms ↔ 441ms、
#                     而当时 ping 对端就是 375/441/508ms ⇒ 没有这条读数就分不清"代码还是链路" ✓）
#     KEEP_MUX=1     跑完不关 SSH 复用连接（连跑多个二进制时少输几次密码）
#     CLEAN=1        跑完删掉远端 /tmp 里的二进制与日志
#     DRY=1          只打印将要做什么，不碰网络（用来检查参数/语法）
#
#   互通自检模式（**本地**跑，不走 ssh ✓）：
#     V1_REF=<v1 二进制>   自动跑一轮"v2 ↔ v1.2.x"互通矩阵，把兼容性变成例行判据 ✓：
#                          ① v1 服务端 ← v2 客户端（两侧 hs:0）⇒ 必须通 ✓
#                          ② v2 服务端 ← v1 客户端（两侧 hs:0）⇒ 必须通 ✓
#                          ③ v2 服务端默认(hs:1) ← v1 客户端 ⇒ 必须**不通**（这就是"默认不互通" ✓）
#                          用法: V1_REF=/tmp/v1wt/test_ffrdp ./test.sh dist/test_ffrdp_v2fix x 2>/dev/null
#
#   依赖: ssh / scp（必需）、nc、md5sum、timeout、stdbuf；sshpass 与 column 可选；
#         远端需要 pgrep / pkill / wc。
set -u

BIN="${1:-}"; TARGET="${2:-}"; PASS="${3:-}"; DUR="${4:-${DUR:-120}}"
PORT="${PORT:-8111}"
LOGDIR="${LOGDIR:-$PWD/logs}"
# STAMP 决定日志与 CSV 的每轮标识。一次扫描想共享一个标识（便于成组比较）就设 SESSION=xxx。
STAMP="${STAMP:-${SESSION:-$(date +%m%d-%H%M%S)}}"
DRY="${DRY:-0}"; CLEAN="${CLEAN:-0}"; PINGWATCH="${PINGWATCH:-0}"

# 把二进制路径转成**绝对路径**再往下用 ✓ —— 后面所有执行都过 env / stdbuf，
# 而 stdbuf 对不含 "/" 的名字会走 PATH 查找 ⇒ 裸名字（test_ffrdp）会 127 失败 ✗。
# 转一次，./x、x、/abs/x、符号链接就都等价了 ✓。
abspath() {
    case "$1" in
        /*) printf '%s\n' "$1" ;;
        *)  printf '%s/%s\n' "$(cd "$(dirname -- "$1")" && pwd)" "$(basename -- "$1")" ;;
    esac
}

usage() {
    # 只打印开头的注释块：遇到第一条非注释行就停 ✓
    # （不要用固定行号 ✗，也别依赖空行 ✗ —— 注释块里的段落间隔是 "#" 行，不是空行 ✓）
    awk 'NR > 1 { if ($0 !~ /^#/) exit; sub(/^# ?/, ""); print }' "$0" 
    exit 2
}
[ -n "$BIN" ] || usage

# ══ V1_REF：本地互通自检（三条判据 ✓）══════════════════════════════════════════
# 为什么要有它：hs:0 是"与 v1.x 互通"的唯一开关 ✓（默认 hs:1 不互通 ✓，见 docs/ffrdp-v2.md §八）。
# 兼容性靠"读代码觉得一样"是不够的 ✗ —— 这里把它变成可执行的判据 ✓：谁把老路径改坏了，这里立刻红 ✓。
if [ -n "${V1_REF:-}" ]; then
    V1="$V1_REF"; V2="$BIN"
    [ -x "$V1" ] || { echo "✗ V1_REF 不是可执行文件：$V1"; exit 2; }
    [ -x "$V2" ] || { echo "✗ 第 1 个参数（v2 二进制）不是可执行文件：$V2"; exit 2; }
    V1="$(abspath "$V1")"; V2="$(abspath "$V2")"      # ⇒ 后面 stdbuf 执行它们才不会走 PATH ✓
    DUR="${DUR:-20}"; PORT="${PORT:-8111}"
    mkdir -p "$LOGDIR"
    FAILN=0

    # 跑一格：$1=标签 $2=服务端 $3=客户端 $4=两侧 env $5=期望(通/不通) $6=端口
    v1_pair() {
        local tag="$1" slug="$2" sb="$3" cb="$4" envs="${5:-}" want="$6" P="$7" ok=1
        # tag 是**给人看的**（带 ①②③ 与箭头 ✓）；slug 是**给文件名用的** ASCII ✓。
        # 以前直接把 tag 拼进日志名 ⇒ logs/ 下会出现中文 + 箭头 + 双空格的文件名 ✗
        # （ls 出来一片引号，`for f in $(ls logs/*)` 也会断 ✗）。
        pkill -f "test_ffrdp --server=127.0.0.1:$P" 2>/dev/null
        ( sleep $((DUR+4)); echo quit ) | timeout $((DUR+25)) env $envs stdbuf -oL "$sb" --server=127.0.0.1:$P \
            > "$LOGDIR/${STAMP}_v1ref_${slug}_server.log" 2>&1 &
        local sp=$!
        sleep 1
        ( sleep "$DUR"; echo quit ) | timeout $((DUR+20)) env $envs stdbuf -oL "$cb" --client=127.0.0.1:$P \
            > "$LOGDIR/${STAMP}_v1ref_${slug}_client.log" 2>&1
        wait $sp 2>/dev/null
        local cs ss cn
        cs="$(grep -c 'verify: OK' "$LOGDIR/${STAMP}_v1ref_${slug}_client.log" 2>/dev/null || true)"
        ss="$(grep -c 'verify: OK' "$LOGDIR/${STAMP}_v1ref_${slug}_server.log" 2>/dev/null || true)"
        cn="$(grep -o 'recv [0-9]* msgs' "$LOGDIR/${STAMP}_v1ref_${slug}_client.log" 2>/dev/null | head -1 | awk '{print $2}')"
        cn="${cn:-0}"
        if [ "$want" = "通" ]; then
            [ "$cs" = 1 ] && [ "$ss" = 1 ] && [ "$cn" -gt 0 ] || ok=0
        else
            [ "$cs" = 0 ] && [ "$ss" = 0 ] || ok=0     # 期望不通：两侧都不该有 verify: OK ✓
        fi
        printf '  %-44s 期望%-4s cli_OK=%s srv_OK=%s 客户端实收 %-6s 条  %s\n' \
            "$tag" "$want" "$cs" "$ss" "$cn" "$([ "$ok" = 1 ] && echo '✅' || echo '✗')"
        [ "$ok" = 1 ] || FAILN=$((FAILN+1))
        return 0
    }

    echo "互通自检（v2 $V2 ↔ v1 $V1，各 ${DUR}s，本地两进程 ✓）"
    v1_pair "① v1(srv) ← v2(cli)  hs:0"     "v1srv_v2cli_hs0" "$V1" "$V2" "FFRDP_HS=0" "通" "$PORT"
    v1_pair "② v2(srv) ← v1(cli)  hs:0"     "v2srv_v1cli_hs0" "$V2" "$V1" "FFRDP_HS=0" "通" "$((PORT+1))"
    v1_pair "③ v2(srv) 默认hs:1 ← v1(cli)"  "v2srv_hs1_v1cli" "$V2" "$V1" ""           "不通" "$((PORT+2))"
    echo
    if [ "$FAILN" = 0 ]; then
        echo "✅ 互通自检通过：hs:0 两条方向都通 ✓、默认 hs:1 仍然不互通 ✓（日志在 $LOGDIR/${STAMP}_v1ref_*.log）"
        exit 0
    fi
    echo "✗ 互通自检失败 $FAILN 项（看 $LOGDIR/${STAMP}_v1ref_*.log）"
    exit 1
fi
[ -n "$TARGET" ] || usage
[ -f "$BIN" ] || { echo "✗ 找不到文件: $BIN"; exit 2; }
[ -x "$BIN" ] || { echo "✗ 没有可执行权限: $BIN  （chmod +x 一下）"; exit 2; }
BIN="$(abspath "$BIN")"                                  # ⇒ 裸名字也能跑 ✓（见文件头的说明）
case "$DUR" in ''|*[!0-9]*) echo "✗ 时长必须是整数秒: $DUR"; exit 2;; esac
case "$PORT" in ''|*[!0-9]*) echo "✗ 端口必须是数字: $PORT"; exit 2;; esac
SPEED="${SPEED:-0}"        # 应用侧限速 KB/s（**每连接**，两端一起传 --send_speed；0 = 不限速 ✓）
case "$SPEED" in ''|*[!0-9]*) echo "✗ SPEED 必须是整数 KB/s（0 = 不限速）: $SPEED"; exit 2;; esac
# UP_TOTAL / DOWN_TOTAL：**总供给**限速（KB/s），按 CLIENTS 均分到每条连接（0/未设 = 沿用 SPEED）。
# 为什么需要它们：SPEED 是**每进程**的 ⇒ 15 个客户端各自 100KB/s，实际总供给 1.5MB/s，而链路容量
# 只有一个数（真机实测：上行 290~340、下行 680~780KB/s，随时段/AP 而异）⇒ "限速到容量以内"这件事只能按总量表达，
# 否则每改一次 CLIENTS 就要手算一遍，而且很容易又把链路超卖（1019-122211 的 5× 超卖就是这么来的）。
# 两端分开给：真链路不对称（上行 ≈0.3 vs 下行 ≈0.7 MB/s），给同一个数会白占下行带宽。
# 具体每连接速率在下面（CLIENTS 已知之后）由 per_conn_speed 算成 CLI_SPEED_ARG / SRV_SPEED_ARG ✓。
UP_TOTAL="${UP_TOTAL:-0}"
DOWN_TOTAL="${DOWN_TOTAL:-0}"
case "$UP_TOTAL"   in ''|*[!0-9]*) echo "✗ UP_TOTAL 必须是整数 KB/s（0 = 不限）: $UP_TOTAL"; exit 2;; esac
case "$DOWN_TOTAL" in ''|*[!0-9]*) echo "✗ DOWN_TOTAL 必须是整数 KB/s（0 = 不限）: $DOWN_TOTAL"; exit 2;; esac
# 头部那一行的限速描述：用总量模式时，SPEED 往往是 0（"不限"），照原样打印会读成"没限速"✗。
SPEED_DESC="${SPEED}KB/s/连接"
[ "$SPEED" = 0 ] && SPEED_DESC="不限"
if [ "$UP_TOTAL" != 0 ] || [ "$DOWN_TOTAL" != 0 ]; then SPEED_DESC="按总量（见下一行）"; fi

HOST="${TARGET#*@}"
BASE="$(basename "$BIN")"
TAG="${BASE#test_ffrdp_}"; [ -n "$TAG" ] || TAG="$BASE"
# REMOTE_BIN：远端用另一个二进制（本地仍用 $1）。用来做"两侧不同版本"的交叉对照，
# 例如本地 rthole4 + 远端 oldarch，以分辨"某个方向被掐"是本地发送路径还是对端确认路径的问题。
REMOTE_BIN="${REMOTE_BIN:-$BIN}"
[ -f "$REMOTE_BIN" ] || { echo "✗ REMOTE_BIN 不存在: $REMOTE_BIN"; exit 2; }
REMOTE_BIN="$(abspath "$REMOTE_BIN")"
RBASE="$(basename "$REMOTE_BIN")"
RTAG="${RBASE#test_ffrdp_}"; [ -n "$RTAG" ] || RTAG="$RBASE"
[ "$RBASE" = "$BASE" ] || TAG="${TAG}-${RTAG}"
mkdir -p "$LOGDIR"
LOCAL_LOG="$LOGDIR/${TAG}_client_${STAMP}.log"
SRV_LOG="/tmp/${TAG}_server_${STAMP}.log"
LOCAL_SRV_LOG="$LOGDIR/${TAG}_server_${STAMP}.log"

say() { printf '%s  %s\n' "$(date +%H:%M:%S)" "$*"; }

# CPUWATCH=1 时采样客户端进程的 CPU（单核百分比 = 100 表示占满一个核）。
# 用来判断"一个方向跑 0.85 MB/s 时另一个方向掉下去"是不是 CPU 容量问题。
cpu_watch() {   # $1 = timeout 的 pid（客户端是它的子进程），$2 = 输出文件
    local tp="$1" out="$2" pid="" i=0
    local hz; hz="$(getconf CLK_TCK 2>/dev/null || echo 100)"
    while [ "$i" -lt 60 ]; do
        pid="$(pgrep -P "$tp" 2>/dev/null | head -1)"
        [ -n "$pid" ] && break
        sleep 0.2; i=$((i+1))
    done
    if [ -z "$pid" ]; then echo 'no_pid' > "$out"; return; fi
    local T0 T1 S0 S1 t0 t1 s0 s1 dt cur peak=0
    T0="$(awk '{print $14+$15}' /proc/$pid/stat 2>/dev/null)"; T0="${T0:-0}"
    t0="$T0"; S0="$(date +%s%N)"; s0="$S0"
    T1="$T0"; S1="$S0"
    while [ -d "/proc/$pid" ]; do
        sleep 1
        t1="$(awk '{print $14+$15}' /proc/$pid/stat 2>/dev/null)"
        [ -n "$t1" ] || break          # 进程刚好退出：保留上一次的好值，别用空串覆盖
        s1="$(date +%s%N)"
        T1="$t1"; S1="$s1"
        dt=$(( (s1 - s0) / 1000000 ))
        if [ "$dt" -gt 0 ]; then
            cur=$(( (t1 - t0) * 100000 / (hz * dt) ))
            [ "$cur" -gt "$peak" ] && peak=$cur
        fi
        t0="$t1"; s0="$s1"
    done
    dt=$(( (S1 - S0) / 1000000 ))
    cur=0; [ "$dt" -gt 0 ] && cur=$(( (T1 - T0) * 100000 / (hz * dt) ))
    printf 'avg_pct_of_one_core=%s\npeak_pct_of_one_core=%s\n' "$cur" "$peak" > "$out"
}

# PINGWATCH=1 时同时量"链路本身"：整轮按 1 秒间隔 ping 对端，结束从 ping 自己的汇总里抽数字。
# 为什么需要它：真机每轮之间链路能差 100 倍 ✗（2026-10-15：同一份二进制 rtts 12ms ↔ 441ms、
# resend_perMB 0.24 ↔ 51.9 ✗），光看协议数字分不清"代码变了"还是"链路变了" ⇒ 每轮带一条环境读数 ✓。
# 注意它必须**独立于协议**：所以用 ping，而不是用我们自己的统计 ✓。
ping_watch() {   # $1 = 对端地址（不含 user@）$2 = 输出前缀 $3 = 秒数
    command -v ping >/dev/null 2>&1 || return 1
    ping -c "$3" -i 1 -W 2 "$1" > "$2.raw" 2>&1 &
    PINGPID=$!
}
ping_note() {    # $1 = 原始输出 $2 = 摘要输出（key=value，与 *.cpu 同风格 ✓）
    local raw="$1" out="$2" loss rtt
    loss="$(grep -o '[0-9.]*% packet loss' "$raw" 2>/dev/null | tail -1 | cut -d% -f1)"
    rtt="$(grep -o 'rtt min/avg/max[^=]*= *[0-9./]*' "$raw" 2>/dev/null | tail -1 | sed 's/.*= *//')"
    printf 'loss_pct=%s\nrtt_min_avg_max_ms=%s\n' "${loss:-?}" "${rtt:-?}" > "$out"
}

# ---------- ssh / scp 封装 ----------
SSH_OPTS=(-o ConnectTimeout=8 -o StrictHostKeyChecking=accept-new -o ServerAliveInterval=10 -o ServerAliveCountMax=2)
# 复用连接的路径固定（不塞进 $$），这样连跑多个二进制时也只输一次密码；KEEP_MUX=1 时退出也不关它。
CTL="/tmp/.ssh-mux-${USER:-u}-$(echo "$TARGET" | tr -c 'A-Za-z0-9' '_')"
KEEP_MUX="${KEEP_MUX:-0}"
MUX=0
cleanup() {
    if [ "$MUX" = 1 ] && [ "$KEEP_MUX" != 1 ]; then
        ssh -O exit -o ControlPath="$CTL" "$TARGET" >/dev/null 2>&1
    fi
    return 0
}
trap cleanup EXIT

if command -v sshpass >/dev/null 2>&1 && [ -n "$PASS" ]; then
    SSH=(sshpass -p "$PASS" ssh "${SSH_OPTS[@]}")
    SCP=(sshpass -p "$PASS" scp "${SSH_OPTS[@]}")
    AUTH="sshpass"
elif [ "$DRY" = 1 ]; then
    SSH=(ssh "${SSH_OPTS[@]}"); SCP=(scp "${SSH_OPTS[@]}"); AUTH="dry"
else
    AUTH="复用连接（只输一次密码）"
    # 先看已有复用连接是否还活着：KEEP_MUX=1 留下的 socket 常常已经死了，
    # 这时直接复用会让 ssh 先卡到 timeout（实测 60 秒）再回落去问密码。
    if ssh -O check -o ControlPath="$CTL" "$TARGET" >/dev/null 2>&1; then
        say "· 复用连接还活着，直接复用（不会再问密码）"
    else
        rm -f "$CTL"
        say "· 未装 sshpass ⇒ 打开复用连接，接下来只会让你输一次密码"
        if ! ssh -o ControlMaster=auto -o ControlPath="$CTL" -o ControlPersist=900 "${SSH_OPTS[@]}" -N -f "$TARGET"; then
            echo "✗ 无法建立到 $TARGET 的 SSH 复用连接（网络/密码/主机密钥？）"; exit 3
        fi
    fi
    MUX=1
    SSH=(ssh -o ControlPath="$CTL" "${SSH_OPTS[@]}")
    SCP=(scp -o ControlPath="$CTL" "${SSH_OPTS[@]}")
fi

echo "──────────────────────────────────────────────────────────"
echo " 本地端 : $BIN   (md5 $(md5sum "$BIN" | awk '{print $1}'))"
echo " 远端   : $REMOTE_BIN   (md5 $(md5sum "$REMOTE_BIN" | awk '{print $1}'))"
echo " 标签   : $TAG      时长 ${DUR}s   端口 $PORT   应用限速 $SPEED_DESC"
echo " 服务端 : $TARGET:/tmp/$RBASE"
echo " 客户端 : 本机 → $HOST:$PORT"
echo " 日志   : $LOGDIR"
echo " 认证   : $AUTH"
echo "──────────────────────────────────────────────────────────"

# ── 多客户端支持：CLIENTS=N（默认 1 ⇒ 与以前完全一样 ✓）──────────────────────
# test.c 的**服务端**现在一条 accept 连接起一个 link 线程 ⇒ 可以同时服务 N 个客户端 ✓
# （协议侧本来就支持：maxconn 默认 16，句柄 = 下标 ✓）。
# ⚠️ N 的上限是服务端 maxconn 减一（内嵌主连接占一个槽 ✓）。
CLIENTS="${CLIENTS:-1}"
case "$CLIENTS" in *[!0-9]*|"") echo "✗ CLIENTS 必须是正整数（现在是 "$CLIENTS"）"; exit 2;; esac
[ "$CLIENTS" -ge 1 ] || { echo "✗ CLIENTS 必须 ≥ 1"; exit 2; }

# ── 总量限速 ⇒ 每连接速率（UP_TOTAL 管客户端**发**、DOWN_TOTAL 管服务端**发**）──────
# 均分到 1KB/s 粒度（四舍五入），下限 1（0 会让 test.c 当成"不限速" ⇒ 超卖回来 ✗）。
per_conn_speed() {   # $1 = 总量 KB/s（0 = 沿用 SPEED 的每连接值）
    local tot="$1" v
    if [ "$tot" = 0 ]; then echo "$SPEED"; return; fi
    v=$(( (tot + CLIENTS / 2) / CLIENTS ))
    [ "$v" -lt 1 ] && v=1
    echo "$v"
}
CLI_SPEED="$(per_conn_speed "$UP_TOTAL")"     # 客户端每条连接发多快 ⇒ 决定上行总量
SRV_SPEED="$(per_conn_speed "$DOWN_TOTAL")"   # 服务端每条连接发多快 ⇒ 决定下行总量
CLI_SPEED_ARG=""; [ "$CLI_SPEED" != 0 ] && CLI_SPEED_ARG="--send_speed=$CLI_SPEED"
SRV_SPEED_ARG=""; [ "$SRV_SPEED" != 0 ] && SRV_SPEED_ARG="--send_speed=$SRV_SPEED"
if [ "$UP_TOTAL" != 0 ] || [ "$DOWN_TOTAL" != 0 ]; then
    echo " 限速总量: 上行 ${UP_TOTAL}KB/s"\
         "⇒ 每连接 ${CLI_SPEED}KB/s（${CLIENTS} 条 = $((CLI_SPEED * CLIENTS))KB/s）|"\
         "下行 ${DOWN_TOTAL}KB/s ⇒ 每连接 ${SRV_SPEED}KB/s（= $((SRV_SPEED * CLIENTS))KB/s）"
fi

# ── 服务端要活多久：必须**比最后一个客户端晚几秒** ✓ ────────────────────────────
# 客户端是串行拉起的（间隔 0.4s，见 [4/6] 的 `sleep 0.4`）⇒ 第 N 个客户端结束于
#     起客户端 + (N-1)×0.4s + DUR
# 而服务端原来只活 DUR+8 ⇒ CLIENTS ≥ 11 时最后几个客户端会比服务端活得久 ✗：
# 服务端关机时发的 FIN 就被它们判成 "server lost !"，随后重建一个空上下文（摘要里也多一行
# "2 个上下文"）。1018-184324 / 185645 / 190533 三连都是这样，每次都恰好是 c12~c15 ——
# 因为 0.4×11 = 4.4s 已经吃掉了 8s 余量的大半。
# 所以余量跟着客户端数走：8s + 串行拉起的总时长（0.4s × CLIENTS，向上取整）。
SRV_HOLD=$(( DUR + 8 + (CLIENTS * 4 + 9) / 10 ))
client_log()  {   # $1 = 客户端序号 ⇒ 第 1 个保持老日志名 ✓（脚本/习惯都不用改 ✓）
    if [ "$1" = 1 ]; then echo "$LOCAL_LOG"; else echo "${LOCAL_LOG%.log}_c$1.log"; fi
}
client_logs_hint() {   # 第 1 个客户端**保持不带后缀**的日志名（兼容老习惯 ✓），第 2..N 个带 _cN ✓
    if [ "$CLIENTS" = 1 ]; then echo "$LOCAL_LOG"
    else echo "${LOCAL_LOG##*/}（第 1 个）+ ${LOCAL_LOG##*/} 的 _c2.._c$CLIENTS（第 2..N 个）"
    fi
}

say "[1/6] 探路 $HOST:22"
if [ "$DRY" = 1 ]; then
    say "      (DRY) 跳过"
elif ! nc -z -w 6 "$HOST" 22; then
    echo "✗ 连不上 $HOST:22 —— 是不是不在那个 AP 上，或服务器没开机？"; exit 3
else
    say "      通"
fi

say "[2/6] 上传二进制并核对 md5（远端用 $(basename "$REMOTE_BIN")）"
if [ "$DRY" != 1 ]; then
    "${SCP[@]}" -q "$REMOTE_BIN" "$TARGET:/tmp/$RBASE" || { echo "✗ scp 上传失败"; exit 4; }
    LMD5="$(md5sum "$REMOTE_BIN" | awk '{print $1}')"
    RMD5="$("${SSH[@]}" "$TARGET" "chmod +x '/tmp/$RBASE'; md5sum '/tmp/$RBASE'" | awk '{print $1}')"
    [ "$LMD5" = "$RMD5" ] || { echo "✗ 两端 md5 不一致：本地 $LMD5 / 远端 $RMD5"; exit 4; }
    say "      一致: $LMD5"
fi

say "[3/6] 远端起服务端（${SRV_HOLD}s 后自动 quit = ${DUR}s + 8s 余量 + ${CLIENTS} 个客户端的串行拉起时间）"
SSHPID=0
if [ "$DRY" != 1 ]; then
    # 注意 pkill 的写法：模式里的路径写成 [/]tmp 这样带一对方括号。
    # 否则这条 ssh 包装命令自己的命令行里就含 "/tmp/xxx --server" 这个串，pkill -f 会连它一起杀掉，
    # 后面的启动步骤一行都不会执行（实测：ssh.out 是 0 字节、远端日志根本没生成、客户端连了个空端口）。
    "${SSH[@]}" "$TARGET" "pkill -f '[/]tmp/$RBASE --server' 2>/dev/null; sleep 0.3; rm -f '$SRV_LOG'; ( sleep $SRV_HOLD; echo quit ) | timeout $((SRV_HOLD+17)) env FFRDP_HS='${FFRDP_HS:-}' FFRDP_IDLE='${FFRDP_IDLE:-}' FFRDP_CLOSE='${FFRDP_CLOSE:-}' TEST_STAMP='${TEST_STAMP:-}' TEST_APP_BURST='${TEST_APP_BURST:-}' TEST_PEER_SILENCE_MS='${TEST_PEER_SILENCE_MS:-}' stdbuf -oL '/tmp/$RBASE' --server=0.0.0.0:$PORT $SRV_SPEED_ARG > '$SRV_LOG' 2>&1; echo REMOTE_EXIT=\$?; wc -c < '$SRV_LOG'" \
        > "$LOGDIR/${TAG}_server_${STAMP}.ssh.out" 2>&1 &
    SSHPID=$!
    sleep 4
    PROBE="$("${SSH[@]}" "$TARGET" "pgrep -f '[/]tmp/$RBASE --server' >/dev/null && echo RUNNING || echo NOTRUNNING" 2>&1)"
    case "$PROBE" in
        *RUNNING*) say "      已拉起并确认在跑（远端日志 $SRV_LOG）" ;;
        *)  echo "✗ 服务端没起来（$PROBE）—— 现在停手，别白等 ${DUR}s"
            echo "  远端输出（本地文件 $LOGDIR/${TAG}_server_${STAMP}.ssh.out）："
            sed 's/^/    /' "$LOGDIR/${TAG}_server_${STAMP}.ssh.out" 2>/dev/null | head -12
            kill "$SSHPID" 2>/dev/null
            exit 5 ;;
    esac
fi

say "[4/6] 本地起客户端（${DUR}s × ${CLIENTS} 个）"
CPUF="$LOGDIR/${TAG}_client_${STAMP}.cpu"
PINGP="$LOGDIR/${TAG}_ping_${STAMP}.txt"
if [ "$PINGWATCH" = 1 ] && [ "$DRY" != 1 ]; then
    ping_watch "$HOST" "$PINGP" "$((DUR+5))" && say "      PINGWATCH：同时对 $HOST 打 $((DUR+5)) 发（写 $PINGP）"
fi
if [ "$DRY" = 1 ]; then
    say "      (DRY) ( sleep $DUR; echo quit ) | stdbuf -oL $BIN --client=$HOST:$PORT $CLI_SPEED_ARG > $LOCAL_LOG   ×${CLIENTS}"
else
    CPIDS=""
    c=1
    while [ "$c" -le "$CLIENTS" ]; do
        LOG_C="$(client_log "$c")"
        # 注意：这里是**普通脚本行**，必须用双引号 —— 单引号不会展开 ⇒ env 会拿到字面量
        # "${TEST_STAMP:-}"、atoi 得 0 ⇒ 客户端等于没开 TEST_STAMP（症状：crc_err 全量 ✗）。
        # 远端那一行不同：它嵌在 ssh 的双引号命令串里，单引号内的 ${…} 仍由本地展开 ✓。
        ( sleep "$DUR"; echo quit ) | timeout $((DUR+15)) env TEST_STAMP="${TEST_STAMP:-}" TEST_APP_BURST="${TEST_APP_BURST:-}" TEST_PEER_SILENCE_MS="${TEST_PEER_SILENCE_MS:-}" stdbuf -oL "$BIN" --client="$HOST:$PORT" $CLI_SPEED_ARG > "$LOG_C" 2>&1 &
        CPIDS="$CPIDS $!"
        [ "$c" = 1 ] && TPID=$!
        sleep 0.4          # 让它们依次连上（也免得 N 个 SYN 挤在同一毫秒 ✓）
        c=$((c+1))
    done
    if [ "${CPUWATCH:-0}" = 1 ]; then
        cpu_watch "$TPID" "$CPUF" &
    fi
    RC=0
    for p in $CPIDS; do
        wait "$p" 2>/dev/null || RC=$?
    done
    say "      ${CLIENTS} 个客户端都结束（最后一个退出码 $RC）→ $(client_logs_hint)"
fi

if [ "$PINGWATCH" = 1 ] && [ "$DRY" != 1 ] && [ -n "${PINGPID:-}" ]; then
    wait "$PINGPID" 2>/dev/null
    ping_note "${PINGP}.raw" "$PINGP"
    say "      链路读数 → $PINGP（$(tr '\n' ' ' < "$PINGP")）"
fi

say "[5/6] 等服务端收尾，把日志取回本地"
if [ "$DRY" != 1 ]; then
    # 不要只等那个 ssh 作业：mux 出岔子时它会早早退出，于是我们在服务端还在跑的时候就把日志抓走
    # （表现为服务端日志没有 verify 行）。改成直接问远端进程在不在，直到它消失为止。
    waited=0
    while [ "$waited" -lt $((DUR+90)) ]; do
        if "${SSH[@]}" "$TARGET" "pgrep -f '[/]tmp/$RBASE --server' >/dev/null" >/dev/null 2>&1; then
            sleep 3; waited=$((waited+3))
        else
            break
        fi
    done
    say "      服务端已退出（等了 ${waited}s）"
    sleep 2
    if "${SCP[@]}" -q "$TARGET:$SRV_LOG" "$LOGDIR/"; then
        # 远端日志名和本地目标名一致时不要再 mv（mv 会报 "are the same file"）
        [ "$LOGDIR/$(basename "$SRV_LOG")" = "$LOCAL_SRV_LOG" ] || mv -f "$LOGDIR/$(basename "$SRV_LOG")" "$LOCAL_SRV_LOG"
        say "      已取回 → $LOCAL_SRV_LOG"
    else
        echo "⚠ 取回服务端日志失败（远端 $SRV_LOG 不存在？）"
        echo "  本地这份 ssh 输出里有远端线索：$LOGDIR/${TAG}_server_${STAMP}.ssh.out"
        sed 's/^/    /' "$LOGDIR/${TAG}_server_${STAMP}.ssh.out" 2>/dev/null | head -8
    fi
    if [ "$CLEAN" = 1 ]; then
        "${SSH[@]}" "$TARGET" "pkill -f '[/]tmp/$RBASE --server' 2>/dev/null; rm -f '/tmp/$RBASE' '$SRV_LOG'" >/dev/null 2>&1
        say "      已清理远端 /tmp（CLEAN=1）"
    else
        "${SSH[@]}" "$TARGET" "pkill -f '[/]tmp/$RBASE --server' 2>/dev/null" >/dev/null 2>&1
    fi
fi

say "[6/6] 摘要"
report() {   # $1=日志 $2=角色
    local f="$1" role="$2"
    if [ ! -f "$f" ]; then printf '  %-7s ✗ 日志不存在: %s\n' "$role" "$f"; return; fi
    if [ ! -s "$f" ]; then printf '  %-7s ✗ 日志是空的: %s\n' "$role" "$f"; return; fi
    if grep -q 'verify: OK' "$f"; then
        printf '  %-7s %s\n' "$role" "$(grep -m1 'verify: OK' "$f" | sed 's/^.*verify/verify/' | cut -c1-64)"
    else
        printf '  %-7s ⚠ 没有 verify 行（多半没连上对端）—— 日志前 4 行：\n' "$role"
        head -4 "$f" | sed 's/^/            /'
    fi
    printf '  %-7s %s\n' "" "$(grep -m1 'in-flight limit' "$f" | sed 's/^ *//' | cut -c1-58)"
    # ── 取数范围：只取**一个上下文** ────────────────────────────────────────────
    # 断线后 test 程序会重建上下文（日志里再出现一次 "ffrdp_init:"），新上下文那份 dump 全是 0
    # （rtts 是 4294967295 哨兵 / total 0.00MB / conn age 0）⇒ 逐指标 `tail -1` 会把它们跟真数据
    # 混在同一行里：1018-184324 的 c12~c15 就是这样显示成 "rtts —、发/收 —、rto/fast 0/0，可重传率
    # 却有值" 的（看着像工具坏了 ✗，其实是两份 dump 各取了一部分）。
    # 规则：按 "ffrdp_init:" 切段，只取 total_send 最大的那一段（= 真跑过数据的那次）✓。
    # 没重建过（绝大多数情况）⇒ f1 就是原文件，取数口径与以前逐字相同 ✓。
    local res waste rtts spd frf f1="$f" nctx
    nctx="$(grep -c 'ffrdp_init: ' "$f" 2>/dev/null)"; nctx="${nctx:-0}"
    if [ "$nctx" -gt 1 ]; then
        f1="$(mktemp)"
        # 日志只在"重建过"时才切（客户端 80 KB 量级；服务端不会走到这里 ✓）
        awk -v out="$f1" '
            /ffrdp_init: / { n++ }
            /^total_send, total_recv:/ { v = $3; sub(/MB,?$/, "", v); t[n] = v + 0 }
            { buf[n] = buf[n] $0 "\n" }
            END {
                pick = 1
                for (i in t) if (t[i] > t[pick]) pick = i
                printf "%s", buf[pick] > out
                close(out)
            }' "$f"
    fi
    res="$(grep -o 'counter_resend_ratio *: *[0-9.]*%' "$f1" | tail -1 | sed 's/.*: *//' | tr -d '\r')"
    waste="$(grep -o 'counter_resend_waste *: *[0-9.]*%' "$f1" | tail -1 | sed 's/.*: *//' | tr -d '\r')"
    rtts="$(grep -o 'rtts: *[0-9]*' "$f1" | tail -1 | sed 's/.*: *//' | tr -d '\r')"
    frf="$(grep -o 'counter_resend_rto *: *[0-9]*' "$f1" | tail -1 | sed 's/.*: *//' | tr -d '\r')/$(grep -o 'counter_resend_fast *: *[0-9]*' "$f1" | tail -1 | sed 's/.*: *//' | tr -d '\r')"
    # 分母必须与分子同源：total_send/total_recv 是**本连接**累计，所以优先用 dump 的 `conn age` ✓；
    # 老二进制没有那一行才退回 `elapsed`（上下文级 ⇒ 重连过就会偏差 ✗，见下面的口径告警）。
    # ⚠ 精度：低速档（限速到每连接几 KB/s）用 %.2f 会把 0.005 抹成 0.00，多连接相加后仍是 0.00 ✗
    #   （实测 slow/f-20：每连接 120 s 发 0.59 MB = 0.005 MB/s，15 条合计 0.075 全被抹平）。
    #   ⇒ 任一侧 < 0.1 MB/s 时改用三位小数；正常速率仍是两位，`column -t` 与旧解析都照常 ✓。
    spd="$(awk '/^conn age/ { ca = $4 }
               /^elapsed/ { el = $3 }
               /^total_send, total_recv:/ { gsub(/MB,/,"",$3); gsub(/MB/,"",$4); s=$3; r=$4 }
               END { d = (ca+0 > 0) ? ca : el
                     if (d+0 > 0) { u = s/d; v = r/d
                         printf ((u < 0.1 || v < 0.1) ? "%.3f/%.3f" : "%.2f/%.2f"), u, v } }' "$f1")"
    [ "$f1" != "$f" ] && rm -f "$f1"
    printf '  %-7s %s\n' "" "重传率 ${res:-—}   白费 ${waste:-—}   rtts ${rtts:-—} ms   发/收 ${spd:-—} MB/s   rto/fast ${frf:-—}"
    if [ "$nctx" -gt 1 ]; then
        printf '  %-7s %s\n' "" "⚠ 日志里有 ${nctx} 个上下文（中途断开重建 $((nctx-1)) 次）—— 上面只统计跑过数据的那一个，别当成丢包/工具坏了"
    fi
    if [ -s "$LOGDIR/${TAG}_${role}_${STAMP}.cpu" ]; then
        printf '  %-7s %s\n' "" "CPU: $(tr '\n' ' ' < "$LOGDIR/${TAG}_${role}_${STAMP}.cpu")"
    fi
    # 顺手记一行到 logs/summary.csv，扫完一串二进制可以直接列成表
    # ⚠ 空字段必须写成 "-"：写成空的话 `column -t` 会把后面几列顶到前面，看着像"列错位/工具坏了" ✗
    #   （2026-10-15 实测：链路 600ms 时一轮只发了 49 条消息 ⇒ 不少字段没有 ⇒ 表面上看成了"结果不对"）。
    #   另外 rtts 的 4294967295 是协议自己的哨兵（= 本轮还没有任何 RTT 采样）⇒ 标成 none 更好认 ✓。
    [ "$rtts" = "4294967295" ] && rtts="none"
    printf '%s,%s,%s,%s,%s,%s,%s\n' "$STAMP" "$TAG" "$role" \
        "${res:--}" "${waste:--}" "${rtts:--}" "${spd:--}" >> "$LOGDIR/summary.csv"
}
if [ -s "${PINGP:-/nonexistent}" ]; then
    printf '链路 ping   : %s\n' "$(tr '\n' ' ' < "$PINGP")"
fi
[ -f "$LOGDIR/summary.csv" ] || echo 'stamp,tag,role,resend_pct,waste_pct,rtts_ms,send_recv_MBps' > "$LOGDIR/summary.csv"
c=1
while [ "$c" -le "$CLIENTS" ]; do
    if [ "$CLIENTS" = 1 ]; then report "$(client_log "$c")" client
    else                      report "$(client_log "$c")" "client$c"; fi
    c=$((c+1))
done
report "$LOCAL_SRV_LOG" server

# 口径自检：`conn age` 是随连接重建而重置的 ⇒ 两端差太多就说明**这一轮发生过重连**（或一端先启动），
# 那时两行的 MB/s 各自归一化在不同的时间窗里，不能横着比 ✗（2026-10-18 pace1/pace2 就是这么被误读的 ✓）。
CAGE="$(grep -o 'conn age *: *[0-9]*' "$(client_log 1)" 2>/dev/null | tail -1 | sed 's/.*: *//')"
SAGE="$(grep -o 'conn age *: *[0-9]*' "$LOCAL_SRV_LOG" 2>/dev/null | tail -1 | sed 's/.*: *//')"
if [ -n "$CAGE" ] && [ -n "$SAGE" ] && [ "$CAGE" -gt 0 ] && [ "$SAGE" -gt 0 ]; then
    big=$(( CAGE > SAGE ? CAGE : SAGE ))
    dif=$(( CAGE > SAGE ? CAGE - SAGE : SAGE - CAGE ))
    if [ $(( dif * 5 )) -gt "$big" ]; then   # 差 > 20%
        echo "  ⚠ 口径告警：client 连接 ${CAGE}s vs server 连接 ${SAGE}s（差 >20%）"
        echo "     这一轮多半重连过 ⇒ 两行的 MB/s 归在不同的时间窗里，别横向比；看绝对字节（verify 那两行）"
    fi
fi

if command -v column >/dev/null 2>&1; then
    echo
    echo "累计汇总（$LOGDIR/summary.csv，最近几行）："
    column -s, -t "$LOGDIR/summary.csv" | tail -10 | sed 's/^/  /'
fi

echo
echo "重点看：重传率(resend)、rtts、发/收 MB/s，以及 verify 是否为 OK。"
echo "再跑一个二进制：同一命令换个路径即可（KEEP_MUX=1 时一轮密码可跑多轮）。"
