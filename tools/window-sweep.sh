#!/bin/bash
# window-sweep.sh —— 接收窗口扫描（真机）：同一个源码，按 W 重编一档、跑一轮、出一张对照表。
#
# 用法:
#   TARGET=chenk@192.168.0.213 PASS=abc bash tools/window-sweep.sh
#   WINDOWS="128 192 256" DURATION=120 TARGET=chenk@192.168.0.213 PASS=abc bash tools/window-sweep.sh
#   WINDOWS="256" DURATION=60 TARGET=... PASS=... bash tools/window-sweep.sh     # 先试水一档
#   PACE=1 WINDOWS="96 128" TARGET=... PASS=... bash tools/window-sweep.sh       # 顺带打开 ms 令牌桶
#   PACES="0 1" WINDOWS="96 128" TARGET=... PASS=... bash tools/window-sweep.sh   # 每个 W 连跑关/开两档（推荐）
#   SPEED=400 WINDOWS="256" PACES="1" TARGET=... bash tools/window-sweep.sh        # 应用侧限速 400KB/s（辨排队自激）
#   EXTRA='-DFFRDP_CA_RTT_STEP=1 -DFFRDP_SSTHRESH_FLOOR=4' ... bash tools/window-sweep.sh  # 额外编译开关（A/B 用）
#   PARSE_ONLY=1 STAMP=1018-012311 TARGET=x@y bash tools/window-sweep.sh         # 只重解析旧日志，不重跑
#   DRY=1 TARGET=x@y bash tools/window-sweep.sh                                  # 只打印会跑什么
#
# 每一档做的事：
#   1) 用 EXTRA_CFLAGS 把这 8 个常数一起改掉（**不碰源码** ✓，build.sh 支持 EXTRA_CFLAGS）：
#        FFRDP_RX_WINDOW_FRAMES=W      接收缓冲 = 通告窗口（两者本来就是一个旋钮 ✓）
#        FFRDP_MAX_WAITSND=W+128       受理侧帧数闸（必须 >= 窗口，否则它比窗口更紧 ✗）
#        FFRDP_MAX_CWND_SIZE=W         cwnd 上限
#        FFRDP_INFLIGHT_MAX=W          在途闸门
#        FFRDP_INFLIGHT_CEIL=W         闸门"能升"的天花板（= W ⇒ 关掉探测的上升空间 ✓）
#        FFRDP_RECV_QUEUE_MAX=W+128    合法乱序的上限（**必须 >= 窗口**，否则合法乱序会被当谎言丢掉 ✗）
#        FFRDP_SND_FRAME_RING / FFRDP_RCV_FRAME_RING = 2 的幂 >= W+128
#                                      两环的槽数（①-46 起各自定容；改了窗口就必须跟着改，否则编译期 #error ✓）
#   2) 编出来的二进制拷成 dist/test_ffrdp_w<W> ⇒ TAG 里带档位，logs/summary.csv 能按档分组 ✓；
#   3) 交给 test.sh 跑完整真机对测（上传 / 远端服务端 / 本地客户端 / 取回日志 / 写 summary.csv ✓）；
#   4) 从两份日志里抽数：速率、resend、重连次数、rto、swnd ⇒ 一行一档。
# 结束后自动重编译回**默认档**（W=128 + 令牌桶，见 ffrdp.c 默认值）✓。
#
# 读法（判据在表最后那列）：
#   · 重连次数 > 0  ⇒ 这轮作废（连接被切，统计窗口不对 ✓）——先解决断连，别比速率 ✗
#   · rto 明显 > rtts（比如 >600ms 而 rtts≈200）⇒ RTO 退避/丢包恢复没工作，这档不可用 ✗
#   · 以上都干净时，速率随 W 涨到某档不再涨 ⇒ 拐点已到，取**前一档** ✓
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1

WINDOWS="${WINDOWS:-96 128 192 256 640}"
DURATION="${DURATION:-120}"
TOOLCHAIN_NAME="${TOOLCHAIN_NAME:-gcc}"
PACE="${PACE:-0}"          # 1 ⇒ 顺带打开 ms 令牌桶（FFRDP_PACE_ENABLE=1；档位 id 会变成 w96p1 ✓）
PACES="${PACES:-}"         # 给成 "0 1" ⇒ 同一个 W 连跑"关/开"两档 —— 同批次成对比较，链路抖动两者一起承担 ✓
[ -n "$PACES" ] || PACES="$PACE"
DRY="${DRY:-0}"
PINGWATCH="${PINGWATCH:-1}"
TEST_APP_BURST="${TEST_APP_BURST:-2}"
SPEED="${SPEED:-0}"        # 应用侧限速 KB/s，透传给 test.sh（0 = 不限速；用来分辨"应用灌太猛"与"窗口算法"）
EXTRA="${EXTRA:-}"         # 额外编译开关，原样拼进 EXTRA_CFLAGS（例：-DFFRDP_CA_RTT_STEP=1 -DFFRDP_SSTHRESH_FLOOR=4）
TEST_PEER_SILENCE_MS="${TEST_PEER_SILENCE_MS:-10000}"   # 200ms 链路：默认 2000 太紧，会在长静默时重连
BASE="${STAMP:-$(date +%m%d-%H%M%S)}"
PARSE_ONLY="${PARSE_ONLY:-0}"   # =1 ⇒ 不编译不跑，只拿 STAMP 对应的旧日志重出表 ✓

usage() { awk 'NR > 1 { if ($0 !~ /^#/) exit; sub(/^# ?/, ""); print }' "$0"; exit 2; }
[ -n "${TARGET:-}" ] || usage
[ -x ./test.sh ] || { echo "[sweep] ✗ 需要 ./test.sh"; exit 1; }

pow2ge() {      # 最小的 2 的幂且 >= $1（两环的槽数必须是 2 的幂 ✓）
    local p=2
    while [ "$p" -lt "$1" ]; do p=$((p << 1)); done
    echo "$p"
}

flags_for() {   # $1 = W, $2 = pace（0/1）
    local w=$1 pc=${2:-$PACE} r
    r=$(pow2ge $((w + 128)))   # 两环都要装下 RECV_QUEUE_MAX / MAX_WAITSND = W+128
    echo "-DFFRDP_RX_WINDOW_FRAMES=$w -DFFRDP_MAX_WAITSND=$((w + 128)) -DFFRDP_MAX_CWND_SIZE=$w" \
         "-DFFRDP_INFLIGHT_MAX=$w -DFFRDP_INFLIGHT_CEIL=$w -DFFRDP_RECV_QUEUE_MAX=$((w + 128))" \
         "-DFFRDP_SND_FRAME_RING=$r -DFFRDP_RCV_FRAME_RING=$r" \
         "-DFFRDP_PACE_ENABLE=$pc" ${EXTRA:+$EXTRA}
}

build_tier() {  # $1 = W, $2 = 档位 id（w96 / w96p1）, $3 = pace；输出到 dist/test_ffrdp_<id>
    local w=$1 t=$2 pc=${3:-$PACE} bin="dist/test_ffrdp_$t"
    mkdir -p dist
    EXTRA_CFLAGS="$(flags_for "$w" "$pc")" TOOLCHAIN_NAME="$TOOLCHAIN_NAME" bash build.sh >/dev/null 2>&1 || return 1
    cp -f test_ffrdp "$bin" || return 1
    echo "[sweep] 已编 W=$w pace=$pc ⇒ $bin"
}

side() {   # $1=log ⇒ "rate_send/rate_recv|conn_age|rto|swnd"
    awk '
        function num(s) { gsub(/[^0-9.]/,"",s); return s }   # 保留小数点：MB 是 3.63 这种 ✗ 别把点删掉
        /^conn age/ { ca = num($4) }
        /^elapsed/ { el = num($3) }        # 老二进制没有 conn age ⇒ 退回 elapsed(since init)
        /^total_send, total_recv:/ { s = num($3); r = num($4) }
        /^rttm:/ { for (i = 1; i <= NF; i++) if ($i == "rto:") rto = num($(i + 1)) }
        /^swnd, cwnd, ssthresh:/ { sw = num($4) }
        END {
            if (ca == "") ca = el
            if (ca + 0 > 0) printf "%.2f/%.2f|%d|%s|%s", s / ca, r / ca, ca, (rto == "" ? "-" : rto), (sw == "" ? "-" : sw)
            else            printf "-/-|-|-|-"
        }
    ' "$1" 2>/dev/null
}

reconnects() {   # $1=client log
    local n
    n=$(grep -ac 'connect to server ok' "$1" 2>/dev/null || true)
    echo $(( ${n:-0} > 0 ? ${n:-0} - 1 : ${n:-0} ))
}

echo "[sweep] 目标 $TARGET，档位 [$WINDOWS]，pace [$PACES]，限速 ${SPEED}KB/s，每档 ${DURATION}s，批次 $BASE"
echo "[sweep] 每档 6 个常数：RX_WINDOW/WAITSND/CWND/INFLIGHT/CEIL/RECV_QUEUE（见脚本头 ✓）"
[ "$DRY" = 1 ] && echo "[sweep] DRY=1 ⇒ 只打印，不编译不上传"

# 表头写死一行的空白：printf 的 %-Ns 按**字节**补，中文标签按字节补出来的宽度和 4 列数据行对不上 ✗
echo
echo "档位    client发/收  server发/收  resend% 重连  age_c/s   rto_c/s   判定"
LAST_W=$(echo "$WINDOWS" | awk '{print $NF}')
LAST_P=$(echo "$PACES"   | awk '{print $NF}')
for w in $WINDOWS; do
for pc in $PACES; do
    t="w$w"; [ "$pc" = 1 ] && t="w${w}p1"
    if [ "$DRY" = 1 ]; then
        echo "[sweep] 会执行: EXTRA_CFLAGS='$(flags_for "$w" "$pc")' bash build.sh && cp test_ffrdp dist/test_ffrdp_$t"
        echo "[sweep] 会执行: STAMP=$BASE-$t ./test.sh dist/test_ffrdp_$t $TARGET '***' $DURATION"
        continue
    fi

    if [ "$PARSE_ONLY" != 1 ]; then
        build_tier "$w" "$t" "$pc" || { echo "[sweep] ✗ W=$w pace=$pc 编译失败，跳过"; continue; }
        if [ "$w" = "$LAST_W" ] && [ "$pc" = "$LAST_P" ]; then keep=0; else keep=1; fi

        echo; echo "[sweep] === $t（W=$w, pace=$pc）：上传 + 对测 ${DURATION}s ==="
        STAMP="$BASE-$t" KEEP_MUX="$keep" DURATION="$DURATION" PINGWATCH="$PINGWATCH" SPEED="$SPEED" \
            TEST_APP_BURST="$TEST_APP_BURST" TEST_PEER_SILENCE_MS="$TEST_PEER_SILENCE_MS" \
            ./test.sh "dist/test_ffrdp_$t" "$TARGET" "${PASS:-}" "$DURATION" > "logs/sweep-$t-$BASE.out" 2>&1 \
            || echo "[sweep] ⚠ $t 的 test.sh 非零退出（明细 logs/sweep-$t-$BASE.out）"
        grep -aE '客户端|服务端|口径告警|verify' "logs/sweep-$t-$BASE.out" | sed 's/^/    /'
    fi

    # 日志名里 TAG 是 **test.sh 自己算的**（对 test_ffrdp_w96 会缩成 w96 ⇒ 不能按二进制全名去找 ✗），
    # 只有 STAMP 是确定的 ⇒ 一律按 "*_<role>_<STAMP>.log" 匹配 ✓。
    clog=$(ls -1t logs/*_client_${BASE}-${t}.log 2>/dev/null | head -1)
    slog=$(ls -1t logs/*_server_${BASE}-${t}.log 2>/dev/null | head -1)

    # 这一档本轮没有日志（PARSE_ONLY 点到的批次里没跑过它）⇒ 明确写"无日志"，
    # 不能落到"重连=?"再被判成 ✅ —— 那是把"没测"当成"稳定" ✗
    if [ -z "$clog" ] && [ -z "$slog" ]; then
        printf "%-7s %-12s %-12s %-8s %-7s %-9s %-9s %s\n" "$t" "-" "-" "-" "-" "-" "-" "· 无日志"
        continue
    fi

    cm="-/-|0|-|-"; sm="-/-|0|-|-"; rc="?"
    [ -n "$clog" ] && { cm=$(side "$clog"); rc=$(reconnects "$clog"); }
    [ -n "$slog" ] && sm=$(side "$slog")

    cr=$(echo "$cm" | cut -d'|' -f1); cage=$(echo "$cm" | cut -d'|' -f2); crto=$(echo "$cm" | cut -d'|' -f3)
    sr=$(echo "$sm" | cut -d'|' -f1); sage=$(echo "$sm" | cut -d'|' -f2); srto=$(echo "$sm" | cut -d'|' -f3)
    resend=$(grep -a -- "${BASE}-${t}" logs/summary.csv 2>/dev/null | awk -F',' '{gsub(/[^0-9.]/,"",$4); printf "%s ", $4}')
    rmax=$(echo "$resend" | awk '{m = 0; for (i = 1; i <= NF; i++) if ($i + 0 > m) m = $i + 0; print m}')

    # 判定只看两条硬指标：重连（统计窗口被打断）与重传率。
    # swnd 只当列看 —— 它在多连接日志里会取到最后一条 conn 的值，不适合当判据 ✗。
    verdict="✅"
    if [ "${rc:-0}" -gt 0 ] 2>/dev/null; then
        verdict="❌ 重连 $rc 次"
    elif awk -v m="${rmax:-0}" 'BEGIN { exit !(m > 2.0) }'; then
        verdict="⚠ 重传 ${rmax}%"
    fi
    printf "%-7s %-12s %-12s %-8s %-7s %-9s %-9s %s\n" \
           "$t" "$cr" "$sr" "${resend:--}" "$rc" "$cage/$sage" "$crto/$srto" "$verdict"
done
done

if [ "$DRY" = 1 ]; then echo; echo "[sweep] DRY 结束（未编译、未上传、未改任何源码 ✓）"; exit 0; fi
if [ "$PARSE_ONLY" = 1 ]; then echo; echo "[sweep] PARSE_ONLY=1 ⇒ 只重解析旧日志，未编译未上传 ✓"; exit 0; fi

echo; echo "[sweep] 重编译回默认档 ..."
TOOLCHAIN_NAME="$TOOLCHAIN_NAME" bash build.sh >/dev/null 2>&1 && echo "[sweep] 默认档已恢复（W=128 + 令牌桶）✓" \
    || echo "[sweep] ✗ 默认档编译失败，请手动 bash build.sh"

echo
echo "================ 对照表（明细 logs/sweep-w*-$BASE.out、summary.csv 里批次 $BASE）================"
echo "读法：① 先看'重连'——非 0 直接作废；② 再看'RTO 退避'——超 600ms 的档不可用；"
echo "      ③ 最后看速率：随 W 涨到某档不再涨 ⇒ 拐点已到，取**前一档**写回源码 ✓。"
echo "      swnd 列应接近 W（是扩展字的证据：>255 就说明 8 位字段装不下了 ✓）。"
