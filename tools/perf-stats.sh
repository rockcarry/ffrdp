#!/usr/bin/env bash
# tools/perf-stats.sh —— 从 perf-eval.sh 的产物目录算"分布统计"（纯离线，不重跑、不改测试口径）
#
# 为什么需要它：perf-eval.sh 的 results.csv 每场景只留**最后一帧 dump**（resend/rtts 单值 +
# 整段平均速率），那只能回答"跑到最后是多少"，回答不了"中间抖不抖"。而 test.c 本来就每
# TEST_DUMP_INTERVAL_MS(10 s) dump 一次（`rttm: …` / `counter_resend_ratio: …` / `total_send, total_recv: …`
# / `conn age: …`），120 s 就有 12 个采样点 —— 把这份现成数据算成 min/max/avg/方差即可。
#
# 口径（报告里必须照抄这几条，否则数字对不上）：
#   · **单连接**：同一连接的 12 次 dump = 时间序列；吞吐用**区间差分** Δtotal_send/Δconn_age
#     （用累计平均会让方差假性偏小）。首帧没有"上一帧"，所以 n = dump 次数 − 1。
#   · **多连接**：横截面 —— 每条连接取它最后一次 dump 的快照（累计吞吐 = total_send / conn age），
#     再跨连接算 min/max/avg/方差；吞吐另给 **sum**（= 全部连接之和）。
#   · **方向**：上行 = 客户端日志（客户端发的），下行 = 服务端日志（服务端发的）。
#     resend / rtts 也用同一侧日志 ⇒ 每个方向的三项指标同源，不会拿服务端的 RTT 去配客户端的重传率。
#   · **方差**是**总体方差**（除以 n，不是 n−1）；单位随指标：resend = %²，rtts = ms²，吞吐 = (MB/s)²。
#
# 用法:
#   bash tools/perf-stats.sh logs/perf-1019-200633 [logs/perf-1019-202754 ...]
#   ⇒ 每个目录写 <dir>/detail.csv，并在终端打印各场景的摘要
#
# 输出列: env,scen,conn,dir,metric,n,min,max,avg,var,sum
#   conn ∈ single|full；dir ∈ up|down；metric ∈ resend|rtts|tput（resend 单位 %、rtts ms、tput MB/s）
set -u

die() { echo "✗ $*" >&2; exit 2; }

# ── 从一份 dump 日志抽样本：每行 "resend% rtts_ms tput_MBps" ────────────────────
extract() {  # $1=日志 $2=series（单连接时间序列）| xconn（每条连接最后快照）
    awk -v mode="$2" '
        /^---- conn [0-9]+/ { cid = $3 }
        /^total_send, total_recv:/ { gsub(/MB,/, "", $3); s = $3 + 0 }
        /^rttm:/  { gsub(/,/, "", $4); rt = $4 + 0 }
        /^counter_resend_ratio/ { v = $2; gsub(/%/, "", v); rs = v + 0 }
        /^conn age/ {
            age = $4 + 0
            if (age <= 0) next
            if (mode == "xconn") {          # 每条连接只要最后一份快照
                # 按 cid 分桶是**必须**的：服务端只有一份 server.log，15 条连接在里面是
                # 15 个 "---- conn N ----" 段 ⇒ 靠 cid 才能凑出 n=CLIENTS。
                # 同一 cid 重复出现（重连）= 覆盖成最后一份 ⇒ 不会重复计数 ✓
                lr[cid] = rs; lt[cid] = rt; lp[cid] = s / age
            } else {                        # 单连接：相邻两次 dump 求区间速率
                if (have && age > lag) {    # age 回退 = 重连过，重新起算
                    print rs, rt, (s - las) / (age - lag)
                }
                lag = age; las = s; have = 1
            }
        }
        END { if (mode == "xconn") for (k in lr) print lr[k], lt[k], lp[k] }' "$1"
}

# ── 统计：stdin 的 "res rtts tput" 样本 ⇒ "metric,n,min,max,avg,var,sum" ────────
stats_of() {
    awk '
        { n++; r = $1 + 0; t = $2 + 0; p = $3 + 0
          if (n == 1 || r < mnr) mnr = r; if (n == 1 || r > mxr) mxr = r; sr += r; qr += r * r
          if (n == 1 || t < mnt) mnt = t; if (n == 1 || t > mxt) mxt = t; st += t; qt += t * t
          if (n == 1 || p < mnp) mnp = p; if (n == 1 || p > mxp) mxp = p; sp += p; qp += p * p }
        END {
            if (n == 0) exit
            vr = qr / n - (sr / n) ^ 2; if (vr < 0) vr = 0
            vt = qt / n - (st / n) ^ 2; if (vt < 0) vt = 0
            vp = qp / n - (sp / n) ^ 2; if (vp < 0) vp = 0
            printf "resend,%d,%.2f,%.2f,%.2f,%.4f,-\n",  n, mnr, mxr, sr / n, vr
            printf "rtts,%d,%d,%d,%.1f,%.2f,-\n",        n, mnt, mxt, st / n, vt
            printf "tput,%d,%.3f,%.3f,%.3f,%.6f,%.3f\n", n, mnp, mxp, sp / n, vp, sp
        }'
}

[ "$#" -ge 1 ] || die "用法: bash tools/perf-stats.sh <perf 目录> [更多目录]"

for D in "$@"; do
    [ -d "$D/raw" ] || die "$D 下没有 raw/（不是 perf-eval.sh 的产物目录？）"
    OUT="$D/detail.csv"
    echo 'env,scen,conn,dir,metric,n,min,max,avg,var,sum' > "$OUT"
    for rd in "$D"/raw/*/; do
        b="$(basename "$rd")"
        env="${b%%-*}"; scen="${b#*-}"
        # REPEAT>1 时目录名是 <scen>-r<rep>（见 perf-eval.sh 的 local_st）⇒ 去掉后缀：
        # 否则同一场景会裂成 s-full-r1 / s-full-r2 两组，对不上模板里的 s-full ✗
        case "$scen" in *-r[0-9]*) scen="${scen%-r*}" ;; esac
        [ "$scen" = base ] && continue
        # lo 档是 client1.log…；真机档是 test_ffrdp_client_<stamp>[_cN].log（单连接那份没有 cN 后缀）
        CF="$(ls "$rd"/test_ffrdp_client_*.log "$rd"/client*.log 2>/dev/null)"
        SF="$(ls "$rd"/test_ffrdp_server_*.log "$rd"/server.log 2>/dev/null | head -1)"
        [ -n "$CF" ] || { echo "  ⚠ $b 没有客户端日志，跳过" >&2; continue; }
        nc="$(printf '%s\n' "$CF" | grep -c .)"
        if [ "$nc" -le 1 ]; then mode=series; conn=single; else mode=xconn; conn=full; fi
        # 上行：客户端侧（它发的方向）；下行：服务端侧
        {
            for f in $CF; do extract "$f" "$mode"; done
        } | stats_of | while IFS=, read -r m n mn mx av vr sm; do
            echo "$env,$scen,$conn,up,$m,$n,$mn,$mx,$av,$vr,$sm" >> "$OUT"
        done
        if [ -n "$SF" ]; then
            extract "$SF" "$mode" | stats_of | while IFS=, read -r m n mn mx av vr sm; do
                echo "$env,$scen,$conn,down,$m,$n,$mn,$mx,$av,$vr,$sm" >> "$OUT"
            done
        fi
        # CPU：cpu_loop 在测试期间每秒采一个点，所以 min/max/avg/σ² 都是现成的
        # （dir 列在这里是 client/server：CPU 属于"端"，不属于"方向" ✗）
        for side in client server; do
            cf="$rd/cpu.$side"
            [ -s "$cf" ] || continue
            av="$(grep -m1 '^avg_pct_of_one_core=' "$cf" 2>/dev/null | cut -d= -f2)"
            [ -n "$av" ] || continue
            mn="$(grep -m1 '^min_pct_of_one_core='  "$cf" 2>/dev/null | cut -d= -f2)"
            mx="$(grep -m1 '^peak_pct_of_one_core=' "$cf" 2>/dev/null | cut -d= -f2)"
            vr="$(grep -m1 '^var_pct_of_one_core='  "$cf" 2>/dev/null | cut -d= -f2)"
            sn="$(grep -m1 '^samples='             "$cf" 2>/dev/null | cut -d= -f2)"
            echo "$env,$scen,$conn,$side,cpu,${sn:--},${mn:--},${mx:--},$av,${vr:--},-" >> "$OUT"
        done
        echo "· $b（$conn）"
    done
    echo "⇒ $OUT"
done
