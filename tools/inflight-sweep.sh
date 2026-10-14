#!/bin/bash
# inflight-sweep.sh —— 在途上限扫描：量出"在途 →（延迟, 速率）"，用来决定延迟那 10~14ms 排队还能不能降。
#
# 为什么不能直接测：上限平时由探测动态改写（inflight_max = probe_state ? probe_cap : cap_kept ?: 默认），
# 所以必须"关掉探测 + 把默认上限钉住"，才能看到固定在途下的延迟与速率。
#
# 在哪跑：**只在这一端**跑（选"被限速、重传多的那一侧"，即 perMB 偏高的那端）；
#         另一端**保持正常在跑**即可，不用改、不用重编译。
# 怎么跑：
#   DURATION=300 bash tools/inflight-sweep.sh --server=0.0.0.0:8111
#   CAPS="probe 2 3 4 6 8" DURATION=300 bash tools/inflight-sweep.sh --server=0.0.0.0:8111
#   （CAPS 里 probe = 保持现状，即"探测开、默认 64"，作为基准档）
#
# 每一档：
#   1) 临时把 ffrdp.c 的 FFRDP_INFLIGHT_MAX 改成该值、FFRDP_PROBE_ENABLE 置 0；
#   2) 编译；3) 跑 DURATION 秒后自动输入 quit 退出（这样才能打印 verify 汇总）；
#   4) 收关键行、抽出一行数据；5) 下一档。
# 结束后：**自动还原 ffrdp.c 并校验字节一致**，再重新编译回交付版本。
#
# 读法：排队延迟 = rtts - rtt_min（越小越好），同时确认速率（confirmed rate）不能掉。
#   若某档 rtts 明显更低、速率保持 ⇒ 值得把探测的保留判据改成"绝对下降"（几行的改动）；
#   若速率跟着掉 ⇒ 窗口这条路到头了，只剩"削峰型 pacing"（速率不变、只压排队峰值）。
set -u

DURATION="${DURATION:-300}"
CAPS="${CAPS:-probe 2 3 4 6 8}"
SRC=ffrdp.c
SUM="sweep-summary.txt"

[ $# -ge 1 ] || { sed -n '2,26p' "$0"; exit 1; }

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
