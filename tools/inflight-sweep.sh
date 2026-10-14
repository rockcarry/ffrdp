#!/bin/bash
# inflight-sweep.sh —— 在途上限扫描：量出"在途 →（延迟, 速率）"，用来决定闸门该取多少。
#
# 两种模式（同一个脚本 ✓）：
#   A) 本机模式（默认）：只在这一端改常数、重编译、跑，另一端保持不动。
#      DURATION=300 bash tools/inflight-sweep.sh --server=0.0.0.0:8111
#      CAPS="probe 2 3 4 6 8" DURATION=300 bash tools/inflight-sweep.sh --server=0.0.0.0:8111
#   B) 真机模式（TARGET=user@host ✓）：每档编一个 dist/test_ffrdp_cap<N>，交给 test.sh 自动
#      "上传 → 远端服务端 → 本地客户端 → 取回日志 → 写 logs/summary.csv"，一轮跑完出对照表。
#      TARGET=chenk@192.168.0.213 PASS=abc DURATION=180 bash tools/inflight-sweep.sh
#      CAPS="5 64" DURATION=60 TARGET=... bash tools/inflight-sweep.sh      # 只跑两档（先试水 ✓）
#      DRY=1 TARGET=x@y bash tools/inflight-sweep.sh                        # 只看它会跑什么（不碰网络 ✓）
#      默认档位 "5 16 32 64 128"：5 = 旧基线（对照 ✓）、128 = 2026-10-12 那次吞吐 ×3 的上限 ✓。
#
# 为什么不能直接测：上限平时由探测动态改写（inflight_max = probe_state ? probe_cap : cap_kept ?: 默认），
# 所以必须"关掉探测 + 把默认上限钉住"，才能看到固定在途下的延迟与速率。
#
# 每一档：1) 临时改 ffrdp.c 的 FFRDP_INFLIGHT_MAX + FFRDP_PROBE_ENABLE 置 0；2) 编译；
#         3) 跑 DURATION 秒后自动 quit（这样才能打印 verify 汇总）；4) 收关键行、抽一行数据。
# 结束后：**自动还原 ffrdp.c 并校验字节一致**，再重新编译回交付版本 ✓。
#
# 读法：排队延迟 = rtts - rtt_min（越小越好），同时确认速率不能掉。
#   真机模式下要紧的是反过来看：**速率随闸门涨** ⇒ 闸门就是刹车，取到"不再涨"的那一档 ✓；
#   速率不再涨而 rtts 继续涨 ⇒ 拐点到了，取前一档 ✓（这个值就是要写回源码的那个 ✓）。
set -u

DURATION="${DURATION:-300}"
CAPS="${CAPS:-probe 2 3 4 6 8}"
SRC=ffrdp.c
SUM="sweep-summary.txt"

usage() {
    awk 'NR > 1 { if ($0 !~ /^#/) exit; sub(/^# ?/, ""); print }' "$0"
    exit 2
}
[ -n "${TARGET:-}" ] || [ $# -ge 1 ] || usage

# ---- 源码备份 + 退出时一定还原（含 Ctrl+C / 出错）----
BAK=$(mktemp /tmp/ffrdp.c.sweep.XXXXXX)
cp -f "$SRC" "$BAK"
SUM_BAK=$(md5sum "$SRC" | cut -d' ' -f1)
restore() {
    cp -f "$BAK" "$SRC"
    have=$(md5sum "$SRC" | cut -d' ' -f1)
    if [ "$have" = "$SUM_BAK" ]; then echo "[sweep] 源码已还原（md5 $have）✓"
    else echo "[sweep] ✗ 源码还原后 md5 不一致（$have != $SUM_BAK）—— 请手动检查 $SRC（备份在 $BAK）"; fi
}
trap 'echo; echo "[sweep] 中断，正在还原源码..."; restore; exit 130' INT TERM

patch_src() {   # $1 = 目标上限；同时关掉探测
    sed -i -E "s|^#define FFRDP_INFLIGHT_MAX +[0-9]+|#define FFRDP_INFLIGHT_MAX   $1|" "$SRC"
    sed -i -E "s|^#define FFRDP_PROBE_ENABLE +[0-9]+|#define FFRDP_PROBE_ENABLE        0|" "$SRC"
}
restore_src() { cp -f "$BAK" "$SRC"; }

build() {
    TOOLCHAIN_NAME=native ./build.sh >/dev/null 2>&1 || { echo "[sweep] ✗ 编译失败"; restore; exit 1; }
}

local_mode() {
: > "$SUM"
printf "%-7s %-7s %-6s %-5s %-12s %-10s %-11s %-13s %-8s\n" \
       "cap" "rtt_min" "rtts" "rttd" "确认速率" "每MB重传" "swnd/cwnd" "inflight/cap" "wait_snd" | tee -a "$SUM"

for cap in $CAPS; do
    if [ "$cap" = "probe" ]; then
        restore_src; label="probe(现状)"
    else
        patch_src "$cap"; label="$cap"
    fi
    echo; echo "[sweep] === 档位 $label：编译 + 跑 ${DURATION}s（另一端保持不动）==="
    build

    log="sweep-$cap.log"
    ( sleep "$DURATION"; echo quit ) | ./test_ffrdp "$@" > "$log" 2>&1
    grep -aE '^(version & build|build|elapsed|total_send|rtt_min|swnd|wait_snd|inflight|confirmed rate|queue test|probe trigger|probe rtt ref/best|fec_txredundancy|counter_(resend_|recv_ooo|ooo_d|mack_bits|send_err|send_failed|send_1sttime))' "$log" > "$log.key"

    # 取最后一块的关键值
    row=$(awk '
        function val(  s){ s=$0; sub(/^[^:]*:[ ]*/,"",s); gsub(/\r/,"",s); return s }
        /^rtt_min, rtts, rttd :/ { v=val(); split(v,a,/ *, */)
                                   # 这一行后面还跟着 "(min / typical / jitter, ms)"，要切掉
                                   for (k=1;k<=3;k++) sub(/[^0-9].*$/,"",a[k])
                                   rmin=a[1]; rtts=a[2]; rttd=a[3] }
        /^confirmed rate /       { v=val(); split(v,a," "); crate=a[1] }
        /^counter_resend_perMB/  { pmb=val() }
        /^swnd, cwnd, ssthresh:/ { v=val(); split(v,a,/ *, */); sw=a[1]; cw=a[2] }
        /^inflight, cap /        { v=val(); split(v,a," "); gsub(/,/,"",a[1]); gsub(/,/,"",a[2]); infl=a[1]; cp=a[2] }
        /^wait_snd /             { w=val() }
        END {
            # 跑得太短时可能一块 dump 都没有 ⇒ 用 "-" 占位，别打印空字段
            if (rmin=="" ) rmin="-";  if (rtts=="") rtts="-";  if (rttd=="") rttd="-"
            if (crate=="") crate="-"; if (pmb =="") pmb="-"
            if (sw==""   ) sw="-";    if (cw==""  ) cw="-"
            if (infl=="" ) infl="-";  if (cp==""  ) cp="-";    if (w==""   ) w="-"
            printf "%s %s %s %s %s %s/%s %s/%s %s", rmin, rtts, rttd, crate, pmb, sw, cw, infl, cp, w
        }
    ' "$log.key")
    printf "%-7s %s\n" "$label" "$row" | tee -a "$SUM"

    # cap 的分布：探测档的验收点就在这里 —— 应当几乎全落在 4 上；若还散在 5/6，说明
    # giveback 又把已证明的上限抬回去了。看 rtts - rtt_min 时也要对着它看：只看最后一次 dump
    # 会被"探测刚赢的那一瞬间"骗到（那一刻 cap 才是 4）。
    printf "         cap 分布: "
    grep -a 'inflight, cap' "$log" | sed -E 's/.*: [0-9]+, ([0-9]+) .*/\1/' | sort -n | uniq -c | tr '\n' ' '
    echo

    # 顺手把这次的 verify 汇总也报一下（判断这轮跑得干不干净）
    grep -a 'verify:' "$log" | tail -2 | sed 's/^/         /'
done

restore_src
echo; echo "[sweep] 还原源码并重编译回交付版本 ..."
build
restore

echo
echo "================ 对照表（存在 $SUM）================"
cat "$SUM"
echo
echo "读法：排队延迟 = rtts - rtt_min；同时看'确认速率'是否保持。"
echo "      · 某档 rtts 明显更低、速率保持 ⇒ 把探测的保留判据改成'绝对下降'就能拿到收益；"
echo "      · 速率跟着掉 ⇒ 窗口这条路到头，只能做'削峰型 pacing'（速率不变、只压排队峰值）。"
}

# ══ 真机模式 ══════════════════════════════════════════════════════════════════
# 每档编一个二进制，然后交给 test.sh 跑完整的一轮真机对测（上传/远端服务端/本地客户端/取日志/
# 写 summary.csv ✓）。档位标识同时进二进制名与 STAMP ⇒ 表里能按档分组 ✓。
# 定义放在文件末尾：它用到 patch_src / build / restore，而 bash 是按顺序解释的 ✓。
real_mode() {
    [ -x ./test.sh ] || { echo "[sweep] ✗ 真机模式需要 ./test.sh（上传与采集都靠它 ✓）"; exit 1; }
    command -v ssh >/dev/null 2>&1 || { echo "[sweep] ✗ 真机模式需要 ssh/scp"; exit 1; }
    # 只有"用户没显式给"时才换成真机默认档（本机默认串是 "probe 2 3 4 6 8" ✓）
    [ "$CAPS" = "probe 2 3 4 6 8" ] && CAPS="5 16 32 64 128"
    [ "$DURATION" = 300 ] && DURATION=180
    BASE="$(date +%m%d-%H%M%S)"
    mkdir -p logs dist
    echo "[sweep] 真机模式：目标 $TARGET，档位 [$CAPS]，每档 ${DURATION}s（两端对测 ✓）"
    echo "[sweep] 本批标识 = $BASE（logs/summary.csv 里按它取这一批 ✓）"
    [ -n "${PASS:-}" ] || echo "[sweep] 没给 PASS ⇒ 走 SSH 复用连接，整批只让你输一次密码 ✓"
    left=$(echo $CAPS | wc -w)
    for cap in $CAPS; do
        patch_src "$cap"
        echo; echo "[sweep] === 档位 cap$cap：编译 + 上传 + 对测 ${DURATION}s ==="
        build || { echo "[sweep] ✗ 编译失败"; restore; exit 1; }
        cp -f test_ffrdp "dist/test_ffrdp_cap$cap"     # 名字会进 TAG ⇒ 表里一眼看出是哪一档 ✓
        if [ "$left" -gt 1 ]; then KEEP_MUX=1; else KEEP_MUX=0; fi    # 最后一档收掉复用连接 ✓
        STAMP="$BASE-cap$cap" KEEP_MUX="$KEEP_MUX" \
            ./test.sh "dist/test_ffrdp_cap$cap" "$TARGET" "${PASS:-}" "$DURATION" 2>&1 \
            | grep -aE '客户端|服务端|重传率|in-flight|verify' | sed 's/^/    /'
        left=$((left - 1))
    done
    restore_src
    echo; echo "[sweep] 还原源码并重编译回交付版本 ..."
    build
    restore
    echo
    echo "================ 真机对照表（logs/summary.csv 里 $BASE 这批 ✓）================"
    echo "  stamp              tag        role    resend%  waste%  rtts_ms  send/recv_MBps"
    grep -a -- "$BASE" logs/summary.csv 2>/dev/null | sed 's/^/  /' || echo "  （没有本批记录 ✗ 看上面的输出）"
    echo
    echo "读法：横着比同一档（tag=capN）的 client / server 两行："
    echo "      · 速率随 cap 涨、rtts 也涨 ⇒ 闸门还在买吞吐，值得继续抬 ✓；"
    echo "      · 速率不再涨、rtts 继续涨 ⇒ 拐点已到，取前一档写回源码 ✓。"
}

if [ -n "${TARGET:-}" ]; then
    real_mode
else
    local_mode "$@"
fi
