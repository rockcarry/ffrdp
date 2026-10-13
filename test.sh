#!/usr/bin/env bash
# test.sh —— 一条命令跑完一轮真机对测（上传 → 服务端 → 本地客户端 → 收回日志 → 出摘要）
#
#   用法:  ./test.sh <本地二进制> <user@host> [密码] [时长秒]
#   例子:  ./test.sh dist/test_ffrdp_cap16 chenk@192.168.0.213 abc 120
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
#     CPUWATCH=1     同时采样本地客户端进程的 CPU（单核百分比），写进 *.cpu
#     KEEP_MUX=1     跑完不关 SSH 复用连接（连跑多个二进制时少输几次密码）
#     CLEAN=1        跑完删掉远端 /tmp 里的二进制与日志
#     DRY=1          只打印将要做什么，不碰网络（用来检查参数/语法）
#
#   依赖: ssh / scp（必需）、nc、md5sum、timeout、stdbuf；sshpass 与 column 可选；
#         远端需要 pgrep / pkill / wc。
set -u

BIN="${1:-}"; TARGET="${2:-}"; PASS="${3:-}"; DUR="${4:-${DUR:-120}}"
PORT="${PORT:-8111}"
LOGDIR="${LOGDIR:-$PWD/logs}"
# STAMP 决定日志与 CSV 的每轮标识。一次扫描想共享一个标识（便于成组比较）就设 SESSION=xxx。
STAMP="${STAMP:-${SESSION:-$(date +%m%d-%H%M%S)}}"
DRY="${DRY:-0}"; CLEAN="${CLEAN:-0}"

usage() {
    # 只打印开头的注释块：遇到第一条非注释行就停 ✓
    # （不要用固定行号 ✗，也别依赖空行 ✗ —— 注释块里的段落间隔是 "#" 行，不是空行 ✓）
    awk 'NR > 1 { if ($0 !~ /^#/) exit; sub(/^# ?/, ""); print }' "$0" 
    exit 2
}
[ -n "$BIN" ] || usage
[ -n "$TARGET" ] || usage
[ -f "$BIN" ] || { echo "✗ 找不到文件: $BIN"; exit 2; }
[ -x "$BIN" ] || { echo "✗ 没有可执行权限: $BIN  （chmod +x 一下）"; exit 2; }
case "$DUR" in ''|*[!0-9]*) echo "✗ 时长必须是整数秒: $DUR"; exit 2;; esac
case "$PORT" in ''|*[!0-9]*) echo "✗ 端口必须是数字: $PORT"; exit 2;; esac

HOST="${TARGET#*@}"
BASE="$(basename "$BIN")"
TAG="${BASE#test_ffrdp_}"; [ -n "$TAG" ] || TAG="$BASE"
# REMOTE_BIN：远端用另一个二进制（本地仍用 $1）。用来做"两侧不同版本"的交叉对照，
# 例如本地 rthole4 + 远端 oldarch，以分辨"某个方向被掐"是本地发送路径还是对端确认路径的问题。
REMOTE_BIN="${REMOTE_BIN:-$BIN}"
[ -f "$REMOTE_BIN" ] || { echo "✗ REMOTE_BIN 不存在: $REMOTE_BIN"; exit 2; }
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
echo " 标签   : $TAG      时长 ${DUR}s   端口 $PORT"
echo " 服务端 : $TARGET:/tmp/$RBASE"
echo " 客户端 : 本机 → $HOST:$PORT"
echo " 日志   : $LOGDIR"
echo " 认证   : $AUTH"
echo "──────────────────────────────────────────────────────────"

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

say "[3/6] 远端起服务端（${DUR}s 后自动 quit，留 8s 余量）"
SSHPID=0
if [ "$DRY" != 1 ]; then
    # 注意 pkill 的写法：模式里的路径写成 [/]tmp 这样带一对方括号。
    # 否则这条 ssh 包装命令自己的命令行里就含 "/tmp/xxx --server" 这个串，pkill -f 会连它一起杀掉，
    # 后面的启动步骤一行都不会执行（实测：ssh.out 是 0 字节、远端日志根本没生成、客户端连了个空端口）。
    "${SSH[@]}" "$TARGET" "pkill -f '[/]tmp/$RBASE --server' 2>/dev/null; sleep 0.3; rm -f '$SRV_LOG'; ( sleep $((DUR+8)); echo quit ) | timeout $((DUR+25)) stdbuf -oL '/tmp/$RBASE' --server=0.0.0.0:$PORT > '$SRV_LOG' 2>&1; echo REMOTE_EXIT=\$?; wc -c < '$SRV_LOG'" \
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

say "[4/6] 本地起客户端（${DUR}s）"
CPUF="$LOGDIR/${TAG}_client_${STAMP}.cpu"
if [ "$DRY" = 1 ]; then
    say "      (DRY) ( sleep $DUR; echo quit ) | stdbuf -oL $BIN --client=$HOST:$PORT > $LOCAL_LOG"
else
    ( sleep "$DUR"; echo quit ) | timeout $((DUR+15)) stdbuf -oL "$BIN" --client="$HOST:$PORT" > "$LOCAL_LOG" 2>&1 &
    TPID=$!
    if [ "${CPUWATCH:-0}" = 1 ]; then
        cpu_watch "$TPID" "$CPUF" &
    fi
    wait "$TPID" 2>/dev/null
    RC=$?
    say "      客户端结束（退出码 $RC）→ $LOCAL_LOG"
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
    local res waste rtts spd frf
    res="$(grep -o 'counter_resend_ratio *: *[0-9.]*%' "$f" | tail -1 | sed 's/.*: *//' | tr -d '\r')"
    waste="$(grep -o 'counter_resend_waste *: *[0-9.]*%' "$f" | tail -1 | sed 's/.*: *//' | tr -d '\r')"
    rtts="$(grep -o 'rtts: *[0-9]*' "$f" | tail -1 | sed 's/.*: *//' | tr -d '\r')"
    frf="$(grep -o 'counter_resend_rto *: *[0-9]*' "$f" | tail -1 | sed 's/.*: *//' | tr -d '\r')/$(grep -o 'counter_resend_fast *: *[0-9]*' "$f" | tail -1 | sed 's/.*: *//' | tr -d '\r')"
    spd="$(awk '/^elapsed/ { el = $3 }
               /^total_send, total_recv:/ { gsub(/MB,/,"",$3); gsub(/MB/,"",$4); s=$3; r=$4 }
               END { if (el+0 > 0) printf "%.2f/%.2f", s/el, r/el }' "$f")"
    printf '  %-7s %s\n' "" "重传率 ${res:-—}   白费 ${waste:-—}   rtts ${rtts:-—} ms   发/收 ${spd:-—} MB/s   rto/fast ${frf:-—}"
    if [ -s "$LOGDIR/${TAG}_${role}_${STAMP}.cpu" ]; then
        printf '  %-7s %s\n' "" "CPU: $(tr '\n' ' ' < "$LOGDIR/${TAG}_${role}_${STAMP}.cpu")"
    fi
    # 顺手记一行到 logs/summary.csv，扫完一串二进制可以直接列成表
    printf '%s,%s,%s,%s,%s,%s,%s\n' "$STAMP" "$TAG" "$role" \
        "${res%\%}" "${waste%\%}" "${rtts:-}" "${spd:-}" >> "$LOGDIR/summary.csv"
}
[ -f "$LOGDIR/summary.csv" ] || echo 'stamp,tag,role,resend_pct,waste_pct,rtts_ms,send_recv_MBps' > "$LOGDIR/summary.csv"
report "$LOCAL_LOG" client
report "$LOCAL_SRV_LOG" server

if command -v column >/dev/null 2>&1; then
    echo
    echo "累计汇总（$LOGDIR/summary.csv，最近几行）："
    column -s, -t "$LOGDIR/summary.csv" | tail -10 | sed 's/^/  /'
fi

echo
echo "重点看：重传率(resend)、rtts、发/收 MB/s，以及 verify 是否为 OK。"
echo "再跑一个二进制：同一命令换个路径即可（KEEP_MUX=1 时一轮密码可跑多轮）。"
