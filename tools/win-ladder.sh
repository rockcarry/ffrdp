#!/bin/bash
# win-ladder.sh —— "小窗口 / 小在途"档位对测：多连接 · 应用限速 · 真机。
#
# 要回答的问题：15 条连接各 100KB/s（总供给 ≈ 2× 链路容量）时，把每条连接的窗口/在途缩小，
# 能不能把"重传 88% / 白费 78% / RTT 涨到 1.2s"的拥塞崩溃压回去？还是说必须靠应用侧限速？
#
# 用法:
#   TARGET=chenk@192.168.0.213 PASS=abc bash tools/win-ladder.sh                       # 默认四档、15×100KB/s、120s
#   TIERS="w32 w16" CLIENTS=15 SPEED=100 DURATION=120 TARGET=... PASS=... bash tools/win-ladder.sh
#   DRY=1 TARGET=x@y bash tools/win-ladder.sh                                          # 只打印会跑什么
#   PARSE_ONLY=1 STAMP=1019-113651 TARGET=x@y bash tools/win-ladder.sh                 # 只重解析旧日志
#
# 与另外两个 sweep 的分工（**tier 映射不同，别把同一档号当同义** ✗）:
#   tools/window-sweep.sh     W 档、单连接；MAX_WAITSND = W+128、两环 = 2^n >= W+128（§9.7 的约束式）
#   tools/inflight-sweep.sh   cap 档、单连接；直接钉住 FFRDP_INFLIGHT_MAX（并自动还原源码）
#   本脚本                    多连接（默认 15 条）**聚合**：最差 resend / waste / rtts + 重连计数 + 判定列；
#                             走"整组 = W"约定（MAX_WAITSND = W、两环 = pow2ge(W)），与默认 64 档同构 ✓
#   ⇒ 换档号前先确认读的是哪一套映射；要横向比三个脚本的同一档，先把映射统一（见 todo.md 维护项 ✓）。
#
# 档位（全部走 EXTRA_CFLAGS，**不改源码** ✓）:
#   base  = 不改（当前默认：窗口/乱序窗/受理闸/cwnd/在途/天花板 + 两环都是 64）
#   w32   = 整组 32（窗口 / 乱序窗 / 受理闸 / cwnd / 在途 / 天花板 + 两环 32）
#   w16   = 整组 16（同上，UNSENT 跟着降到 8）
#   cap16 = 窗口仍 64，只把**在途闸门与天花板**砍到 16
#           ⇒ 这是"按 waste 砍 cap"那个候选动作的等价实验：先看只砍在途够不够 ✓
#
# 每一档：build.sh 编出来 ⇒ 拷成 dist/test_ffrdp_<档> ⇒ test.sh 上传对测 ⇒ 抽数
# 结束后自动编回**默认档**（build.sh 会原地覆盖 test_ffrdp ✓）。
#
# 读法:
#   verify  必须全 OK；重连 > 0 ⇒ 这一档作废（统计窗口被打断）
#   resend% 重传率；waste% 白费占比（对端说"这帧我早就有"）—— 白费高 = 伪超时 / 超发
#   rtts    典型 RTT（排队深度）：回到 10~30ms 级才算"没堵"
#   maxwait `timeout diag` 里最长的一次等待 —— 秒级说明队头被卡住
#   txfull  发送环满的圈数（应用被窗口顶住的次数）
#   MB/s    client 15 条的**合计**发/收
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1

TARGET="${TARGET:-}"
PASS="${PASS:-}"
CLIENTS="${CLIENTS:-15}"
SPEED="${SPEED:-100}"          # 每连接 KB/s（0 = 不限速）
DURATION="${DURATION:-120}"
TIERS="${TIERS:-base w32 w16 cap16}"
TOOLCHAIN_NAME="${TOOLCHAIN_NAME:-gcc}"
KEEP_MUX="${KEEP_MUX:-1}"
DRY="${DRY:-0}"
PARSE_ONLY="${PARSE_ONLY:-0}"
BASE="${STAMP:-$(date +%m%d-%H%M%S)}"

usage() { awk 'NR > 1 { if ($0 !~ /^#/) exit; sub(/^# ?/, ""); print }' "$0"; exit 2; }
[ -n "$TARGET" ] || usage
[ -x ./test.sh ] || { echo "[ladder] ✗ 需要 ./test.sh"; exit 1; }
mkdir -p dist logs

pow2ge() {      # 最小的 2 的幂且 >= $1（环槽数必须是 2 的幂 ✓）
    local p=2
    while [ "$p" -lt "$1" ]; do p=$((p << 1)); done
    echo "$p"
}

tier_flags() {  # $1 = 档位名 ⇒ EXTRA_CFLAGS
    case "$1" in
    base) echo "" ;;
    cap16) echo "-DFFRDP_INFLIGHT_MAX=16 -DFFRDP_INFLIGHT_CEIL=16" ;;
    w*)
        local w="${1#w}" r
        r=$(pow2ge "$w")
        echo "-DFFRDP_RX_WINDOW_FRAMES=$w -DFFRDP_RECV_QUEUE_MAX=$w" \
             "-DFFRDP_MAX_WAITSND=$w -DFFRDP_MAX_CWND_SIZE=$w" \
             "-DFFRDP_INFLIGHT_MAX=$w -DFFRDP_INFLIGHT_CEIL=$w" \
             "-DFFRDP_UNSENT_MAX=$((w / 2))" \
             "-DFFRDP_SND_FRAME_RING=$r -DFFRDP_RCV_FRAME_RING=$r"
        ;;
    *) echo "" ;;
    esac
}

tier_desc() {   # 表里那列用的短说明
    case "$1" in
    base) echo "默认 64" ;;
    cap16) echo "窗口 64/在途 16" ;;
    w*)   echo "全 ${1#w}" ;;
    *)    echo "$1" ;;
    esac
}

# 从一堆日志里汇总: "client发/收(MB/s)|resend%(最大)|waste%(最大)|rtts(最大)|maxwait(最大)|txfull(合计)|verifyOK/总"
agg() {
    # 每份日志里 dump 会出现多次（每次 quit 前都打）⇒ 一律取**最后一次**（=最终状态），
    # 再跨 15 条连接取"最差"（resend/waste/rtts/maxwait）或"合计"（速率 / txfull）。
    awk '
        function num(s) { gsub(/[^0-9.]/, "", s); return s + 0 }
        /^conn age/                 { ca[FILENAME] = num($4) }
        /^total_send, total_recv:/  { ts[FILENAME] = num($3); tr[FILENAME] = num($4); seen[FILENAME] = 1 }
        /^rtt_min, rtts, rttd/      { rt[FILENAME] = num($6) }
        /^counter_resend_ratio/     { re[FILENAME] = num($2) }
        /^counter_resend_waste/     { wa[FILENAME] = num($2) }
        /^counter_txfull/           { tf[FILENAME] = num($3) }
        /^timeout diag/             { for (i = 1; i <= NF; i++) if ($i == "wait") mw[FILENAME] = num($(i + 1)) }
        /verify: OK/                { vok[FILENAME] = 1 }
        END {
            ns = nr = sre = swa = srtt = smw = stf = nf = 0
            for (f in seen) {
                nf++
                d = ca[f] ? ca[f] : 0
                if (d > 0) { ns += ts[f] / d; nr += tr[f] / d }
                if (re[f] > sre) sre = re[f]
                if (wa[f] > swa) swa = wa[f]
                if (rt[f] > srtt) srtt = rt[f]
                if (mw[f] > smw) smw = mw[f]
                stf += tf[f]
                if (f in vok) nv++
            }
            printf "%.2f/%.2f|%.2f|%.2f|%d|%d|%d|%d/%d\n", ns, nr, sre, swa, srtt, smw, stf, nv + 0, nf
        }
    ' "$@" 2>/dev/null
}

reconnects() {  # $1..=client logs ⇒ 重连次数（"connect to server ok" 条数 − 日志份数）
    local n=0 f
    for f in "$@"; do n=$(( n + $(grep -ac 'connect to server ok' "$f" 2>/dev/null || echo 0) )); done
    echo $(( n > $# ? n - $# : 0 ))
}

echo "[ladder] 目标 $TARGET | ${CLIENTS} 连接 × ${SPEED}KB/s | 每档 ${DURATION}s | 批次 $BASE"
# SPEED 是**每进程**的 ⇒ 乘上连接数才是实际供给；慢 AP 的上行容量只有约 290~340KB/s，
# 15 × 100 就是 4.4× 超卖（默认值天生落在崩溃区 ⇒ 只能用来跑"塌不塌"的对照 ✗）。
if [ "$SPEED" != 0 ]; then
    echo "[ladder] 应用侧供给 ${CLIENTS} × ${SPEED}KB/s = $((CLIENTS * SPEED))KB/s"\
         "（慢 AP 上行 ~290~340KB/s ⇒ 超过就是超卖；要按容量跑就改用 UP_TOTAL/DOWN_TOTAL）"
fi
# UP_TOTAL/DOWN_TOTAL 走环境变量透传给 test.sh（它按 CLIENTS 均分）；这时 SPEED 不起作用 ⇒ 明说一句，
# 否则上面那行"× ${SPEED}KB/s"会被读成实际供给（= 又把超卖算错 ✓）。
if [ "${UP_TOTAL:-0}" != 0 ] || [ "${DOWN_TOTAL:-0}" != 0 ]; then
    echo "[ladder] 总量限速: 上行 ${UP_TOTAL:-0}KB/s | 下行 ${DOWN_TOTAL:-0}KB/s（每连接由 test.sh 均分；SPEED 被忽略）"
fi
echo "[ladder] 档位 [$TIERS]（base=不改；wN=整组 N；cap16=只砍在途）"
[ "$DRY" = 1 ] && echo "[ladder] DRY=1 ⇒ 只打印，不编译不上传"

if [ "$DRY" != 1 ] && [ "$PARSE_ONLY" != 1 ]; then
    total=$(echo "$TIERS" | wc -w); i=0
    for t in $TIERS; do
        i=$((i + 1))
        echo; echo "[ladder] === $t（$(tier_desc "$t")）：编译 + 对测 ${DURATION}s ==="
        if ! EXTRA_CFLAGS="$(tier_flags "$t")" TOOLCHAIN_NAME="$TOOLCHAIN_NAME" bash build.sh >"logs/ladder-build-$t-$BASE.log" 2>&1; then
            echo "[ladder] ✗ $t 编译失败（明细 logs/ladder-build-$t-$BASE.log），跳过"
            continue
        fi
        cp -f test_ffrdp "dist/test_ffrdp_$t" || { echo "[ladder] ✗ 拷贝失败"; continue; }
        keep=1; [ "$i" -eq "$total" ] && keep=0
        # 扫参时（一条命令连着跑很多轮）要的是"只输一次密码"⇒ 显式钉住复用连接，
        # 否则最后一档 keep=0 会把 mux 关掉，下一轮又得重输（KEEP_MUX 在 test.sh 里默认 0 ✓）。
        [ "${LADDER_KEEP_MUX:-0}" = 1 ] && keep=1
        STAMP="$BASE-$t" KEEP_MUX="$keep" CLIENTS="$CLIENTS" SPEED="$SPEED" \
            ./test.sh "dist/test_ffrdp_$t" "$TARGET" "$PASS" "$DURATION" \
            >"logs/ladder-$t-$BASE.out" 2>&1 \
            || echo "[ladder] ⚠ $t 的 test.sh 非零退出（明细 logs/ladder-$t-$BASE.out）"
        grep -aE 'verify|重传率' "logs/ladder-$t-$BASE.out" | head -4 | sed 's/^/    /'
    done
    echo; echo "[ladder] 重编译回默认档 ..."
    if TOOLCHAIN_NAME="$TOOLCHAIN_NAME" bash build.sh >/dev/null 2>&1; then
        echo "[ladder] 默认档已恢复 ✓（test_ffrdp 回到窗口/闸门 64）"
    else
        echo "[ladder] ✗ 默认档编译失败，请手动 bash build.sh"
    fi
fi

if [ "$DRY" = 1 ]; then
    for t in $TIERS; do
        echo "[ladder] 会执行: EXTRA_CFLAGS='$(tier_flags "$t")' bash build.sh && cp test_ffrdp dist/test_ffrdp_$t"
        echo "[ladder] 会执行: STAMP=$BASE-$t CLIENTS=$CLIENTS SPEED=$SPEED ./test.sh dist/test_ffrdp_$t $TARGET *** $DURATION"
    done
    exit 0
fi

echo
echo "---- 对照表（明细 logs/ladder-<档>-$BASE.out；汇总批次 $BASE）----"
printf "%-7s %-14s %-8s %-8s %-6s %-8s %-7s %-10s %s\n" \
       "档位" "说明" "verify" "resend%" "waste%" "rtts" "maxwait" "client发/收" "判定"
for t in $TIERS; do
    pat="logs/*_client_${BASE}-${t}*.log"
    # shellcheck disable=SC2086
    clog=$(ls -1t $pat 2>/dev/null)
    if [ -z "$clog" ]; then
        printf "%-7s %-14s %s\n" "$t" "$(tier_desc "$t")" "· 无日志（没跑或 test.sh 失败）"
        continue
    fi
    # shellcheck disable=SC2086
    m=$(agg $clog)
    rc=$(reconnects $clog)
    rate=$(echo "$m" | cut -d'|' -f1)
    re=$(echo "$m"   | cut -d'|' -f2)
    wa=$(echo "$m"   | cut -d'|' -f3)
    rt=$(echo "$m"   | cut -d'|' -f4)
    mw=$(echo "$m"   | cut -d'|' -f5)
    tf=$(echo "$m"   | cut -d'|' -f6)
    vk=$(echo "$m"   | cut -d'|' -f7)

    spat="logs/*_server_${BASE}-${t}*.log"
    # shellcheck disable=SC2086
    slog=$(ls -1t $spat 2>/dev/null | head -1)

    verdict="✅"
    if [ "${vk%%/*}" != "${vk##*/}" ]; then verdict="❌ verify $vk"
    elif [ -n "$slog" ] && ! grep -qa 'verify: OK' "$slog"; then verdict="❌ server verify"
    elif [ "$rc" -gt 0 ]; then      verdict="❌ 重连 $rc 次"
    elif awk -v w="$wa" 'BEGIN { exit !(w > 50) }'; then verdict="⚠ 白费 ${wa}%"
    elif awk -v w="$rt" 'BEGIN { exit !(w > 120) }'; then verdict="⚠ RTT ${rt}ms"
    fi
    printf "%-7s %-14s %-8s %-8s %-6s %-8s %-7s %-10s %s\n" \
           "$t" "$(tier_desc "$t")" "$vk" "$re" "$wa" "${rt}ms" "${mw}ms" "$rate" "$verdict"
done

echo
echo "判据：① verify 全 OK 且无重连；② waste% 明显下降（伪超时被治住）；"
echo "      ③ rtts 回到 30ms 以下；④ client 发/收 合计上升。"
echo "      cap16 与 w16 都干净 ⇒ 只砍在途就够（候选 a 可做，值域 16）；"
echo "      只有 w16 干净 ⇒ 窗口/通告窗也得一起缩，候选 a 要连 swnd 一起收。"
echo "      三档都不干净 ⇒ 根因不在这，别改协议，先按链路容量给应用侧总限速。"
