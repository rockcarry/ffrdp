#!/bin/bash
# tools/cpu-ab.sh —— 「18 点 CPU 差距」的对账工具：交错 A/B + 逐线程拆分 + 按吞吐归一化 + 自动判定归属
#
# 用途（notes §26.85~§26.88 留下的问题）：master 与 v1.1 的 CPU 差要落到**三个桶**里，而不是只报一个总数：
#   A 每圈固定成本  → 工作线程的 select/notify/drain、arm_timer→next_due、tick_conn（发送段+重传扫描）✓
#   B 跨线程交接    → 应用↔工作线程的唤醒/唤醒回程（futex / 条件变量 / 通知字节）✓
#   C 应用侧锁      → ffrdp_send / ffrdp_recv 每次进出同一把锁 ✓
# 判据：**差距落在哪条线程上** —— 工作线程 ⇒ 做 tick 瘦身；应用线程 ⇒ 交接成本，优化空间小 ✓
#
# 用法:
#   bash tools/cpu-ab.sh                        # A=当前构建（./test_ffrdp），B=dist/test_ffrdp_v11_stock_o2
#   bash tools/cpu-ab.sh <A_bin> <B_bin>        # 点名两个二进制（A 是"新的"，B 是"参照"）
#   N=3 D=60 bash tools/cpu-ab.sh               # 3 轮交错（共 6 次运行）、每次 60 s
#   SPEED=0 bash tools/cpu-ab.sh                # 不限速
#
# 设计要点（都是踩过的坑 ✓）:
#   · **交错**：每轮 A→B，下一轮 B→A ⇒ 单机负载漂移（实测可达 ±50 点 ✗）不会系统性偏向某一方 ✓
#   · **逐线程**：/proc/<pid>/task/*/stat 的 utime+stime ⇒ 分 app（comm 不含 ffrdp-udp）与 worker（ffrdp-udp-*）✓
#   · **归一化**：CPU% ÷ 吞吐（MB/s）⇒ 各轮吞吐差异不再变成假的 CPU 差异 ✓（§26.85 的教训）
#   · **上下文切换**：/proc/.../status 的 voluntary+nonvoluntary ⇒ 直接量 B 桶（交接次数），不靠推理 ✓
#   · **防泄漏**：每次运行前后 pkill 同端口进程 ✓（§26.88 那次 11 分钟空转进程污染了整轮结论 ✗）
#
# 输出: logs/cpu-ab-<时间戳>/{run-*.log, run-*.threads, metrics.tsv, summary.txt}
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1

A=${1:-}
B=${2:-}
N=${N:-2}                 # 轮数：每轮 A、B 各跑一次 ⇒ 总运行次数 = 2N
D=${D:-45}                # 每次运行的总时长（秒）
W=${W:-12}                # 预热（秒）：之后才开始采样，避开建连与首轮 dump ✓
SPEED=${SPEED:-60000}     # 每进程发送限速 KB/s（0 = 不限速）
BURST=${BURST:-2}         # TEST_APP_BURST（与历史口径一致 ✓）
PORT0=${PORT0:-8300}
EXTRA_ENV=${EXTRA_ENV:-}   # 额外环境变量（原样传给它，例：EXTRA_ENV="TEST_APP_SENDWAIT_MS=-1"）
HZ=$(getconf CLK_TCK)
STAMP=$(date +%m%d-%H%M%S)
OUT="logs/cpu-ab-$STAMP"
mkdir -p "$OUT"

if [ -z "$A" ]; then
    if [ -x ./test_ffrdp ]; then A=./test_ffrdp
    elif [ -x dist/test_ffrdp_v2_ship ]; then A=dist/test_ffrdp_v2_ship
    else echo "找不到 A 二进制（先 bash build.sh，或显式传入）" >&2; exit 1; fi
fi
if [ -z "$B" ]; then
    B=$(ls -t dist/test_ffrdp_v11_stock_o2 dist/test_ffrdp_v1.1 logs/v1ref-*.bin 2>/dev/null | head -1)
    [ -n "$B" ] || { echo "找不到 B 二进制（v1.1 参照），请显式传入" >&2; exit 1; }
fi
[ -x "$A" ] || { echo "A 不可执行: $A" >&2; exit 1; }
[ -x "$B" ] || { echo "B 不可执行: $B" >&2; exit 1; }

WINDOW=$((D - W - 4))
if [ "$WINDOW" -lt 5 ]; then
    echo "D=$D 太小：要留出预热 ${W}s + 采样至少 5s + 收尾 4s" >&2; exit 1
fi

echo "cpu-ab  $(date '+%F %T')"
echo "  A (新)   : $A   md5 $(md5sum "$A" | awk '{print $1}')"
echo "  B (参照) : $B   md5 $(md5sum "$B" | awk '{print $1}')"
echo "  轮数 $N（交错）· 每次 ${D}s（预热 ${W}s、采样 ${WINDOW}s）· 限速 ${SPEED} KB/s · BURST=$BURST"
echo "  环境     : $(nproc) 核，负载 $(awk '{print $1", "$2", "$3}' /proc/loadavg)"
echo "  明细     : $OUT"
echo

# ── 采样：每个 tid 的 utime/stime/上下文切换/线程名 ───────────────────────────
snap()
{
    local pid=$1 out=$2 d tid ut st vol non name
    : > "$out"
    for d in /proc/$pid/task/*; do
        [ -r "$d/stat" ] || continue
        tid=${d##*/}
        ut=$(awk '{print $14}' "$d/stat" 2>/dev/null) || continue
        st=$(awk '{print $15}' "$d/stat" 2>/dev/null) || continue
        name=$(awk '/^Name:/{print $2}' "$d/status" 2>/dev/null)
        vol=$(awk '/^voluntary_ctxt_switches:/{print $2}' "$d/status" 2>/dev/null)
        non=$(awk '/^nonvoluntary_ctxt_switches:/{print $2}' "$d/status" 2>/dev/null)
        echo "$tid ${name:-?} ${ut:-0} ${st:-0} ${vol:-0} ${non:-0}" >> "$out"
    done
}

# 同一套两份快照 → 汇总量：app% worker% app_sys% worker_sys% app_ctxt worker_ctxt
SUM_AWK='
    NR==FNR { u[$1]=$3; s[$1]=$4; v[$1]=$5; n[$1]=$6; nm[$1]=$2; next }
    {
        tid=$1; du=$3-u[tid]; ds=$4-s[tid]
        pct=(du+ds)/hz/dt*100
        if (nm[tid] ~ /^ffrdp-udp/) { wa+=pct; ws+=ds/hz/dt*100; wc+=($5-v[tid])+($6-n[tid]) }
        else                        { aa+=pct; as_+=ds/hz/dt*100; ac+=($5-v[tid])+($6-n[tid]) }
    }
    END { printf "%.1f %.1f %.1f %.1f %d %d", aa, wa, as_, ws, ac, wc }
'

# ── 一次运行：起进程 → 采样窗口 → 收尾 → 一行 TSV ────────────────────────────
run_one()
{
    local bin=$1 tag=$2 idx=$3
    local port=$((PORT0 + idx))
    local log="$OUT/run-$tag-$idx.log"
    local s1="$OUT/run-$tag-$idx.s1" s2="$OUT/run-$tag-$idx.s2"
    local th="$OUT/run-$tag-$idx.threads"
    local pid t0 t1 dt line kbps mb ts0 ts1 el

    pkill -f "127.0.0.1:$port" 2>/dev/null
    sleep 0.3
    ts0=$(date +%s.%N)
    # EXTRA_ENV：原样塞给被测程序的额外环境（例：TEST_APP_SENDWAIT_MS=-1 = 阻塞等到放得下 ✓，
    # 排除 harness 那条"被拒就等下一轮 10ms recv"的背压节拍 ✓）
    ( sleep "$D"; echo quit ) | env TEST_APP_BURST=$BURST $EXTRA_ENV "$bin" \
        --server=127.0.0.1:$port --client=127.0.0.1:$port --send_speed=$SPEED > "$log" 2>&1 &
    pid=$!
    # 看门狗：进程不听话就按端口收掉，绝不让它泄漏到下一轮（§26.88 ✓）
    ( sleep $((D + 25)); pkill -9 -f "127.0.0.1:$port" ) &
    local dog=$!
    sleep "$W"
    if ! kill -0 "$pid" 2>/dev/null; then
        echo "  $tag#$idx: 进程已退出（见 $log）"
        echo -e "$tag\t$idx\t0\t0\t0\t0\t0\t0\t0\t0" >> "$OUT/metrics.tsv"
        kill "$dog" 2>/dev/null
        return
    fi
    t0=$(date +%s.%N)
    snap "$pid" "$s1"
    sleep "$WINDOW"
    t1=$(date +%s.%N)
    snap "$pid" "$s2"
    wait "$pid" 2>/dev/null
    ts1=$(date +%s.%N)
    kill "$dog" 2>/dev/null

    dt=$(awk -v a="$t0" -v b="$t1" 'BEGIN{print b-a}')
    el=$(awk -v a="$ts0" -v b="$ts1" 'BEGIN{print ((b-a) > 0 ? (b-a) : 1)}')
    line=$(awk -v hz="$HZ" -v dt="$dt" "$SUM_AWK" "$s1" "$s2")
    awk -v hz="$HZ" -v dt="$dt" '
        NR==FNR { u[$1]=$3; s[$1]=$4; v[$1]=$5; n[$1]=$6; nm[$1]=$2; next }
        {
            tid=$1; du=$3-u[tid]; ds=$4-s[tid]
            pct=(du+ds)/hz/dt*100
            if (pct >= 0.5) printf "  %-16s tid %-7s cpu %5.1f%%  sys %5.1f%%  ctxt %d\n",
                                   nm[tid], tid, pct, ds/hz/dt*100, ($5-v[tid])+($6-n[tid])
        }' "$s1" "$s2" > "$th"

    # 吞吐①（**主口径**，所有构建都有）：verify 汇总里的字节数 ÷ 进程实际存活时长。
    #   ship 档 DIAG=0 ⇒ 没有 dump ⇒ 只能走这一条；为了可比，A/B 一律用它 ✓。
    #   单进程两端 ⇒ 每个字节既"发"一次又"收"一次 ⇒ (Σsent + Σrecv)/2 就是一个方向的总量 ✓。
    local bytes mbps
    bytes=$(grep -E '^\[(server|client)\] verify: sent' "$log" 2>/dev/null |
            awk '{ for (i=1;i<=NF;i++) { if ($i=="sent") s+=$(i+4); if ($i=="recv") r+=$(i+4) } }
                 END { printf "%.0f", (s+r)/2 }')
    mbps=$(awk -v b="${bytes:-0}" -v e="$el" 'BEGIN{ printf "%.1f", (e>0) ? b/e/1000000 : 0 }')
    # 吞吐②（交叉验证，只有 DIAG=1 的构建才有）：最后一对 dump 的 averg_send/averg_recv。
    kbps=$(grep -E '^averg_send, averg_recv:' "$log" | tail -n 2 |
           awk -F'[ ,]+' '{s+=($3+0); r+=($4+0); n++} END{printf "%.1f", n ? (s+r)/2 : 0}')
    echo -e "$tag\t$idx\t${line// /$'\t'}\t$mbps\t$kbps" >> "$OUT/metrics.tsv"
    printf '  %s#%d  app %5.1f%%  worker %5.1f%%  MB/s %6s%s\n' "$tag" "$idx" \
        "$(echo "$line" | cut -d' ' -f1)" "$(echo "$line" | cut -d' ' -f2)" "$mbps" \
        "$(awk -v k="$kbps" 'BEGIN{ if (k>0) printf "（dump %.1f）", k/1024; else printf "" }')"
}

# TSV 列: 1 tag 2 idx 3 app% 4 worker% 5 app_sys% 6 worker_sys% 7 app_ctxt 8 worker_ctxt 9 MB/s（主）10 KB/s（dump 交叉验证）
: > "$OUT/metrics.tsv"
for r in $(seq 1 "$N"); do
    i=$((r * 2 - 1))
    if [ $((r % 2)) -eq 1 ]; then
        echo "[轮 $r] A → B"
        run_one "$A" A "$i"
        run_one "$B" B "$i"
    else
        echo "[轮 $r] B → A"
        run_one "$B" B "$i"
        run_one "$A" A "$i"
    fi
done

# ── 汇总：中位数 + 归一化 ────────────────────────────────────────────────────
median()
{
    sort -n | awk '{a[NR]=$1} END{
        if (NR==0) { print 0; exit }
        if (NR%2) printf "%.3f", a[(NR+1)/2];
        else      printf "%.3f", (a[NR/2]+a[NR/2+1])/2 }'
}

# 取某标签某列的中位数（吞吐为 0 的坏轮次直接排除）
col()
{
    awk -F'\t' -v t="$1" -v c="$2" '$1==t && $9>0 {print $c}' "$OUT/metrics.tsv" | median
}

MA_APP=$(col A 3);  MA_WRK=$(col A 4);  MA_SYS=$(col A 5);  MA_SYSW=$(col A 6)
MA_CSWA=$(col A 7); MA_CSWW=$(col A 8); MA_MB=$(col A 9);   MA_KD=$(col A 10)
MB_APP=$(col B 3);  MB_WRK=$(col B 4);  MB_SYS=$(col B 5);  MB_SYSW=$(col B 6)
MB_CSWA=$(col B 7); MB_CSWW=$(col B 8); MB_MB=$(col B 9);   MB_KD=$(col B 10)

{
echo
echo "──── 中位数（交错 $N 轮）──────────────────────────────────────────────"
printf '  %-4s %7s %8s %8s %8s %8s %8s %9s\n' "" "app%" "worker%" "total%" "user%" "sys%" "MB/s" "ctxt/s"
for t in A B; do
    if [ "$t" = A ]; then a=$MA_APP; w=$MA_WRK; s=$MA_SYS; sw=$MA_SYSW; m=$MA_MB; c1=$MA_CSWA; c2=$MA_CSWW
    else                  a=$MB_APP; w=$MB_WRK; s=$MB_SYS; sw=$MB_SYSW; m=$MB_MB; c1=$MB_CSWA; c2=$MB_CSWW; fi
    printf '  %-4s %7.1f %8.1f %8.1f %8.1f %8.1f %8.1f %9.0f\n' "$t" "$a" "$w" \
        "$(awk -v a="$a" -v w="$w" 'BEGIN{print a+w}')" \
        "$(awk -v a="$a" -v w="$w" -v s="$s" -v sw="$sw" 'BEGIN{print (a-s)+(w-sw)}')" \
        "$(awk -v s="$s" -v sw="$sw" 'BEGIN{print s+sw}')" \
        "$m" \
        "$(awk -v c1="$c1" -v c2="$c2" -v win="$WINDOW" 'BEGIN{print (c1+c2)/win}')"
done
printf '  %s\n' "（MB/s = verify 汇总字节 ÷ 进程存活时长（所有构建都有 ⇒ A/B 同口径 ✓）；dump 口径（只有 DIAG=1 才有）A $(awk -v v="${MA_KD:-0}" 'BEGIN{printf "%s", (v>0)?v" KB/s":"n/a"}') / B $(awk -v v="${MB_KD:-0}" 'BEGIN{printf "%s", (v>0)?v" KB/s":"n/a"}')）"

echo
echo "──── 差距分解（A 的 CPU 折算到 B 的吞吐后，逐项相减）──────────────────"
awk -v aa="$MA_APP" -v aw="$MA_WRK" -v sa="$MA_SYS" -v sw="$MA_SYSW" \
    -v ba="$MB_APP" -v bw="$MB_WRK" -v sb="$MB_SYS" -v sbw="$MB_SYSW" \
    -v ma="$MA_MB" -v mb="$MB_MB" \
    -v ca="$MA_CSWA" -v cw="$MA_CSWW" -v cba="$MB_CSWA" -v cbw="$MB_CSWW" \
    -v win="$WINDOW" '
    BEGIN {
        if (ma <= 0 || mb <= 0) { print "  ⚠ 吞吐读数为 0 ⇒ 这一轮不可用（没跑满 / 进程提前退出 / 没有 verify 行）"; exit }
        k = mb/ma                       # 把 A 的 CPU 折算到 B 的吞吐（同吞吐才可比 ✓）
        A_au = (aa-sa)*k; A_as = sa*k; A_wu = (aw-sw)*k; A_ws = sw*k
        B_au = ba-sb;     B_as = sb;   B_wu = bw-sbw;    B_ws = sbw
        A_t = A_au+A_as+A_wu+A_ws; B_t = B_au+B_as+B_wu+B_ws
        printf "    原始读数      A %.1f%% @ %.1f MB/s   vs   B %.1f%% @ %.1f MB/s\n", A_t/k, ma, B_t, mb
        printf "    折算到同吞吐  A %.1f%%（×%.3f）      vs   B %.1f%%      ⇒ 差 %+.1f 点\n\n", A_t, k, B_t, A_t-B_t
        printf "    %-22s %9s %9s %9s\n", "项", "A", "B", "差(A−B)"
        printf "    %-22s %9.1f %9.1f %+9.1f\n", "应用线程 user",  A_au, B_au, A_au-B_au
        printf "    %-22s %9.1f %9.1f %+9.1f\n", "应用线程 sys",   A_as, B_as, A_as-B_as
        printf "    %-22s %9.1f %9.1f %+9.1f\n", "工作线程 user",  A_wu, B_wu, A_wu-B_wu
        printf "    %-22s %9.1f %9.1f %+9.1f\n", "工作线程 sys",   A_ws, B_ws, A_ws-B_ws
        printf "    %-22s %9.1f %9.1f %+9.1f\n", "合计",           A_t,  B_t,  A_t-B_t
        gap = A_t - B_t
        dsys = (A_as+A_ws) - (B_as+B_ws)
        dusr = gap - dsys
        printf "\n    ⇒ 这 %+.1f 点里：user（每圈用户态计算）%+.1f 点、sys（syscall+futex）%+.1f 点", gap, dusr, dsys
        if (gap > 0.1) printf "  ⇒ **sys 占 %.0f%%**\n", 100*dsys/gap; else printf "\n"
        printf "    ctxt/s（B 桶直接读数）：app %+.0f、worker %+.0f 次/s\n",
               (ca-cba)/win, (cw-cbw)/win
        if (gap <= 1.0)
            printf "\n  ⇒ 判定：折算后几乎没有差距（%+.1f 点）⇒ 先查口径（限速/贴不满/丢包/二进制选错）\n", gap
        else if (dsys/gap >= 0.6) {
            printf "\n  ⇒ 判定：差距以 **sys 为主（%.0f%%）** ⇒ 是系统调用次数与跨线程交接，不是用户态算法\n", 100*dsys/gap
            printf "     · tick 瘦身只动 user ⇒ 预期收益 ≤ 2~3 点；要真拿回差距得减 syscall 次数/减少交接\n"
            printf "     · 先看下面判据行的 laps / arm-now / rx cost 三行确认圈率与空转，再决定值不值\n"
        }
        else if (dusr/gap >= 0.6) {
            printf "\n  ⇒ 判定：差距以 **user 为主（%.0f%%）** ⇒ 每圈用户态计算（tick_conn / next_due / 发送段）\n", 100*dusr/gap
            printf "     ⇒ tick 瘦身 + 批上限重扫是对症的，预期 2~4 点 ✓\n"
        }
        else
            printf "\n  ⇒ 判定：user %+.1f / sys %+.1f 接近对半 ⇒ 两件都做才有意义，单做一件最多拿一半\n", dusr, dsys
    }'
} | tee "$OUT/summary.txt"

# ── 判据行（各标签最后一次运行）：回答"每圈 / 每包 / 空转"三问 ────────────────
{
echo
echo "──── 判据行（各标签最后一次运行）────────────────────────────────────"
for t in A B; do
    f=$(ls -t "$OUT"/run-$t-*.log 2>/dev/null | head -1)
    [ -n "$f" ] || continue
    echo "  [$t] $(basename "$f")"
    # v1.1 的 dump 里没有 lowlevel 段（那是 v2 才有的）⇒ 判据行按"有没有任何 dump"来分岔 ✓
    if ! grep -qE '^(confirmed rate|averg_send)' "$f" 2>/dev/null; then
        echo "    （没有 dump 行 —— ship 档 FFRDP_DIAG=0 的构建本来就不打，属正常 ✓）"
    else
        grep -E '^(udp lowlevel laps|udp lowlevel lap cost|udp lowlevel rx cost|udp lowlevel arm-now|confirmed rate|timer due given|send wake|cond broadcast)' "$f" 2>/dev/null |
            tail -n 8 | sed 's/^/    /'
    fi
    echo "    逐线程："
    sed 's/^/    /' "${f%.log}.threads" 2>/dev/null
done
} | tee -a "$OUT/summary.txt"

echo
echo "明细已存：$OUT"
