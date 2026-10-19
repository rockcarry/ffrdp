#!/usr/bin/env bash
# tools/perf-eval.sh —— 性能评测一键跑：3 环境 × (11 档 ffrdp + 3 档 TCP)，产出 results.csv + summary.md
#
# 为什么要有它：性能评测的**口径**比数字重要 —— 同一条命令里固定住
#   · 二进制（同一 md5，两端一致）· 时长 · 连接数 · 采样间隔 · 方向 · 限速（按基线容量的百分比）
#   · CPU 采样方式（/proc/<pid>/stat 的 utime+stime ÷ 墙上时间，客户端为**多进程合计**）
# 否则两轮之间的差异分不清是代码变了、链路变了还是口径变了（1019-163445 那种 8.6× 漂移）。
#
# 用法:
#   TARGET=chenk@192.168.0.213 bash tools/perf-eval.sh        # 三环境全跑：lo → 慢 AP → 快 AP
#   TARGET=... ENVS=lo bash tools/perf-eval.sh                # 只跑 lo（验工具 ✓）
#   TARGET=... SCEN="s-up-100 f-asym-80-20" bash tools/perf-eval.sh   # 只跑指定档
#   TARGET=... DUR=60 REPEAT=2 bash tools/perf-eval.sh        # 每轮 60 s、每档 2 遍
#   TARGET=... TCP=0 bash tools/perf-eval.sh                  # 跳过 iperf3（只测 ffrdp）
#   DRY=1 TARGET=x@y bash tools/perf-eval.sh                  # 只打印计划，不编译不跑
#
# 环境（ENVS，按给出的顺序跑）:
#   lo    本地回环 127.0.0.1（两端都在本机；不碰 ssh、不切 AP）
#   slow  慢 AP（有路由器带宽限制）—— 会提示你切 AP 并**等回车**
#   fast  快 AP（无带宽限制）      —— 同上
#
# 场景（SCEN=all 或空格分隔的场景名）—— 11 档 = 方向 × 比例，单连接与满连接各一遍：
#   {s|f}-up-{100|80|20}     只上行（客户端发 / 服务端 --recv_only）
#   {s|f}-down-{100|80|20}   只下行（服务端发 / 客户端 --recv_only）
#   {s|f}-both-{100|80|20}   双向对称
#   {s|f}-asym-80-20         双向非对称：上行 80% / 下行 20%
#   {s|f}-asym-20-80         双向非对称：上行 20% / 下行 80%
#   比例是**参照容量**的百分比；总带宽 ÷ 连接数后下发（--send_speed 是每连接语义 ✓）。
#
# TCP 对照（TCP=1，只有 100% 档）：
#   base           单连接三方向（iperf3 上行 / -R 下行 / --bidir 双向）+ ping + CPU
#   tcp-f-{up|down|both}   满连接三方向（iperf3 -P CLIENTS）
#   ⚠ iperf3 的 -b 只对 UDP 生效（TCP 直接忽略）⇒ 20/80 档**没有**等价的 TCP 能力，
#     那几档只有 ffrdp，报告里不能拿满速 TCP 去比限速 ffrdp ✗。
#   TCP 侧的 resend 记 ping 丢包率、rtts 记 ping 平均延迟（没有协议层计数器可读）。
#
# 输出（logs/perf-<stamp>/）:
#   results.csv   每个场景一行（吞吐 / resend / rtts / 两侧 CPU / verify / 重连）
#   base.csv      三环境的基线（ping + iperf3）
#   summary.md    7 张表（按 docs/perf-eval.md 的顺序生成，可直接抄）
#   raw/<场景>/   两侧日志、cpu、ping/iperf3 原始输出
#
# 依赖: iperf3、ping、nc、awk、pgrep；远端需要同样的 iperf3/pgrep（lo 档不需要 ssh）。
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1

# ── 参数 ────────────────────────────────────────────────────────────────────
TARGET="${TARGET:-}"
PASS="${PASS:-}"
BIN="${BIN:-./test_ffrdp}"
ENVS="${ENVS:-lo slow fast}"
SCEN="${SCEN:-all}"               # all | 空格分隔的场景名（如 "s-up-100 f-asym-80-20"）
DUR="${DUR:-60}"                  # 60 s + 5 s 采样 = 12 点，与 120 s/10 s 的样本量相同，但墙钟减半
CLIENTS="${CLIENTS:-15}"          # 满连接 = maxconn-1（见 ffrdp.h：0 号槽给内嵌主连接）
DUMP_MS="${DUMP_MS:-5000}"        # ffrdp 的 dump 间隔；60 s 轮次要 12 个采样点 ⇒ 5 s
DUMP_S="${DUMP_S:-5}"             # iperf3 的 -i 间隔（与 dump 同网格，两阶段统计才对得上）
TCP="${TCP:-1}"                   # 1 = 跑 TCP 基线/对照（100% 三方向，单连接 + 满连接）
PORT="${PORT:-8111}"
PINGCOUNT="${PINGCOUNT:-20}"
IPERF_TIME="${IPERF_TIME:-30}"
IPERF_PORT="${IPERF_PORT:-5301}"
REPEAT="${REPEAT:-1}"
BUILD="${BUILD:-dev}"             # dev：dump 在 ⇒ resend/rtts 读得到（仪表开销 <0.5% 核）
TOOLCHAIN_NAME="${TOOLCHAIN_NAME:-gcc}"
REF_MODE="${REF_MODE:-auto}"      # auto|iperf|self：限速百分比的参照（lo 自动用 self）
APWAIT="${APWAIT:-1}"             # 1 = 切 AP 时提示并等回车
DRY="${DRY:-0}"
OUT="${OUT:-$ROOT/logs/perf-$(date +%m%d-%H%M%S)}"

say() { printf '%s  %s\n' "$(date +%H:%M:%S)" "$*"; }
die() { echo "✗ $*" >&2; exit 2; }

env_name() { case "$1" in lo) echo "lo（本地回环）";; slow) echo "慢 AP（有带宽限制）";; fast) echo "快 AP（无带宽限制）";; *) echo "$1";; esac; }
env_host() { case "$1" in lo) echo "127.0.0.1";; *) echo "${TARGET#*@}";; esac; }
is_remote() { [ "$1" != "lo" ]; }

# ── CPU 采样：/proc/<pid>/stat 的 utime+stime，跨**所有**匹配进程求和 ──────────
# 为什么按模式找而不是按 pid：满连接有 15 个客户端进程，只盯第一个会漏掉 14/15 的 CPU。
_cpu_ticks() {  # $1 = pgrep -f 模式
    local pat="$1" tot=0 p a b
    for p in $(pgrep -f "$pat" 2>/dev/null); do
        [ -r "/proc/$p/stat" ] || continue
        a="$(awk '{print $14}' "/proc/$p/stat" 2>/dev/null)"
        b="$(awk '{print $15}' "/proc/$p/stat" 2>/dev/null)"
        [ -n "$a" ] && [ -n "$b" ] && tot=$(( tot + a + b ))
    done
    echo "$tot"
}

cpu_loop() {  # $1 = 模式，$2 = 最长采样秒数 ⇒ 输出 min/avg/peak/var（单核百分比，可 >100 = 多进程）
    # 为什么要 min/var：只看 avg/peak 分不清"稳定 20%"和"一半时间 0、一半时间 40%"。方差就是那半个答案 ✓。
    local pat="$1" guard="$2"
    local hz; hz="$(getconf CLK_TCK 2>/dev/null || echo 100)"
    local end t now dt cur peak=0 mn=0 sum=0 sumsq=0 n=0
    local prev_t=0 prev_n=0 first_t=0 first_n=0 last_t=0 last_n=0
    end=$(( $(date +%s) + guard ))
    # 等进程出现（最多 20 s；test.sh 里服务端先起 4 s）
    while [ "$(date +%s)" -lt "$end" ]; do
        t="$(_cpu_ticks "$pat")"
        if [ "$t" != 0 ]; then break; fi
        sleep 0.2
    done
    if [ "$t" = 0 ]; then echo "no_pid"; return; fi
    prev_t="$t"; prev_n="$(date +%s%N)"; first_t="$prev_t"; first_n="$prev_n"
    while [ "$(date +%s)" -lt "$end" ]; do
        sleep 1
        t="$(_cpu_ticks "$pat")"
        now="$(date +%s%N)"
        if [ "$t" = 0 ]; then break; fi          # 进程都退了
        dt=$(( (now - prev_n) / 1000000 ))
        if [ "$dt" -gt 0 ] && [ "$t" -ge "$prev_t" ]; then
            cur=$(( (t - prev_t) * 100000 / (hz * dt) ))
            [ "$cur" -gt "$peak" ] && peak="$cur"
            [ "$n" -eq 0 ] || [ "$cur" -lt "$mn" ] && mn="$cur"
            sum=$(( sum + cur )); sumsq=$(( sumsq + cur * cur )); n=$(( n + 1 ))
        fi
        prev_t="$t"; prev_n="$now"; last_t="$t"; last_n="$now"
    done
    # avg **必须**用逐点平均（与 min/max/σ² 同一批样本）✗ 不能用首末 ticks 差：
    # 满连接档的 15 个客户端是错峰启停的（每 0.4 s 起一个、陆续退出），首末差会被启动/退出阶段
    # 稀释到离谱（实测 lo/f-full：min 40、max 64、首末平均 2 —— 三个数互相矛盾）。
    # 首末跨度平均另存为 span_avg，只当"含未采样时间"的诊断值，不再喂给 CSV。
    dt=$(( (last_n - first_n) / 1000000 ))
    local span=0
    if [ "$dt" -gt 0 ] && [ "$last_t" -ge "$first_t" ]; then
        span=$(( (last_t - first_t) * 100000 / (hz * dt) ))
    fi
    awk -v span="$span" -v mn="$mn" -v mx="$peak" -v s="$sum" -v q="$sumsq" -v n="$n" 'BEGIN {
        v = (n > 0) ? q / n - (s / n) ^ 2 : 0; if (v < 0) v = 0
        a = (n > 0) ? s / n : 0
        printf "min_pct_of_one_core=%d\n", mn
        printf "avg_pct_of_one_core=%.0f\n", a
        printf "peak_pct_of_one_core=%d\n", mx
        printf "var_pct_of_one_core=%.2f\n", v
        printf "span_avg_pct_of_one_core=%d\n", span
        printf "samples=%d\n", n
    }'
}

cpu_start_local() {  # $1=模式 $2=guard $3=输出文件 ⇒ 置 CPUPID
    cpu_loop "$1" "$2" > "$3" 2>&1 &
    CPUPID=$!
}

cpu_start_remote() { # $1=模式 $2=guard $3=输出文件 ⇒ 置 CPUPID（函数体整体发到远端 ✓）
    # 用 declare -f 把同一份实现发过去：本地/远端口径逐字相同（不复制一份循环 ✗）
    "${SSH[@]}" "$TARGET" "$(declare -f _cpu_ticks cpu_loop)
cpu_loop $(printf '%q' "$1") $(printf '%q' "$2")" > "$3" 2>&1 &
    CPUPID=$!
}

cpu_get() {  # $1=文件 $2=avg|peak ⇒ 数字（缺失 = "-"）
    [ -s "$1" ] || { echo "-"; return; }
    local v
    v="$(grep -m1 "^$2_pct_of_one_core=" "$1" 2>/dev/null | cut -d= -f2)"
    echo "${v:--}"
}

# ── 整机 CPU（含 softirq）：回答"进程 CPU 很低，是不是机器其实很忙"──────────────
# 为什么需要：cpu_get 读的是 `/proc/<pid>/stat` 的 utime+stime，**只算进程自己**；
# 跨网卡的收发包开销落在软中断 / ksoftirqd / host 侧（WSL2 的 vNIC 尤其如此），完全不计入。
# 实测：fast AP 上 iperf3 跑 28 MB/s，进程 CPU 是 0%（峰值也 0）—— 不是采样失败（不是 no_pid），
# 而是这笔账不在进程头上。整机口径能把"guest 内到底忙不忙"分离出来。
host_cpu_loop() {  # $1=最长秒数 $2=输出文件 ⇒ min/avg/peak/var（整机，含 softirq）
    local end cur peak=0 mn=0 sum=0 sumsq=0 n=0
    local u n0 s id iw ir si st db di dtot
    end=$(( $(date +%s) + $1 ))
    # /proc/stat 的 cpu 行有 11 个字段；read 的变量少了会让**最后一个变量吃掉剩余字段**
    # （实测：只读 9 个 ⇒ st="0 0" ⇒ $(( )) 报 "syntax error in expression"）
    read -r _cp u n0 s id iw ir si st _g _gn < /proc/stat
    local b0=$(( u + n0 + s + ir + si + st )) i0=$(( id + iw ))
    while [ "$(date +%s)" -lt "$end" ]; do
        sleep 1
        read -r _cp u n0 s id iw ir si st _g _gn < /proc/stat
        local b1=$(( u + n0 + s + ir + si + st )) i1=$(( id + iw ))
        db=$(( b1 - b0 )); di=$(( i1 - i0 )); dtot=$(( db + di ))
        if [ "$dtot" -gt 0 ]; then
            cur=$(( db * 100 / dtot ))
            [ "$cur" -gt "$peak" ] && peak=$cur
            [ "$n" -eq 0 ] || [ "$cur" -lt "$mn" ] && mn=$cur
            sum=$(( sum + cur )); sumsq=$(( sumsq + cur * cur )); n=$(( n + 1 ))
        fi
        b0=$b1; i0=$i1
    done
    awk -v mn="$mn" -v mx="$peak" -v s="$sum" -v q="$sumsq" -v n="$n" 'BEGIN {
        if (n == 0) { print "min_pct_of_machine=-"; print "avg_pct_of_machine=-"; print "peak_pct_of_machine=-"; print "var_pct_of_machine=-"; print "samples=0"; exit }
        v = q / n - (s / n) ^ 2; if (v < 0) v = 0
        printf "min_pct_of_machine=%d\n", mn
        printf "avg_pct_of_machine=%.1f\n", s / n
        printf "peak_pct_of_machine=%d\n", mx
        printf "var_pct_of_machine=%.2f\n", v
        printf "samples=%d\n", n
    }'
}

host_cpu_get() {  # $1=文件 $2=avg|peak ⇒ 整机占用百分比
    [ -s "$1" ] || { echo "-"; return; }
    local v
    v="$(grep -m1 "^$2_pct_of_machine=" "$1" 2>/dev/null | cut -d= -f2)"
    echo "${v:--}"
}

# ── 模式串（pgrep -f）：用 [x]xxx 的写法避免匹配到自己（自己的命令行里是 "[x]xxx"）──
pat_of() { printf '[%s]%s' "${1:0:1}" "${1:1}"; }
pat_cli()   { pat_of "$(basename "$1") --client=$(env_host "$2"):$PORT"; }
pat_srv_lo() { pat_of "$(basename "$1") --server=127.0.0.1:$PORT"; }
pat_srv_ap() { pat_of "/tmp/$(basename "$1") --server"; }

# ── ssh 复用（与 test.sh 用同一个 ControlPath ⇒ 两边共享一条连接、只输一次密码）──
CTL="/tmp/.ssh-mux-${USER:-u}-$(echo "${TARGET:-x}" | tr -c 'A-Za-z0-9' '_')"
SSH=()
ssh_setup() {
    is_remote "$1" || return 0
    [ -n "$TARGET" ] || die "真机档要 TARGET=user@host"
    local opts=(-o ConnectTimeout=8 -o StrictHostKeyChecking=accept-new -o ServerAliveInterval=10 -o ServerAliveCountMax=2)
    if command -v sshpass >/dev/null 2>&1 && [ -n "$PASS" ]; then
        SSH=(sshpass -p "$PASS" ssh "${opts[@]}")
        return 0
    fi
    # 幂等：**不能**只靠 `${#SSH[@]} -gt 0` 提前返回 ✗ —— 切 Wi-Fi 之后老 ControlMaster 的 TCP 路径
    # 已经没了，复用它只会得到 "mux_client_request_session: read from master failed: Broken pipe"
    # 再退化成一串 password 提示（实测：切到第二个 AP 时整轮卡死）。所以每次都验一次，死了就重建。
    if [ "${#SSH[@]}" -gt 0 ] \
        && ssh -O check -o ControlPath="$CTL" "$TARGET" >/dev/null 2>&1 \
        && ssh -o ControlPath="$CTL" -o BatchMode=yes -o ConnectTimeout=6 "$TARGET" true >/dev/null 2>&1; then
        return 0
    fi
    say "· 建立 SSH 复用连接（切 AP 后会自动重建，只需再输一次密码）"
    # 老 master 可能还挂着一条僵死的 socket：先让它体面退出，再删 socket 重建
    ssh -O exit -o ControlPath="$CTL" "$TARGET" >/dev/null 2>&1 || true
    rm -f "$CTL"
    ssh -o ControlMaster=auto -o ControlPath="$CTL" -o ControlPersist=900 "${opts[@]}" -N -f "$TARGET" \
        || die "无法建立 SSH 复用连接到 $TARGET"
    SSH=(ssh -o ControlPath="$CTL" "${opts[@]}")
}

# ── 切 AP：提示 + 等回车 + 连通性确认 ─────────────────────────────────────────
wait_switch() {  # $1 = 环境名
    is_remote "$1" || return 0
    [ "$APWAIT" = 1 ] || return 0
    local h; h="$(env_host "$1")"
    echo
    echo "────────────────────────────────────────────────────────────────"
    echo "  ▶ 要测【$(env_name "$1")】"
    echo "    请把客户端切到对应的 Wi-Fi（本工具无法判断你连的是哪一个）"
    echo "    切好后先确认：  ping -c 3 $h"
    echo "    然后按【回车】开始；按 Ctrl-C 中止整轮"
    echo "────────────────────────────────────────────────────────────────"
    if [ "$DRY" = 1 ]; then echo "  (DRY) 不等待"; return 0; fi
    read -r _ < /dev/tty || true
    local i=0
    until nc -z -w 6 "$h" 22 2>/dev/null; do
        i=$((i+1))
        [ "$i" -gt 20 ] && die "连不上 $h:22（切好 Wi-Fi 后重跑）"
        echo "    …还没连上 $h:22（$i/20），5 s 后重试"
        sleep 5
    done
}

confirm_base() {  # $1=环境 $2=丢包 $3=rtt avg $4=下行 MB/s ⇒ 让人眼确认"这是对的 AP"
    is_remote "$1" || return 0
    [ "$APWAIT" = 1 ] || return 0
    [ "$DRY" = 1 ] && return 0
    echo
    echo "  基线读数：丢包 ${2:-?}   ping avg ${3:-?} ms   iperf3 下行 ${4:-?} MB/s"
    echo "  与【$(env_name "$1")】相符吗？不符按 Ctrl-C，相符按【回车】继续"
    read -r _ < /dev/tty || true
}

_pct() {  # $1=参考 KB/s  $2=百分比 ⇒ 取整后的 KB/s
    local v; v=$(( ($1 * $2 + 50) / 100 ))
    [ "$v" -lt 1 ] && v=1
    echo "$v"
}

kb_of() {  # $1=MB/s（可能带小数，如 0.33）⇒ KB/s 整数（不能用 ${x%.*}：0.33 会被截成 0 ✗）
    awk -v m="${1:-0}" 'BEGIN { if (m + 0 <= 0) print 0; else printf "%d", m * 1024 }'
}

client_logs() {  # $1=目录 ⇒ 客户端日志（真机：<tag>_client_<stamp>*.log；lo：client*.log）
    ls -1 "$1"/*_client_*.log "$1"/client*.log 2>/dev/null
}

# ── 基线：ping + iperf3（TCP，上行 = 客户端发；下行 = -R）──────────────────────
base_run() {  # $1=环境 $2=输出前缀
    local e="$1" p="$2" h; h="$(env_host "$e")"
    mkdir -p "$p"
    # ping
    ping -c "$PINGCOUNT" -i 0.2 -W 2 "$h" > "$p/ping.txt" 2>&1 || true
    local loss rtt
    loss="$(grep -o '[0-9.]*% packet loss' "$p/ping.txt" | tail -1 | cut -d% -f1)"
    rtt="$(grep -o 'rtt min/avg/max[^=]*= *[0-9./]*' "$p/ping.txt" | tail -1 | sed 's/.*= *//')"
    local rtt_avg; rtt_avg="$(echo "${rtt:-/0/}" | cut -d/ -f2)"
    loss="${loss:-?}"; rtt_avg="${rtt_avg:-?}"

    local up_mbps="-" dn_mbps="-"
    if [ "$SKIP_IPERF" = 1 ]; then
        say "  (SKIP_IPERF=1) 跳过 iperf3"
    elif ! command -v iperf3 >/dev/null 2>&1; then
        say "  ⚠ 本机没有 iperf3 ⇒ 基线吞吐留空（装一下：apt install iperf3）"
    else
        local ip_up ip_dn
        if is_remote "$e"; then
            ssh_setup "$e"
            if ! "${SSH[@]}" "$TARGET" "command -v iperf3 >/dev/null 2>&1"; then
                say "  ⚠ 远端没有 iperf3 ⇒ 基线吞吐留空（远端装一下：apt install iperf3）"
                SKIP_IPERF=1
            fi
        fi
        if [ "$SKIP_IPERF" != 1 ]; then
            ip_up="$(iperf_one "$e" "" "$p/iperf-up.txt")"
            ip_dn="$(iperf_one "$e" "-R" "$p/iperf-down.txt")"
            up_mbps="${ip_up:-?}"; dn_mbps="${ip_dn:-?}"
            # 双向：ffrdp 是双向同时跑，单向 iperf3 会把"回程 ACK 抢占上行队列"这笔账漏掉
            # ⇒ 双向读数才是同形状的参照（报告里两个都给，别只挑好看的那个）。
            if iperf3 --help 2>&1 | grep -q -- '--bidir'; then
                local bd bup bdn
                bd="$(iperf_bidir "$e" "$p/iperf-bidir.txt")"
                bup="${bd%% *}"; bdn="${bd##* }"
                printf '%s,%s,%s\n' "$e" "$bup" "$bdn" >> "$OUT/base-bidir.csv"
                BB["$e"]="$bup/$bdn"
                say "  基线 $e：iperf3 双向 发/收 ${bup}/${bdn} MB/s"
            else
                say "  ⚠ iperf3 没有 --bidir（需 ≥ 3.7）⇒ 双向基线留空"
            fi
        fi
    fi
    # CPU 文件名由 iperf_one 决定：$out(=iperf-up.txt/.iperf-down.txt) + .cli.cpu/.srv.cpu
    printf '%s,%s,%s,%s,%s,%s,%s\n' "$e" "$loss" "${rtt:-?}" "$up_mbps" "$dn_mbps" \
        "$(cpu_get "$p/iperf-up.txt.cli.cpu" avg)" "$(cpu_get "$p/iperf-up.txt.srv.cpu" avg)" >> "$OUT/base.csv"
    say "  基线 $e：丢包 ${loss}  ping ${rtt:-?}  iperf3 上/下 ${up_mbps}/${dn_mbps} MB/s"
    # ── 吞吐**期间**的 ping（并发采样）：空载 ping 只看得到传播时延，看不到排队 ──
    # 每个阶段（单向发 / 单向收 / 双向）各自的 min/avg/max/mdev 落 base-load.csv；
    # 这一组才是"用户体感"，也是判断 bufferbloat 的唯一直接证据 ✓。
    local ph file pl pr pavg=-
    local avgu=- avgd=- avgb=-
    for ph in up down bidir; do
        case "$ph" in
        up)    file="$p/iperf-up.txt.ping" ;;
        down)  file="$p/iperf-down.txt.ping" ;;
        *)     file="$p/iperf-bidir.txt.ping" ;;
        esac
        [ -s "$file" ] || continue
        pl="$(grep -o '[0-9.]*% packet loss' "$file" | tail -1 | cut -d% -f1)"
        pr="$(grep -o 'rtt min/avg/max[^=]*= *[0-9./]*' "$file" | tail -1 | sed 's/.*= *//')"
        pavg="$(echo "${pr:-}" | cut -d/ -f2)"
        printf '%s,%s,%s,%s\n' "$e" "$ph" "${pl:-?}" "${pr:--}" >> "$OUT/base-load.csv"
        case "$ph" in
        up)   avgu="${pavg:--}" ;;
        down) avgd="${pavg:--}" ;;
        *)    avgb="${pavg:--}" ;;
        esac
    done
    LP["$e"]="上 ${avgu} / 下 ${avgd} / 双向 ${avgb}"
    say "  基线 $e：吞吐期间 ping avg（发/收/双向）${avgu}/${avgd}/${avgb} ms"
    # 进程 CPU 与整机 CPU 一起看：前者只含 utime+stime，跨网卡的软中断开销不在里面
    say "  基线 $e：整机 CPU avg/peak（客户端，双向阶段）$(host_cpu_get "$p/iperf-bidir.txt.hostcpu" avg)/$(host_cpu_get "$p/iperf-bidir.txt.hostcpu" peak)%"
    # 给 summary 用（KB/s 参照在调用处换算）
    BASE_LOSS="$loss"; BASE_RTT="$rtt"; BASE_UP_MBPS="$up_mbps"; BASE_DN_MBPS="$dn_mbps"
}

# ── iperf3 输出解析 ──────────────────────────────────────────────────────────
_bitrate_units() {  # $1=文件 $2=角色标记（可空；如 TX-C / RX-C）⇒ "数字 单位"
    # 单流时 iperf3 **没有 [SUM]**（-P>1 才有），而且 [SUM]/[ID] 两种行的字段偏移不同 ⇒
    # 统一从行尾的 sender|receiver 反查它前面那个 "数字 + 比特率单位"。
    # ⚠ `$2` 必须用 ${2:-}：单向解析（iperf_one）不传角色，`set -u` 下裸 $2 会直接 unbound。
    awk -v role="${2:-}" '
        /(sender|receiver)[ \t]*$/ {
            if (role != "" && index($0, "[" role "]") == 0) next
            n = ""
            for (i = 1; i < NF; i++) if ($(i+1) ~ /^[KMG]?bits\/sec$/) n = $(i) " " $(i+1)
            if (n == "") next
            if ($NF == "receiver") r = n; else s = n
        }
        END { print (r != "") ? r : s }' "$1"
}

_bitrate_mbps() {  # $1="数字 单位" ⇒ MB/s（空 ⇒ "-"）
    local n="${1%% *}" u="${1##* }"
    [ -z "$n" ] && { echo "-"; return; }
    awk -v n="$n" -v u="$u" 'BEGIN {
        m = (u ~ /Gbits/) ? n * 1000 / 8 : (u ~ /Mbits/) ? n / 8 : (u ~ /Kbits/) ? n / 8000 : n / 8;
        printf "%.2f", m }'
}

_iperf_server_start() {  # $1=环境 $2=输出前缀 ⇒ 置 IPERF_SP / IPERF_SCP
    if is_remote "$1"; then
        # 服务端真实命令行是 "iperf3 -s -1 -p <port>"，匹配串里必须带 -1，否则 pgrep/pkill 全落空。
        # ⚠️ pkill 必须**单独一次 ssh**：若与启动写在同一条命令里，承载它的 shell 的 cmdline 就含
        #   字面量 "iperf3 -s -1 -p <port>"，`pkill -f` 会把父 shell 一起杀掉 ⇒ 服务端从未启动 ⇒
        #   客户端只拿到 "Connection refused"、.srv 文件 0 字节（`[i]` 只挡得住 pkill 自己 ✗）。
        "${SSH[@]}" "$TARGET" "pkill -f '[i]perf3 -s -1 -p $IPERF_PORT' 2>/dev/null; true" >/dev/null 2>&1
        "${SSH[@]}" "$TARGET" "iperf3 -s -1 -p $IPERF_PORT" > "$2.srv" 2>&1 &
        IPERF_SP=$!
        sleep 1
        cpu_start_remote "$(pat_of "iperf3 -s -1 -p $IPERF_PORT")" $((IPERF_TIME + 15)) "$2.srv.cpu"
        IPERF_SCP=$CPUPID
    else
        iperf3 -s -1 -p "$IPERF_PORT" > "$2.srv" 2>&1 &
        IPERF_SP=$!
        sleep 1
        cpu_start_local "$(pat_of "iperf3 -s -1 -p $IPERF_PORT")" $((IPERF_TIME + 15)) "$2.srv.cpu"
        IPERF_SCP=$CPUPID
    fi
}

iperf_one() {  # $1=环境 $2="-R" 或 "" $3=原始输出 $4=并行流数（默认 1）⇒ echo MB/s（单向）
    local e="$1" rev="$2" out="$3" par="${4:-1}" h; h="$(env_host "$e")"
    local parg=""; [ "$par" -gt 1 ] && parg="-P $par"    # -P 1 就别加了：输出格式会多出 [SUM] 行
    local IPERF_SP=0 IPERF_SCP=0
    _iperf_server_start "$e" "$out"
    local sp=$IPERF_SP scp=$IPERF_SCP
    cpu_start_local "$(pat_of "iperf3 -c $h")" $((IPERF_TIME + 15)) "$out.cli.cpu"
    local ccp=$!
    # 并发 ping：**空载** ping 看不到排队（bufferbloat）。吞吐打满时的 RTT 才是体感，
    # 而且 5 次/秒 ⇒ 30 s 有 150 个样本，够看 P 型尾部（见 base-load.csv）。
    timeout $((IPERF_TIME + 5)) ping -c $((IPERF_TIME * 5)) -i 0.2 -W 2 "$h" > "$out.ping" 2>&1 &
    local pp=$!
    host_cpu_loop $((IPERF_TIME + 2)) > "$out.hostcpu" &
    local hcp=$!
    iperf3 -c "$h" -p "$IPERF_PORT" -t "$IPERF_TIME" -i "$DUMP_S" $parg $rev > "$out" 2>&1 || true
    wait $pp 2>/dev/null
    wait $hcp 2>/dev/null
    wait $sp 2>/dev/null; wait $scp 2>/dev/null; wait $ccp 2>/dev/null
    _bitrate_mbps "$(_bitrate_units "$out")"
}

iperf_bidir() {  # $1=环境 $2=原始输出 $3=并行流数（默认 1）⇒ echo "上行 下行"（MB/s）
    # 为什么要它：`iperf3 -c` 只跑一个方向（-R 也只是"只跑反向"），而 ffrdp/test.c 是**双向同时** ——
    # 回程 ACK 与本端上行会抢同一条发送队列（ffrdp-v2.md §9.1 那条自伤就是它），所以单向 iperf3 当参照
    # 会**高估** ffrdp 的相对表现。--bidir 才是同形状的对照（iperf3 ≥ 3.7 支持）。
    local e="$1" out="$2" par="${3:-1}" h; h="$(env_host "$e")"
    local parg=""; [ "$par" -gt 1 ] && parg="-P $par"
    local IPERF_SP=0 IPERF_SCP=0
    _iperf_server_start "$e" "$out"
    local sp=$IPERF_SP scp=$IPERF_SCP
    cpu_start_local "$(pat_of "iperf3 -c $h")" $((IPERF_TIME + 15)) "$out.cli.cpu"
    local ccp=$!
    # 双向期间的并发 ping：这一档最接近 ffrdp 的真实工作点（上下行同时压满）
    timeout $((IPERF_TIME + 5)) ping -c $((IPERF_TIME * 5)) -i 0.2 -W 2 "$h" > "$out.ping" 2>&1 &
    local pp=$!
    host_cpu_loop $((IPERF_TIME + 2)) > "$out.hostcpu" &
    local hcp=$!
    iperf3 -c "$h" -p "$IPERF_PORT" -t "$IPERF_TIME" -i "$DUMP_S" $parg --bidir > "$out" 2>&1 || true
    wait $pp 2>/dev/null
    wait $hcp 2>/dev/null
    wait $sp 2>/dev/null; wait $scp 2>/dev/null; wait $ccp 2>/dev/null
    # --bidir 的行带角色标记：[TX-C] = 客户端发（上行）、[RX-C] = 客户端收（下行）
    printf '%s %s' "$(_bitrate_mbps "$(_bitrate_units "$out" 'TX-C')")" \
                   "$(_bitrate_mbps "$(_bitrate_units "$out" 'RX-C')")"
}

# ── ffrdp 一轮：lo（本地两/多进程）与真机（走 test.sh）─────────────────────────
run_lo() {  # $1=kind $2=方向 $3=上行% $4=下行% $5=目录 $6=stamp
    local kind="$1" dmode="$2" up_pct="$3" dn_pct="$4" dir="$5" st="$6" ncl c
    ncl=1; [ "$kind" = full ] && ncl="$CLIENTS"
    local up_kb=0 dn_kb=0 srv_arg="" cli_arg=""
    # 限速 = 参照容量 × 比例，再按连接数均分（--send_speed 是**每连接**的 ⇒ 总量 ÷ 连接数 ✓）
    if [ "$up_pct" != 0 ] && [ "${REF_UP:-0}" != 0 ]; then
        up_kb=$(( $(_pct "$REF_UP" "$up_pct") / ncl )); [ "$up_kb" -lt 1 ] && up_kb=1
        cli_arg="--send_speed=$up_kb"
    fi
    if [ "$dn_pct" != 0 ] && [ "${REF_DN:-0}" != 0 ]; then
        dn_kb=$(( $(_pct "$REF_DN" "$dn_pct") / ncl )); [ "$dn_kb" -lt 1 ] && dn_kb=1
        srv_arg="--send_speed=$dn_kb"
    fi
    # 方向：两个参数必须成对给 —— --send_only 那侧才不会被"5 s 收不到字节"判失联 ✓
    case "$dmode" in
    up)   cli_arg="$cli_arg --send_only"; srv_arg="$srv_arg --recv_only" ;;
    down) cli_arg="$cli_arg --recv_only"; srv_arg="$srv_arg --send_only" ;;
    esac
    cli_arg="$cli_arg --dump_ms=$DUMP_MS"
    srv_arg="$srv_arg --dump_ms=$DUMP_MS"
    mkdir -p "$dir"
    # 服务端用 FIFO 喂 stdin：客户端全部结束后写 quit 立刻收工（test.c 也认 stdin EOF ✓），
    # 不用像 test.sh 那样干等到"最后一个客户端 + 余量"的时刻（15 连接能省 ~10 s/轮 ✓）。
    local hold=$(( DUR + 30 + (ncl * 4 + 9) / 10 ))
    rm -f "$dir/stdin"; mkfifo "$dir/stdin" || { say "  ⚠ mkfifo 失败"; return 1; }
    timeout "$hold" stdbuf -oL "$BIN" --server=127.0.0.1:"$PORT" $srv_arg \
        < "$dir/stdin" > "$dir/server.log" 2>&1 &
    local spid=$!
    exec 9> "$dir/stdin"
    sleep 1
    cpu_start_local "$(pat_srv_lo "$BIN")" $((DUR + 25)) "$dir/cpu.server"; local cspid=$CPUPID
    cpu_start_local "$(pat_cli "$BIN" lo)" $((DUR + 25)) "$dir/cpu.client"; local cclid=$CPUPID
    local CPIDS="" TPID=0
    for c in $(seq 1 "$ncl"); do
        ( sleep "$DUR"; echo quit ) | timeout $((DUR + 15)) stdbuf -oL "$BIN" \
            --client=127.0.0.1:"$PORT" $cli_arg > "$dir/client$c.log" 2>&1 &
        CPIDS="$CPIDS $!"
        sleep 0.4
    done
    for p in $CPIDS; do wait "$p" 2>/dev/null || true; done
    echo quit >&9 2>/dev/null || true
    exec 9>&-
    wait $spid 2>/dev/null || true
    wait $cspid 2>/dev/null || true
    wait $cclid 2>/dev/null || true

    # 写成与 test.sh 相同的 summary.csv 格式 ⇒ 后面一套解析通吃 ✓
    local csv="$dir/summary.csv"
    echo 'stamp,tag,role,resend_pct,waste_pct,rtts_ms,send_recv_MBps' > "$csv"
    c=1
    for f in "$dir"/client*.log; do
        local role="client"; [ "$ncl" -gt 1 ] && role="client$c"
        printf '%s,%s,%s,%s,%s\n' "$st" "lo" "$role" "$(lo_metrics "$f")" >> "$csv"
        c=$((c + 1))
    done
    printf '%s,%s,server,%s\n' "$st" "lo" "$(lo_metrics "$dir/server.log")" >> "$csv"
}

lo_metrics() {  # $1=日志 ⇒ "res,waste,rtts,up,down"（取最后一个 dump ✓ 与 test.sh 同口径）
    awk '
        function trim(s) { gsub(/^[ \t]+|[ \t]+$/, "", s); return s }
        /^counter_resend_ratio *:/ { if (match($0, /[0-9]+\.[0-9]+%/)) res = substr($0, RSTART, RLENGTH - 1) }
        /^counter_resend_waste *:/ { if (match($0, /[0-9]+\.[0-9]+%/)) wa  = substr($0, RSTART, RLENGTH - 1) }
        /^rtt_min, rtts, rttd/     { l = $0; sub(/^[^:]*:[ \t]*/, "", l); split(l, a, ","); rt = trim(a[2]) }
        /^total_send, total_recv:/ { s = $3; r = $4; gsub(/MB,?/, "", s); gsub(/MB/, "", r) }
        /^conn age/                { ca = $4 }
        /^verify: OK/              { vok = 1 }
        END {
            # 必须写成 "up/down" 一格（与 test.sh 的 summary.csv 同格式 ✓ —— 写成两个字段后面全错位 ✗）
            if (ca + 0 > 0 && s + 0 > 0) printf "%s,%s,%s,%.2f/%.2f\n", \
                (res == "" ? "-" : res), (wa == "" ? "-" : wa), (rt == "" ? "-" : rt), s / ca, r / ca
            else printf "%s,%s,%s,-/-\n", (res == "" ? "-" : res), (wa == "" ? "-" : wa), (rt == "" ? "-" : rt)
        }' "$1"
}

run_ap() {  # $1=kind $2=方向 $3=上行% $4=下行% $5=目录 $6=stamp
    local kind="$1" dmode="$2" up_pct="$3" dn_pct="$4" dir="$5" st="$6" ncl
    ncl=1; [ "$kind" = full ] && ncl="$CLIENTS"
    mkdir -p "$dir"
    # 真机走 test.sh 的**总量**限速：它按 CLIENTS 均分，正好是"总带宽 ÷ 连接数" ✓
    local up_arg="" dn_arg=""
    [ "$up_pct" != 0 ] && [ "${REF_UP:-0}" != 0 ] && up_arg="UP_TOTAL=$(_pct "$REF_UP" "$up_pct")"
    [ "$dn_pct" != 0 ] && [ "${REF_DN:-0}" != 0 ] && dn_arg="DOWN_TOTAL=$(_pct "$REF_DN" "$dn_pct")"
    # CPU：客户端（本机，多进程合计）+ 服务端（远端单进程）
    cpu_start_local "$(pat_cli "$BIN" "$CUR_ENV")" $((DUR + 45)) "$dir/cpu.client"; local cclid=$CPUPID
    cpu_start_remote "$(pat_srv_ap "$BIN")" $((DUR + 45)) "$dir/cpu.server";   local cspid=$CPUPID
    env LOGDIR="$dir" STAMP="$st" KEEP_MUX=1 CLIENTS="$ncl" DIR="$dmode" DUMP_MS="$DUMP_MS" \
        $up_arg $dn_arg ./test.sh "$BIN" "$TARGET" "$PASS" "$DUR" > "$dir/testsh.out" 2>&1 \
        || say "  ⚠ test.sh 非零退出（看 $dir/testsh.out）"
    wait $cclid 2>/dev/null || true
    wait $cspid 2>/dev/null || true
}

# ── TCP 对照（满连接）：iperf3 -P CLIENTS ─────────────────────────────────────
# 只跑 100%：iperf3 的 -b 只对 UDP 生效（TCP 直接忽略）⇒ 20/80 档没有等价的 TCP 能力，
# 那几档只有 ffrdp（报告里必须写"该档无 TCP 对照"，不能拿满速 TCP 去比限速 ffrdp ✗）。
# ⚠ CPU 口径：iperf3 是**一个进程 15 条流**，ffrdp 是**15 个进程** ⇒ 客户端 CPU 不可直接比；
#   公平的对照看服务端 CPU 与整机 CPU。
tcp_run() {  # $1=环境 $2=方向 up|down|both $3=目录 $4=场景名
    local e="$1" dmode="$2" dir="$3" sc="$4" h; h="$(env_host "$e")"
    local par="$CLIENTS" up="-" dn="-" cli="-" csrv="-" pf=""
    mkdir -p "$dir"
    case "$dmode" in
    up)   up="$(iperf_one "$e" ""     "$dir/iperf-up.txt"    "$par")"
          pf="$dir/iperf-up.txt.ping";   dn=0 ;;
    down) dn="$(iperf_one "$e" "-R"   "$dir/iperf-down.txt"  "$par")"
          pf="$dir/iperf-down.txt.ping"; up=0 ;;
    *)    local bd; bd="$(iperf_bidir "$e" "$dir/iperf-bidir.txt" "$par")"
          up="${bd%% *}"; dn="${bd##* }"
          pf="$dir/iperf-bidir.txt.ping" ;;
    esac
    # CPU 文件与 ping 同前缀（iperf_one/iperf_bidir 都写 <out>.cli.cpu / .srv.cpu）
    local b="${pf%.ping}"
    cli="$(cpu_get "$b.cli.cpu" avg)"; csrv="$(cpu_get "$b.srv.cpu" avg)"
    CU["$e/$sc/client"]="$(cpu_fmt "$b.cli.cpu")"; CU["$e/$sc/server"]="$(cpu_fmt "$b.srv.cpu")"
    # TCP 侧没有协议层 resend/rtt 可读 ⇒ resend 列用 ping 丢包率、rtts 列用 ping 平均延迟 ✓
    local loss rt
    loss="$(grep -o '[0-9.]*% packet loss' "$pf" 2>/dev/null | tail -1 | cut -d% -f1)"
    rt="$(grep -o 'rtt min/avg/max[^=]*= *[0-9./]*' "$pf" 2>/dev/null | tail -1 | sed 's/.*= *//' | cut -d/ -f2)"
    say "  [tcp] $sc → 发/收 ${up}/${dn} MB/s  ping 丢包 ${loss:--}%  rtt ${rt:--} ms  CPU ${cli}/${csrv}%"
    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s\n' "$e" "$sc" "${loss:--}%" "${rt:--}" "$up" "$dn" "$cli" "$csrv" \
        "tcp -P $par; resend=ping 丢包 / rtts=ping 平均" >> "$OUT/results.csv"
    record "$e" "$sc" "${loss:--}%" "${rt:--}" "$up" "$dn" "$cli" "$csrv" "tcp -P $par"
}

# ── 汇总：从 summary.csv 取一格场景的四个数 ──────────────────────────────────
agg_csv() {  # $1=csv $2=single|full ⇒ "res,waste,rtts,up,down,n,reconn"
    awk -F, -v kind="$2" '
        function pct(s) { gsub(/%/, "", s); return s + 0 }
        NR == 1 { next }
        $3 ~ /^client/ {
            n++
            split($7, a, "/")
            su += a[1]; sd += a[2]
            r = pct($4); if (r > res) res = r
            w = pct($5); if (w > wa)  wa  = w
            t = $6 + 0;  if (t > rt)  rt  = t
        }
        END {
            if (n == 0) { print "-,-,-,-,-,0,0"; exit }
            printf "%.2f,%.2f,%d,%.2f,%.2f,%d,%d\n", res, wa, rt, su, sd, n, 0
        }' "$1"
}

# ── 场景矩阵：方向 × 带宽比例 ────────────────────────────────────────────────
# 每档是 "方向:上行%:下行%"，0 = 该方向完全不发流量（用 test.c 的 --send_only/--recv_only，
# **不是** --send_speed=0 —— 后者只是关掉令牌桶 ⇒ 全速发 ✗）。
# 11 档：三个 100%、三个 80%、三个 20%、两种非对称（80/20 与 20/80）；
# 单连接与 15 连接各跑一遍 ⇒ 22 次测试/环境。
SCEN_TABLE="up:100:0 down:0:100 both:100:100 up:80:0 down:0:80 both:80:80 up:20:0 down:0:20 both:20:20 both:80:20 both:20:80"

scen_name() {  # $1=方向 $2=上行% $3=下行% ⇒ 场景名（single/full 由调用方加前缀）
    if [ "$1" = both ] && [ "$2" != "$3" ]; then printf 'asym-%s-%s' "$2" "$3"
    elif [ "$1" = up ];   then printf 'up-%s'   "$2"
    elif [ "$1" = down ]; then printf 'down-%s' "$3"
    else                       printf 'both-%s' "$2"; fi
}

scen_title() {  # $1=场景名 ⇒ 人类可读的标题（summary.md 用）
    local sc="$1" kp who rest a
    case "$sc" in
    base)      echo "基准（ping 丢包/延时 + iperf3 TCP 100%，单连接）"; return ;;
    tcp-f-up)  echo "TCP 对照 · 满连接 · 只上行 · 100%"; return ;;
    tcp-f-down) echo "TCP 对照 · 满连接 · 只下行 · 100%"; return ;;
    tcp-f-both) echo "TCP 对照 · 满连接 · 双向对称 · 100%"; return ;;
    esac
    kp="${sc%%-*}"; who="单连接"; [ "$kp" = f ] && who="满连接"
    rest="${sc#*-}"
    case "$rest" in
    up-*)   echo "$who · 只上行 · ${rest#up-}%" ;;
    down-*) echo "$who · 只下行 · ${rest#down-}%" ;;
    both-*) echo "$who · 双向对称 · ${rest#both-}%" ;;
    asym-*) a="${rest#asym-}"; echo "$who · 双向非对称 · 上 ${a%-*}% / 下 ${a#*-}%" ;;
    *)      echo "$sc" ;;
    esac
}

declare -A V   # V["env/scen/metric"]
declare -A BB  # BB["env"]="上行/下行"：iperf3 双向基线（单向读数在 base.csv）
declare -A LP  # LP["env"]="发 / 收 / 双向"：吞吐**期间**的 ping avg（空载值在 base.csv）
declare -A CU  # CU["env/scen/client|server"]="min/max/avg/σ²"：CPU 分布（avg 另在 results.csv）

cpu_fmt() {  # $1=cpu 文件 ⇒ "min/max/avg/σ²"（缺失 = "-"）
    printf '%s/%s/%s/%s' "$(cpu_get "$1" min)" "$(cpu_get "$1" peak)" \
                        "$(cpu_get "$1" avg)" "$(cpu_get "$1" var)"
}

record() {  # $1=env $2=scen $3=res $4=rtt $5=up $6=down $7=cpucl $8=cpusrv $9=note
    V["$1/$2/resend"]="$3"; V["$1/$2/rtts"]="$4"
    V["$1/$2/up"]="$5"; V["$1/$2/down"]="$6"
    V["$1/$2/cpucl"]="$7"; V["$1/$2/cpusrv"]="$8"; V["$1/$2/note"]="${9:-}"
}

# ── 主流程 ──────────────────────────────────────────────────────────────────
[ "$DRY" = 1 ] || mkdir -p "$OUT/raw"
echo "════════════════════════════════════════════════════════════════"
echo " 性能评测  $OUT"
echo " 环境 : $ENVS"
echo " 场景 : $SCEN"
echo " 参数 : DUR=${DUR}s  CLIENTS=${CLIENTS}  REPEAT=$REPEAT  BUILD=$BUILD  dump=${DUMP_MS}ms  TCP=$TCP"
echo "════════════════════════════════════════════════════════════════"
[ -f "$BIN" ] || die "找不到二进制: $BIN（先 bash build.sh $BUILD）"

# 编一次，两端用同一个 md5（test.sh 会自己上传这个文件 ✓）
if [ "$DRY" != 1 ]; then
    say "编译 $BUILD 档 ..."
    TOOLCHAIN_NAME="$TOOLCHAIN_NAME" bash build.sh "$BUILD" > "$OUT/build.log" 2>&1 \
        || die "编译失败，看 $OUT/build.log"
    [ "$BIN" = "./test_ffrdp" ] || cp -f test_ffrdp "$BIN"
    say "md5 $(md5sum "$BIN" | awk '{print $1}')"
    printf 'env,loss_pct,ping_rtt,up_MBps,down_MBps,cpu_cli_avg,cpu_srv_avg\n' > "$OUT/base.csv"
    printf 'env,up_MBps,down_MBps\n' > "$OUT/base-bidir.csv"
    printf 'env,phase,loss_pct,ping_rtt\n' > "$OUT/base-load.csv"
    printf 'env,scen,resend_pct,rtts_ms,up_MBps,down_MBps,cpu_cli_avg,cpu_srv_avg,note\n' > "$OUT/results.csv"
fi

SKIP_IPERF="${SKIP_IPERF:-0}"
REF_UP=0; REF_DN=0

for e in $ENVS; do
    CUR_ENV="$e"
    # 参照容量**只属于本环境**：必须在每个环境开头清零。否则沿用上一个 AP 的数（基线没读到、
    # 或 REF_MODE=self 时），快 AP 的 80% 档会按慢 AP 的容量下发 ⇒ 变成 1/30 的限速，
    # 看着像"限速没生效"，其实是参照串了 ✗。
    REF_UP=0; REF_DN=0
    say "════════ 环境：$(env_name "$e") ════════"
    wait_switch "$e"
    [ "$DRY" != 1 ] && is_remote "$e" && ssh_setup "$e"

    # ── TCP 基线（iperf3，100% 三个方向）──
    # 为什么不按 20/80% 跑：iperf3 的 -b 只对 UDP 生效，TCP 会忽略它 ⇒ 没有等价的限速能力。
    # 所以 TCP 只做 100% 的三方向对照，20/80 档只有 ffrdp（报告里要标明"该档无 TCP 对照"）。
    if [ "$TCP" = 1 ]; then
        say "[base] ping + iperf3"
        if [ "$DRY" = 1 ]; then
            echo "  (DRY) ping -c $PINGCOUNT $e ; iperf3 -c $(env_host "$e") -t $IPERF_TIME（上行 + -R 下行）"
        else
            base_run "$e" "$OUT/raw/$e-base"
            # 参照：真机用 iperf3（KB/s），lo 用 ffrdp 自身单流满速（后面回填）
            if is_remote "$e" && [ "${BASE_UP_MBPS:-"-"}" != "-" ] && [ "${BASE_DN_MBPS:-"-"}" != "-" ]; then
                REF_UP="$(kb_of "$BASE_UP_MBPS")"
                REF_DN="$(kb_of "$BASE_DN_MBPS")"
                [ "$REF_MODE" = self ] && { REF_UP=0; REF_DN=0; }
                say "  参照容量：上 ${REF_UP}KB/s 下 ${REF_DN}KB/s（iperf3）"
            else
                say "  参照容量：稍后用 ffrdp 单流满速（self）"
            fi
            record "$e" base "$BASE_LOSS%" "${BASE_RTT:-?}" "${BASE_UP_MBPS:-?}" "${BASE_DN_MBPS:-?}" \
                   "$(cpu_get "$OUT/raw/$e-base/iperf-up.txt.cli.cpu" avg)" "$(cpu_get "$OUT/raw/$e-base/iperf-up.txt.srv.cpu" avg)" \
                   "ping 丢包 / ping RTT / iperf3 TCP"
            CU["$e/base/client"]="$(cpu_fmt "$OUT/raw/$e-base/iperf-up.txt.cli.cpu")"
            CU["$e/base/server"]="$(cpu_fmt "$OUT/raw/$e-base/iperf-up.txt.srv.cpu")"
            confirm_base "$e" "$BASE_LOSS" "$(echo "${BASE_RTT:-/}" | cut -d/ -f2)" "$BASE_DN_MBPS"
        fi
        # 满连接 TCP 对照（单连接三方向已经由 base_run 跑过 ⇒ 这里只差 -P）
        for dm in up down both; do
            sc="tcp-f-$dm"
            [ "$SCEN" = all ] || echo " $SCEN " | grep -q " $sc " || continue
            if [ "$DRY" = 1 ]; then
                echo "  (DRY) iperf3 -c $(env_host "$e") -P $CLIENTS（方向 $dm，${IPERF_TIME}s）"
                continue
            fi
            tcp_run "$e" "$dm" "$OUT/raw/$e-$sc" "$sc"
        done
    fi

    # ── ffrdp：11 档（方向 × 比例），单连接与满连接各一遍 ──
    for kind in single full; do
        kp=s; [ "$kind" = full ] && kp=f
        for spec in $SCEN_TABLE; do
            IFS=: read -r dmode up_pct dn_pct <<< "$spec"
            sc="$kp-$(scen_name "$dmode" "$up_pct" "$dn_pct")"
            [ "$SCEN" = all ] || echo " $SCEN " | grep -q " $sc " || continue
            for rep in $(seq 1 "$REPEAT"); do
                local_st="$sc"; [ "$REPEAT" -gt 1 ] && local_st="$sc-r$rep"
                dir="$OUT/raw/$e-$local_st"
                nn=1; [ "$kind" = full ] && nn="$CLIENTS"
                say "[$e/$sc] 方向=$dmode 上=${up_pct}% 下=${dn_pct}% ${DUR}s ×${nn} 连接"
                # 参照没定就别假装限速了：REF 只在单连接 both-100 档回填（或用 iperf3/REF_MODE=self）
                if { [ "$up_pct" != 0 ] && [ "$up_pct" != 100 ] && [ "${REF_UP:-0}" = 0 ]; } \
                    || { [ "$dn_pct" != 0 ] && [ "$dn_pct" != 100 ] && [ "${REF_DN:-0}" = 0 ]; }; then
                    say "  ⚠ 参照容量未定（REF_UP=${REF_UP:-0} REF_DN=${REF_DN:-0}）⇒ 本档**不限速**；先跑 s-both-100 或设 REF_MODE=self"
                fi
                if [ "$DRY" = 1 ]; then
                    echo "  (DRY) $( [ "$e" = lo ] && echo "本地两进程" || echo "./test.sh $BIN $TARGET …" ) \
LOGDIR=$dir STAMP=$local_st CLIENTS=$nn DIR=$dmode DUMP_MS=$DUMP_MS"
                    continue
                fi
                if [ "$e" = lo ]; then
                    run_lo "$kind" "$dmode" "$up_pct" "$dn_pct" "$dir" "$local_st"
                else
                    run_ap "$kind" "$dmode" "$up_pct" "$dn_pct" "$dir" "$local_st"
                fi
                IFS=, read -r res wa rt up dn n _ < <(agg_csv "$dir/summary.csv" "$kind")
                ccl="$(cpu_get "$dir/cpu.client" avg)"; csrv="$(cpu_get "$dir/cpu.server" avg)"
                cclp="$(cpu_get "$dir/cpu.client" peak)"; csrvp="$(cpu_get "$dir/cpu.server" peak)"
                clog="$(client_logs "$dir" | head -1)"
                nctx=0
                [ -n "$clog" ] && nctx="$(grep -c 'ffrdp_init: ' "$clog" 2>/dev/null || true)"
                nctx="${nctx:-0}"
                [ "$nctx" -le 1 ] && nctx=0 || nctx=$((nctx - 1))
                note="conns=$n"
                [ "$nctx" -gt 0 ] && note="$note; 重连 $nctx 次"
                verify="$(client_logs "$dir" | tr '\n' '\0' | xargs -0 -r grep -l 'verify: OK' 2>/dev/null | wc -l)"
                note="$note; verify $verify/$n"
                say "  → resend ${res}%  rtts ${rt}ms  发/收 ${up}/${dn} MB/s  CPU ${ccl}/${csrv}%（峰值 ${cclp}/${csrvp}）  $note"
                printf '%s,%s,%s,%s,%s,%s,%s,%s,%s\n' "$e" "$sc" "$res" "$rt" "$up" "$dn" "$ccl" "$csrv" "$note" >> "$OUT/results.csv"
                if [ "$rep" = 1 ]; then
                    record "$e" "$sc" "${res}%" "${rt}" "$up" "$dn" "$ccl" "$csrv" "$note"
                    CU["$e/$sc/client"]="$(cpu_fmt "$dir/cpu.client")"
                    CU["$e/$sc/server"]="$(cpu_fmt "$dir/cpu.server")"
                    # 参照：单连接**双向满速**（both-100）—— 只有这一档同时压满上下行，两个方向才都代表容量。
                    # lo 恒用它（loopback 没有外部瓶颈，iperf3 的容量对 ffrdp 无意义）；
                    # 真机在 REF_MODE=self 时同样回填（单向 iperf3 高于 ffrdp 单流上限时只能拿自身当参照）。
                    # ⚠ 条件里必须带 REF_MODE=self：若只认 lo，真机 REF_MODE=self 会把 REF 清成 0
                    #   ⇒ 限速档的 `[ REF_UP != 0 ]` 不成立 ⇒ **完全不限速**，白跑一轮 ✗
                    if [ "$kind" = single ] && [ "$dmode" = both ] \
                        && [ "$up_pct" = 100 ] && [ "$dn_pct" = 100 ] && [ "${REF_UP:-0}" = 0 ] \
                        && { [ "$e" = lo ] || [ "$REF_MODE" = self ]; }; then
                        REF_UP="$(kb_of "$up")"; REF_DN="$(kb_of "$dn")"
                        say "  $e 参照容量（ffrdp 单流满速）：上 ${REF_UP}KB/s 下 ${REF_DN}KB/s"
                    fi
                fi
            done
        done
    done
done

[ "$DRY" = 1 ] && { echo; echo "(DRY) 结束，什么都没跑"; exit 0; }

# ── summary.md ──────────────────────────────────────────────────────────────
# 表顺序：基准 → TCP 满连接三方向 → 单连接 11 档 → 满连接 11 档
SCENS="base tcp-f-up tcp-f-down tcp-f-both"
for kp in s f; do
    for spec in $SCEN_TABLE; do
        IFS=: read -r dmode up_pct dn_pct <<< "$spec"
        SCENS="$SCENS $kp-$(scen_name "$dmode" "$up_pct" "$dn_pct")"
    done
done
ENVCOLS=($ENVS)
{
    echo "# 性能评测结果（$(date '+%Y-%m-%d %H:%M')）"
    echo
    echo "- 二进制：\`$(md5sum "$BIN" | awk '{print $1}')\`（BUILD=$BUILD，两端同一份）"
    echo "- 每轮 ${DUR}s，满连接 ${CLIENTS} 条，dump 每 ${DUMP_MS} ms，REPEAT=$REPEAT"
    echo "- 原始数据：\`results.csv\` / \`base.csv\` / \`base-bidir.csv\` / \`base-load.csv\` / \`raw/\`"
    echo
    for sc in $SCENS; do
        # SCEN 过滤掉的档别出空表（否则 summary 里全是 "-"，像工具坏了 ✗）
        [ -z "${V["${ENVCOLS[0]}/$sc/up"]:-}" ] && continue
        echo "## $(scen_title "$sc")"
        echo
        if [ "$sc" = base ]; then
            printf '| 指标 |'; for e in "${ENVCOLS[@]}"; do printf ' %s |' "$(env_name "$e")"; done; echo
            printf '|---|'; for e in "${ENVCOLS[@]}"; do printf '%s' '---|'; done; echo
            printf '| resend / 丢包率 |'; for e in "${ENVCOLS[@]}"; do printf ' %s |' "${V["$e/base/resend"]:--}"; done; echo
            printf '| rtts / ping 延时（空载）|'; for e in "${ENVCOLS[@]}"; do printf ' %s |' "${V["$e/base/rtts"]:--}"; done; echo
            printf '| ping @ 吞吐（avg，发/收/双向）|'; for e in "${ENVCOLS[@]}"; do printf ' %s |' "${LP["$e"]:--}"; done; echo
            printf '| 吞吐量（iperf3 单向，发/收）|'; for e in "${ENVCOLS[@]}"; do printf ' %s MB/s |' "${V["$e/base/up"]:--}/${V["$e/base/down"]:--}"; done; echo
            printf '| 吞吐量（iperf3 双向，发/收）|'; for e in "${ENVCOLS[@]}"; do printf ' %s MB/s |' "${BB["$e"]:--}"; done; echo
            printf '| cpu 客户端（min/max/avg/σ²）|'; for e in "${ENVCOLS[@]}"; do printf ' %s |' "${CU["$e/base/client"]:--}"; done; echo
            printf '| cpu 服务端（min/max/avg/σ²）|'; for e in "${ENVCOLS[@]}"; do printf ' %s |' "${CU["$e/base/server"]:--}"; done; echo
        else
            printf '| 指标 |'; for e in "${ENVCOLS[@]}"; do printf ' %s |' "$(env_name "$e")"; done; echo
            printf '|---|'; for e in "${ENVCOLS[@]}"; do printf '%s' '---|'; done; echo
            printf '| resend（最差那条）|'; for e in "${ENVCOLS[@]}"; do printf ' %s |' "${V["$e/$sc/resend"]:--}"; done; echo
            printf '| rtts（最差那条，ms）|'; for e in "${ENVCOLS[@]}"; do printf ' %s |' "${V["$e/$sc/rtts"]:--}"; done; echo
            printf '| 吞吐量（MB/s，发/收）|'; for e in "${ENVCOLS[@]}"; do printf ' %s |' "${V["$e/$sc/up"]:--}/${V["$e/$sc/down"]:--}"; done; echo
            printf '| cpu 客户端（min/max/avg/σ²）|'; for e in "${ENVCOLS[@]}"; do printf ' %s |' "${CU["$e/$sc/client"]:--}"; done; echo
            printf '| cpu 服务端（min/max/avg/σ²）|'; for e in "${ENVCOLS[@]}"; do printf ' %s |' "${CU["$e/$sc/server"]:--}"; done; echo
            printf '| 备注 |'; for e in "${ENVCOLS[@]}"; do printf ' %s |' "${V["$e/$sc/note"]:--}"; done; echo
        fi
        echo
    done
} > "$OUT/summary.md"

echo
echo "════════════════════════════════════════════════════════════════"
echo " 结果：$OUT/summary.md（按 docs/perf-eval-template.md 抄成报告）"
echo " 明细：$OUT/results.csv / $OUT/base.csv / $OUT/raw/"
echo "════════════════════════════════════════════════════════════════"