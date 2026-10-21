#!/usr/bin/env bash
# tools/perf-tables.sh —— 把一轮/多轮 perf-eval 产物直接渲染成 markdown 表（报告正文用）
#
# 为什么要有它：报告里的表格**不手写**。手抄一次就会有一次口径漂移（上一版就出过
# "TCP both 档 -/- 被当成 0" 和 "lo 下行全是 0" 这种事故）。这里所有数字都从
#   results.csv / detail.csv / raw/<场景>/ 下的日志与采样文件 现算现印。
#
# 口径（与 perf-stats.sh 完全一致，报告里照抄）：
#   · 单连接：同一连接的时间序列（5 s 一个 dump 区间，丢首区间）⇒ min/avg/max/std
#   · 多连接：每条连接先压成时间均值 ⇒ 再跨 15 条算 min/avg/max/std；"合计"另给
#     同一区间序号跨连接相加后的时间序列（detail.csv 的 total 行）
#   · 两个栈**同口径**：吞吐都用"丢首区间的区间均值"，所以 ffrdp 与 iperf3 可直接同表
#     （results.csv 里的是整段平均，和这里的 avg 会有 ≤2% 差，两处都对，只是问法不同）
#   · ping：iperf3 侧来自 `iperf-*.txt.ping`（吞吐期间并发 ping）；ffrdp 侧来自
#     `ping.txt`（perf-eval.sh 从 v2.1 起也会采）——老日志没有就印 `待采` ✗ 不要当 0
#   · CPU：进程 utime+stime，多进程（15 个客户端）是合计；std 由 cpu_loop 的 var 开方
#
# 用法:
#   bash tools/perf-tables.sh logs/perf-1020-042012 logs/perf-1020-210704 logs/perf-1019-231318
#   bash tools/perf-tables.sh --out tables.md <上面的目录...>
set -u

die() { echo "✗ $*" >&2; exit 2; }

GEARS="up-100 down-100 both-100 up-80 down-80 both-80 up-20 down-20 both-20 asym-80-20 asym-20-80"
OUT=""

while [ $# -gt 0 ]; do
    case "$1" in
    --out) OUT="$2"; shift 2 ;;
    -*)    die "未知参数 $1" ;;
    *)     break ;;
    esac
done
[ "$#" -ge 1 ] || die "用法: bash tools/perf-tables.sh [--out 文件] <perf 目录> [更多目录]"

emit() { if [ -n "$OUT" ]; then printf '%s\n' "$*" >> "$OUT"; else printf '%s\n' "$*"; fi }
[ -n "$OUT" ] && : > "$OUT"

# ── 小工具 ───────────────────────────────────────────────────────────────────
f2() {  # $1=值 $2=小数位 ⇒ 数字（缺值统一印 "—"）
    case "${1:-}" in
    ''|'-'|'?') printf '—' ;;
    *) awk -v v="$1" -v d="${2:-2}" 'BEGIN { printf "%.*f", d, v + 0 }' ;;
    esac
}
pm() {  # $1=avg $2=std $3=小数位 ⇒ "avg±std" / "—"
    case "${1:-}" in ''|'-'|'?') printf '—' ;; *)
        if [ -z "${2:-}" ] || [ "${2:-}" = "-" ]; then f2 "$1" "${3:-2}"
        else printf '%s±%s' "$(f2 "$1" "${3:-2}")" "$(f2 "$2" "${3:-2}")"; fi ;;
    esac
}

# ping 文件 ⇒ "loss avg std p95"（没有样本 ⇒ "- - - -"）
ping_stats() {
    [ -s "${1:-}" ] || { printf -- '- - - -\n'; return; }
    awk '
        /packet loss/ { for (i = 1; i <= NF; i++) if ($i ~ /%$/) { l = $(i - 1) + 0; sub(/%/, "", l) } }
        /time=[0-9.]+ *ms/ {
            if (match($0, /time=[0-9.]+/)) {
                v = substr($0, RSTART + 5, RLENGTH - 5) + 0
                n++; s += v; q += v * v
                if (n == 1 || v < mn) mn = v
                if (n == 1 || v > mx) mx = v
                a[n] = v
            }
        }
        END {
            if (n == 0) { printf "- - - -\n"; exit }
            # 手工插入排序求 p95（样本 ~150，不值得上外部 sort）
            for (i = 2; i <= n; i++) { k = a[i]; j = i - 1; while (j >= 1 && a[j] > k) { a[j + 1] = a[j]; j-- } a[j + 1] = k }
            idx = int(n * 0.95); if (idx < 1) idx = 1; if (idx > n) idx = n
            av = s / n; sd = sqrt((q / n) - av * av); if (sd != sd) sd = 0
            printf "%.1f %.1f %.1f %.1f\n", l, av, sd, a[idx]
        }' "$1"
}

# iperf3 原始输出 ⇒ "avg std"（MB/s，丢首区间；汇总行/无数据 ⇒ "- -"）
iperf_tput() {  # $1=文件 $2=角色（可空；--bidir 用 TX-C / RX-C）
    [ -s "${1:-}" ] || { printf -- '- -\n'; return; }
    awk -v role="${2:-}" '
        function mbps(n, u) {
            return (u ~ /Gbits/) ? n * 1000 / 8 : (u ~ /Mbits/) ? n / 8 : (u ~ /Kbits/) ? n / 8000 : n / 8
        }
        /bits\/sec/ && /sec/ && /^\[/ {
            if ($0 ~ /(sender|receiver)[ \t]*$/) next          # 汇总行，不是区间
            if (role != "" && index($0, "[" role "]") == 0) next
            n = ""; u = ""
            for (i = 1; i < NF; i++) if ($(i + 1) ~ /^[KMG]?bits\/sec$/) { n = $(i); u = $(i + 1) }
            if (n == "") next
            v = mbps(n, u)
            # -P >1 时每个区间有 15 行（每流一行）+ 1 行 [SUM] ⇒ 必须只吃 [SUM]，
            # 否则得到的是"每流速率"（慢 AP 会被算成 0.06 而不是 0.38 ✗）
            if (index($0, "[SUM]") > 0) sum[++sn] = v
            else                        one[++pn] = v
        }
        END {
            if (sn > 0) { for (i = 2; i <= sn; i++) use[++m] = sum[i] }      # 丢首区间（爬坡）
            else        { for (i = 2; i <= pn; i++) use[++m] = one[i] }
            if (m == 0) { printf "- -\n"; exit }
            for (i = 1; i <= m; i++) { s += use[i]; q += use[i] * use[i] }
            av = s / m; sd = sqrt((q / m) - av * av); if (sd != sd) sd = 0
            printf "%.3f %.3f\n", av, sd
        }' "$1"
}

# cpu 采样文件（cpu_loop 的汇总）⇒ "avg std"
cpu_stats() {  # $1=文件
    [ -s "${1:-}" ] || { printf -- '- -\n'; return; }
    awk -F= '
        $1 == "avg_pct_of_one_core" { av = $2 }
        $1 == "var_pct_of_one_core" { vr = $2 }
        END {
            if (av == "") { printf "- -\n"; exit }
            sd = (vr + 0 > 0) ? sqrt(vr + 0) : 0
            printf "%.0f %.0f\n", av, sd
        }' "$1"
}

# detail.csv ⇒ "avg std"（key: scen,conn,dir,metric）
det() {  # $1=detail.csv $2=scen $3=conn $4=dir $5=metric
    [ -s "${1:-}" ] || { printf -- '- -\n'; return; }
    awk -F, -v s="$2" -v c="$3" -v d="$4" -v m="$5" '
        $2 == s && $3 == c && $4 == d && $5 == m { printf "%s %s\n", $9, $11; f = 1 }
        END { if (!f) printf "- -\n" }' "$1"
}

# 在场景目录里找 ping 文件
#   iperf 侧：<iperf 输出文件>.ping（吞吐期间并发 ping）
#   ffrdp 侧：ping.txt（perf-eval.sh 从 v2.1 起才采；老日志没有 ⇒ 空）
find_ping() {  # $1=目录 $2=iperf 输出文件名（ffrdp 传空）
    local d="$1" base="${2:-}"
    if [ -s "$d/ping.txt" ]; then printf '%s\n' "$d/ping.txt"; return 0; fi
    if [ -n "$base" ] && [ -s "$d/$base.ping" ]; then printf '%s\n' "$d/$base.ping"; fi
    return 0
}

# ── 环境发现：一个目录 = 一个环境（lo / slow / fast）────────────────────────────
ENVS=""    # "key:dir:label" 三元组
for D in "$@"; do
    [ -d "$D/raw" ] || die "$D 下没有 raw/"
    key=""
    for rd in "$D"/raw/*/; do
        b="$(basename "$rd")"; k="${b%%-*}"
        [ "$k" = base ] && continue
        key="$k"; break
    done
    [ -n "$key" ] || die "$D 里找不到场景目录"
    case "$key" in
    lo)   label="lo（回环）" ;;
    slow) label="慢 AP" ;;
    fast) label="快 AP" ;;
    *)    label="$key" ;;
    esac
    ENVS="$ENVS$key:$D:$label
"
done

# 按 key 排序：lo → slow → fast（报告固定顺序）
ordered_envs() {
    printf '%s' "$ENVS" | awk -F: 'NF >= 3 { print }' | sort -t: -k1,1 | awk -F: '
        { want[NR] = $1; line[$1] = $0 }
        END { split("lo slow fast", o, " "); for (i = 1; i <= 3; i++) if (o[i] in line) print line[o[i]]
              for (k in line) if (k != "lo" && k != "slow" && k != "fast") print line[k] }'
}

# ── 单元格拼装 ───────────────────────────────────────────────────────────────
# 吞吐单元格：$1=up_avg $2=up_std $3=dn_avg $4=dn_std
tput_cell() { printf '%s / %s' "$(pm "$1" "$2" 2)" "$(pm "$3" "$4" 2)"; }

# 一行（栈 × 连接档）
row() {  # $1=label $2=dir $3=scenfull $4=stack(tcp|ffrdp) $5=conn $6=detail.csv $7=gear
    local label="$1" d="$2" sc="$3" stack="$4" conn="$5" detf="$6"
    # up / down / both（取自档位名，别从 sc 推）；asym-* 是"两个方向同时跑、各自限速" ⇒ 也算 both
    local phase
    case "$7" in up-*) phase=up ;; down-*) phase=down ;; *) phase=both ;; esac
    local up_a=- up_s=- dn_a=- dn_s=- loss=- lat_a=- lat_s=- lat_p=- ca=- cas=- sa=- sas=- fail=0

    if [ "$stack" = tcp ]; then
        # asym 两档 iperf3 做不到"一个方向 80%、另一个 20% 同时跑" ⇒ harness 顺序跑两遍，
        # 文件仍是 iperf-up.txt / iperf-down.txt（**没有** iperf-bidir.txt）
        local cfile
        case "$7" in
        up-*)   cfile="iperf-up.txt";   read -r up_a up_s <<<"$(iperf_tput "$d/iperf-up.txt" '')" ;;
        down-*) cfile="iperf-down.txt"; read -r dn_a dn_s <<<"$(iperf_tput "$d/iperf-down.txt" '')" ;;
        both-*) cfile="iperf-bidir.txt"
                read -r up_a up_s <<<"$(iperf_tput "$d/iperf-bidir.txt" 'TX-C')"
                read -r dn_a dn_s <<<"$(iperf_tput "$d/iperf-bidir.txt" 'RX-C')"
                # --bidir 失败时 harness 会降级成两条单向（iperf-up/down.txt）⇒ 这里回退读
                if [ "$up_a" = "-" ] && [ "$dn_a" = "-" ] && [ -s "$d/iperf-up.txt" ]; then
                    cfile="iperf-up.txt"
                    read -r up_a up_s <<<"$(iperf_tput "$d/iperf-up.txt" '')"
                    read -r dn_a dn_s <<<"$(iperf_tput "$d/iperf-down.txt" '')"
                fi ;;
        asym-*) cfile="iperf-up.txt"
                read -r up_a up_s <<<"$(iperf_tput "$d/iperf-up.txt" '')"
                read -r dn_a dn_s <<<"$(iperf_tput "$d/iperf-down.txt" '')" ;;
        esac
        # 采集失败要显式写出来：`-/-` 最坑的地方就是看起来像"测到了 0"
        if [ "$up_a" = "-" ] && [ "$dn_a" = "-" ]; then fail=1; fi
        local pf; pf="$(find_ping "$d" "$cfile")"
        if [ -n "${pf:-}" ]; then read -r loss lat_a lat_s lat_p <<<"$(ping_stats "$pf")"; fi
        read -r ca cas <<<"$(cpu_stats "$d/$cfile.cli.cpu")"
        read -r sa sas <<<"$(cpu_stats "$d/$cfile.srv.cpu")"
    else
        # ffrdp：吞吐用 detail.csv（single=时间序列 / full=total 行=15 条合计）
        local t
        if [ "$conn" = single ]; then
            if [ "$phase" = up ] || [ "$phase" = both ]; then
                read -r up_a up_s <<<"$(det "$detf" "$sc" single up tput)"
            fi
            if [ "$phase" = down ] || [ "$phase" = both ]; then
                read -r dn_a dn_s <<<"$(det "$detf" "$sc" single down tput)"
            fi
        else
            if [ "$phase" = up ] || [ "$phase" = both ]; then
                read -r up_a up_s <<<"$(det "$detf" "$sc" full up total)"
            fi
            if [ "$phase" = down ] || [ "$phase" = both ]; then
                read -r dn_a dn_s <<<"$(det "$detf" "$sc" full down total)"
            fi
        fi
        local pf; pf="$(find_ping "$d" "")"
        if [ -n "${pf:-}" ]; then read -r loss lat_a lat_s lat_p <<<"$(ping_stats "$pf")"; fi
        read -r ca cas <<<"$(cpu_stats "$d/cpu.client")"
        read -r sa sas <<<"$(cpu_stats "$d/cpu.server")"
    fi

    if [ "$loss" = "-" ]; then loss="待采"; else loss="${loss}%"; fi
    local tc
    if [ "$fail" = 1 ]; then tc="采集失败"; else tc="$(tput_cell "$up_a" "$up_s" "$dn_a" "$dn_s")"; fi
    emit "$(printf '| %s | `%s` | %s | %s | %s | %s | %s / %s |' \
        "$label" "$sc" "$tc" \
        "$loss" "$(pm "$lat_a" "$lat_s" 1)" "$(f2 "$lat_p" 1)" \
        "$(pm "$ca" "$cas" 0)" "$(pm "$sa" "$sas" 0)")"
}

table_envs() {  # $1=stack $2=conn
    local stack="$1" conn="$2"
    local hdr='| 环境 | 档位 | 吞吐 ↑ / ↓ (MB/s) | 丢包 (%) | 延迟 avg (ms) | 延迟 p95 (ms) | CPU cli / srv (%) |'
    local sep='|---|---|---|---|---|---|---|'
    emit ""
    emit "$hdr"
    emit "$sep"
    local line key D label sc detf dir
    while IFS= read -r line; do
        [ -n "$line" ] || continue
        key="$(printf '%s' "$line" | cut -d: -f1)"
        D="$(printf '%s' "$line" | cut -d: -f2)"
        label="$(printf '%s' "$line" | cut -d: -f3-)"
        detf="$D/detail.csv"
        for g in $GEARS; do
            # single → s-xxx；full → f-xxx（目录名与 detail.csv 的 scen 一致）
            local pre=s; [ "$conn" = full ] && pre=f
            # 目录名永远带环境前缀（fast-tcp-s-up-100）；detail.csv 的 scen 不带（s-up-100）
            if [ "$stack" = tcp ]; then sc="tcp-$pre-$g"; dir="$D/raw/$key-tcp-$pre-$g"
            else sc="$pre-$g"; dir="$D/raw/$key-$pre-$g"; fi
            [ -d "$dir" ] || continue
            row "$label" "$dir" "$sc" "$stack" "$conn" "$detf" "$g"
        done
    done <<EOF
$(ordered_envs)
EOF
}

# ── ffrdp 专属指标表 ────────────────────────────────────────────────────────
ffrdp_only() {  # $1=conn
    local conn="$1" title
    if [ "$conn" = single ]; then title="ffrdp 专属指标 · 单连接（时间序列 min/avg/max/std）"
    else title="ffrdp 专属指标 · 多连接（15 条各取时间均值，再跨连接统计）"; fi
    emit ""
    emit "### $title"
    emit ""
    if [ "$conn" = single ]; then
        emit '| 环境 | 档位 | 方向 | resend (%) | waste (%) | rtts (ms) | 在途上限 (帧) |'
        emit '|---|---|---|---|---|---|---|'
    else
        emit '| 环境 | 档位 | 方向 | resend (%) | waste (%) | rtts (ms) | 在途上限 (帧) | 吞吐 max/min |'
        emit '|---|---|---|---|---|---|---|---|'
    fi
    local line key D label sc dir g d
    while IFS= read -r line; do
        [ -n "$line" ] || continue
        key="$(printf '%s' "$line" | cut -d: -f1)"
        D="$(printf '%s' "$line" | cut -d: -f2)"
        label="$(printf '%s' "$line" | cut -d: -f3-)"
        local detf="$D/detail.csv"
        local pre=s; [ "$conn" = full ] && pre=f
        for g in $GEARS; do
            d="$D/raw/$key-$pre-$g"; [ -d "$d" ] || continue
            sc="$pre-$g"
            case "$g" in
            up-*)   for dir in up; do one_ff "$label" "$detf" "$sc" "$conn" "$dir"; done ;;
            down-*) for dir in down; do one_ff "$label" "$detf" "$sc" "$conn" "$dir"; done ;;
            *)      for dir in up down; do one_ff "$label" "$detf" "$sc" "$conn" "$dir"; done ;;
            esac
        done
    done <<EOF
$(ordered_envs)
EOF
}

one_ff() {  # $1=label $2=detail $3=scen $4=conn $5=dir
    local ra rs wa ws rt rts ca cas t
    read -r ra rs <<<"$(det "$2" "$3" "$4" "$5" resend)"
    read -r wa ws <<<"$(det "$2" "$3" "$4" "$5" waste)"
    read -r rt rts <<<"$(det "$2" "$3" "$4" "$5" rtts)"
    read -r ca cas <<<"$(det "$2" "$3" "$4" "$5" incap)"
    [ "$ra" = "-" ] && [ "$wa" = "-" ] && return 0     # 该方向没跑
    if [ "$4" = full ]; then
        local ta ts
        read -r ta ts <<<"$(det "$2" "$3" full "$5" tput)"
        local ar mk
        read -r ar mk <<<"$(awk -F, -v s="$3" -v d="$5" '
            $2 == s && $3 == "full" && $4 == d && $5 == "tput" { printf "%s %s\n", $7, $8 }' "$2")"
        # 公平性 = 15 条里最快/最慢（1.00× = 完全均匀；越大越不公）
        local fair="—"
        case "$mk" in ''|-|0|0.000) ;; *) fair="$(awk -v lo="$ar" -v hi="$mk" 'BEGIN { printf "%.2f×", (lo + 0 > 0) ? (hi + 0) / (lo + 0) : 0 }')" ;; esac
        emit "$(printf '| %s | `%s` | %s | %s | %s | %s | %s | %s |' \
            "$1" "$3" "$5" "$(pm "$ra" "$rs" 2)" "$(pm "$wa" "$ws" 1)" "$(pm "$rt" "$rts" 1)" "$(pm "$ca" "$cas" 1)" "$fair")"
    else
        emit "$(printf '| %s | `%s` | %s | %s | %s | %s | %s |' \
            "$1" "$3" "$5" "$(pm "$ra" "$rs" 2)" "$(pm "$wa" "$ws" 1)" "$(pm "$rt" "$rts" 1)" "$(pm "$ca" "$cas" 1)")"
    fi
}

# ── 内存配置（固定分配：只报"当前版本的配置"，不做采样）───────────────────────
mem_block() {
    # 找任意一份含 footprint 的日志即可（编译期常量与连接数无关）
    local f="" line key D
    while IFS= read -r line; do
        [ -n "$line" ] || continue
        D="$(printf '%s' "$line" | cut -d: -f2)"
        f="$(grep -l footprint "$D"/raw/*/*.log 2>/dev/null | head -1)"
        [ -n "$f" ] && break
    done <<EOF
$(ordered_envs)
EOF
    [ -n "$f" ] || { emit ""; emit "（未找到带 footprint 的日志）"; return; }

    # ⚠ 两个坑：① 每行里还有别的冒号（"compile-time macros: ..."）⇒ 只能切第一个冒号；
    #          ② 新建连接的那几帧 dump 是 "snd - 0/64 slots x 0 B"（环还没分配）⇒ 取 `snd raw`
    #             那几行（已分配）的最后一条，否则会把槽大小算成 0 ✗
    local gk ub fr rr fp am
    gk="$(grep -m1 '^gate knobs' "$f" | sed 's/^[^:]*:[ \t]*//')"
    ub="$(grep -m1 '^unsent' "$f" | sed 's/^[^:]*:[ \t]*//' | sed 's/[ \t]*$//')"
    fr="$(grep '^frame ring' "$f" | grep 'snd raw' | tail -1 | sed 's/^[^:]*:[ \t]*//')"
    [ -n "$fr" ] || fr="$(grep -m1 '^frame ring' "$f" | sed 's/^[^:]*:[ \t]*//')"
    rr="$(grep '^recv ring' "$f" | tail -1 | sed 's/^[^:]*:[ \t]*//')"
    fp="$(grep -m1 '^footprint' "$f" | sed 's/^[^:]*:[ \t]*//')"
    am="$(grep -m1 '^ack mode' "$f" | sed 's/^[^:]*:[ \t]*//')"

    # 一行里同时有 snd 和 rcv 两个 "x N B" ⇒ 必须按 snd/rcv 分段取，否则会拿到第二个数
    local slot_snd slot_rcv ring_n conn_sz node_sz ll_sz
    slot_snd="$(printf '%s' "$fr" | tr ',' '\n' | grep 'snd' | sed -n 's/.*x \([0-9]*\) B.*/\1/p' | head -1)"
    ring_n="$(printf '%s' "$fr" | sed -n 's#.*/\([0-9]*\) slots.*#\1#p' | head -1)"
    slot_rcv="$(printf '%s' "$fr" | tr ',' '\n' | grep 'rcv' | sed -n 's/.*x \([0-9]*\) B.*/\1/p' | head -1)"
    [ -n "$slot_rcv" ] || slot_rcv="$(printf '%s' "$rr" | sed -n 's/.*x \([0-9]*\) B per conn.*/\1/p' | head -1)"
    conn_sz="$(printf '%s' "$fp" | sed -n 's/.*sizeof(conn) \([0-9]*\).*/\1/p')"
    node_sz="$(printf '%s' "$fp" | sed -n 's/.*frame node) \([0-9]*\).*/\1/p')"
    ll_sz="$(grep -m1 '^footprint.*UDP_LOWLEVEL' "$f" | sed -n 's/.*UDP_LOWLEVEL) \([0-9]*\).*/\1/p')"

    emit ""
    emit '| 项 | 值 | 说明 |'
    emit '|---|---|---|'
    emit "| \`sizeof(FFRDPCONN)\` | ${conn_sz:-?} B | 每连接协议对象 |"
    emit "| 在飞帧节点 | ${node_sz:-?} B + smss | 只随在飞窗口线性增长 |"
    emit "| 发送环 | ${ring_n:-?} 槽 × ${slot_snd:-?} B = \`$(awk -v a="${ring_n:-0}" -v b="${slot_snd:-0}" 'BEGIN{printf "%d", a*b}')\` B/连接 | 建连即分配 |"
    emit "| 接收环 | ${ring_n:-?} 槽 × ${slot_rcv:-?} B = \`$(awk -v a="${ring_n:-0}" -v b="${slot_rcv:-0}" 'BEGIN{printf "%d", a*b}')\` B/连接 | **首帧到达才分配**（懒分配） |"
    emit "| \`sizeof(UDP_LOWLEVEL)\` | ${ll_sz:-?} B | 每 socket 实例 |"
    emit "| 单连接合计 | \`$(awk -v c="${conn_sz:-0}" -v n="${ring_n:-0}" -v s="${slot_snd:-0}" -v r="${slot_rcv:-0}" 'BEGIN{printf "%.0f", c + n*(s+r)}')\` B | 收发环都分配后 |"
    emit "| 15 连接合计 | \`$(awk -v c="${conn_sz:-0}" -v n="${ring_n:-0}" -v s="${slot_snd:-0}" -v r="${slot_rcv:-0}" 'BEGIN{printf "%.1f", 15*(c + n*(s+r))/1048576}')\` MiB | 不含内核 socket 缓冲 |"
    emit ""
    emit '```'
    emit "gate knobs : $gk"
    emit "unsent     : $ub"
    emit "frame ring : $fr"
    emit "recv ring  : $rr"
    emit "footprint  : $fp"
    emit "ack mode   : $am"
    emit '```'
}

# ── 输出 ─────────────────────────────────────────────────────────────────────
emit "## 表 1 · iperf3 · 单连接（11 档 × 3 环境）"
table_envs tcp single
emit ""
emit "## 表 2 · ffrdp · 单连接（11 档 × 3 环境）"
table_envs ffrdp single
emit ""
emit "## 表 3 · iperf3 · 多连接（15 条合计，11 档 × 3 环境）"
table_envs tcp full
emit ""
emit "## 表 4 · ffrdp · 多连接（15 条合计，11 档 × 3 环境）"
table_envs ffrdp full
emit ""
emit "## 表 5 · ffrdp 专属（iperf3 测不到的指标）"
ffrdp_only single
ffrdp_only full
emit ""
emit "## 表 6 · 本版内存配置"
mem_block
emit ""
emit "> 单元格 = \`avg±std\`；单连接 std 为时间序列标准差，多连接 std 为 15 条（每条先取时间均值）的横截面标准差。"
emit "> \`—\` 表示该方向没跑；\`待采\` 表示该轮没有采 ping（v2.1 前的 ffrdp 档都属此类）。"
