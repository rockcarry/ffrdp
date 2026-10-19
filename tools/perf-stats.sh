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

# ── 单连接：从一份 dump 日志抽样本，每行 "resend% rtts_ms tput_MBps" ───────────
# 第一个区间含握手/慢启动前段，速率明显偏低（甚至因为分母小反而偏高）⇒ 默认丢掉 ✓
SKIP_FIRST="${SKIP_FIRST:-1}"
extract() {  # $1=日志 ⇒ 相邻两次 dump 的区间差分（累计平均会让方差假性偏小 ✗）
    awk -v skip="$SKIP_FIRST" '
        /^---- conn [0-9]+/ { cid = $3 }
        /^total_send, total_recv:/ { gsub(/MB,/, "", $3); s = $3 + 0 }
        /^rttm:/  { gsub(/,/, "", $4); rt = $4 + 0 }
        /^counter_resend_ratio/ { v = $2; gsub(/%/, "", v); rs = v + 0 }
        /^conn age/ {
            age = $4 + 0
            if (age <= 0) next
            if (have && age > lag) {                 # age 回退 = 重连过，重新起算
                n++
                if (n > skip) print rs, rt, (s - las) / (age - lag)
            }
            lag = age; las = s; have = 1
        }' "$1"
}

# ── 多连接：两阶段口径 ──────────────────────────────────────────────────────
# 阶段 1：一份日志 ⇒ 每行 "文件序号 连接号 区间序号 吞吐 resend rtts"
#   服务端的 15 条连接在**同一份** server.log 里（15 个 conn 段）⇒ 靠 cid 分开；
#   客户端是 15 份日志 ⇒ 靠文件序号分开。两条来源都送进阶段 2，口径一致 ✓
extract_rows() {  # $1=日志 $2=文件序号
    awk -v fid="$2" -v skip="$SKIP_FIRST" '
        # ⚠ 每个 dump 块都会重复打 "#---- conn N ----" ⇒ 只能**首次**初始化计数，
        # 原地重置会让 it 永远是 0 ⇒ 每个区间都落进"首区间"被跳过（一行都出不来 ✗）
        /^---- conn [0-9]+/ { cid = $3; if (!(cid in it)) it[cid] = 0; next }
        /^total_send, total_recv:/ { v = $3; gsub(/MB,?/, "", v); cum[cid] = v + 0 }
        /^rttm:/  { v = $4; gsub(/,/, "", v); rt[cid] = v + 0 }
        /^counter_resend_ratio/ { v = $2; gsub(/%/, "", v); rs[cid] = v + 0 }
        /^conn age/ {
            age = $4 + 0
            if (age <= 0) next
            # 重连：age 大幅回退（远大于 dump 周期）⇒ 序号重新起算
            if (pag[cid] > 0 && age + 20 < pag[cid]) { it[cid] = 0; pag[cid] = 0 }
            # ⚠ 服务端是**每个 link 线程各自 dump**（15 条连接 ⇒ 15 份全表快照交织），
            #   同一个 age 会重复出现很多次，且相位差会让年龄偶发"回退"。
            #   落后的采样必须直接丢弃：不丢的话每次抖动都算一个新区间，还会产出 0 值样本，
            #   把分布整体拉低（实测 f-full 的 down 被算成 73 个区间 ✗）。
            if (age <= pag[cid]) next
            if (pag[cid] > 0) {
                it[cid]++
                if (it[cid] > skip)
                    printf "%s %s %d %.6f %.4f %.2f\n", fid, cid, it[cid],
                           (cum[cid] - pcum[cid]) / (age - pag[cid]), rs[cid], rt[cid]
            }
            pag[cid] = age; pcum[cid] = cum[cid]
        }' "$1"
}

# 阶段 2：聚合成 detail.csv 的行
#   · 每连接的**时间均值**（三个指标各一个）——先把时间维度压掉 ✓
#   · 跨连接：对这些均值求 min/max/avg/方差（n = 连接数）
#   · 总量序列：同一区间序号把各连接相加 ⇒ 再求 min/max/avg/方差（n = 区间数）
#     ⚠ 只取"所有连接都有样本"的区间，否则缺一条连接的区间会让总量假性偏低 ✗
aggregate_rows() {  # $1=env $2=scen $3=dir
    awk -v env="$1" -v scen="$2" -v dir="$3" '
        {
            k = $1 ":" $2
            if (!(k in nk)) nk[k] = 0
            nk[k]++
            sp[k] += $4; sr[k] += $5; st[k] += $6
            tot[$3] += $4; cnt[$3]++
            if ($3 > itmax) itmax = $3
        }
        END {
            m = 0
            for (k in nk) {
                m++
                pm = sp[k] / nk[k]
                if (m == 1 || pm < mnp) mnp = pm; if (m == 1 || pm > mxp) mxp = pm; ap += pm; qp += pm * pm
                rm = sr[k] / nk[k]
                if (m == 1 || rm < mnr) mnr = rm; if (m == 1 || rm > mxr) mxr = rm; ar += rm; qr += rm * rm
                tm = st[k] / nk[k]
                if (m == 1 || tm < mnt) mnt = tm; if (m == 1 || tm > mxt) mxt = tm; at += tm; qt += tm * tm
            }
            if (m == 0) exit
            vr = qr / m - (ar / m) ^ 2; if (vr < 0) vr = 0
            vt = qt / m - (at / m) ^ 2; if (vt < 0) vt = 0
            vp = qp / m - (ap / m) ^ 2; if (vp < 0) vp = 0
            printf "%s,%s,full,%s,resend,%d,%.2f,%.2f,%.2f,%.4f,-\n",    env, scen, dir, m, mnr, mxr, ar / m, vr
            printf "%s,%s,full,%s,rtts,%d,%.1f,%.1f,%.1f,%.2f,-\n",      env, scen, dir, m, mnt, mxt, at / m, vt
            printf "%s,%s,full,%s,tput,%d,%.3f,%.3f,%.3f,%.6f,%.3f\n",   env, scen, dir, m, mnp, mxp, ap / m, vp, ap
            # 总量：按"区间序号"把各连接相加。各连接是错峰启动的 ⇒ 区间数会差 1~2 个，
            # 所以取**样本最多的那批区间**（maxc），而不是要求"全都有"（那样常常一个都不剩 ✗）。
            # n 列给出实际参与的对齐度：n == 连接数 才是完美的（错峰越小越接近）。
            maxc = 0
            for (i = 1; i <= itmax; i++) if (cnt[i] > maxc) maxc = cnt[i]
            nt = 0
            # 对齐度门槛：maxc 至少要到连接数的一半，否则那些"只有一两条连接有样本"的区间
            # 会被当成总量（n 会把区间数暴露成 70+ 这种明显不对的值 ✗）—— 宁可不给，也不给错的
            if (maxc * 2 >= m) {
                for (i = 1; i <= itmax; i++) {
                    if ((i in tot) && cnt[i] == maxc) {
                        g = tot[i]; nt++
                        if (nt == 1 || g < mng) mng = g; if (nt == 1 || g > mxg) mxg = g; ag += g; qg += g * g
                    }
                }
            }
            if (nt > 0) {
                vg = qg / nt - (ag / nt) ^ 2; if (vg < 0) vg = 0
                printf "%s,%s,full,%s,total,%d,%.3f,%.3f,%.3f,%.6f,-\n", env, scen, dir, nt, mng, mxg, ag / nt, vg
            }
        }'
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
        if [ "$nc" -le 1 ]; then conn=single; else conn=full; fi
        # 上行：客户端侧（它发的方向）；下行：服务端侧
        if [ "$nc" -le 1 ]; then
            # 单连接：同一连接的 N 次 dump = 时间序列（区间差分）
            for f in $CF; do extract "$f"; done | stats_of | while IFS=, read -r m n mn mx av vr sm; do
                echo "$env,$scen,$conn,up,$m,$n,$mn,$mx,$av,$vr,$sm" >> "$OUT"
            done
            if [ -n "$SF" ]; then
                extract "$SF" | stats_of | while IFS=, read -r m n mn mx av vr sm; do
                    echo "$env,$scen,$conn,down,$m,$n,$mn,$mx,$av,$vr,$sm" >> "$OUT"
                done
            fi
        else
            # 多连接：两阶段（每连接时间均值 ⇒ 跨连接分布）+ 总量序列
            i=0
            for f in $CF; do i=$((i + 1)); extract_rows "$f" "$i"; done \
                | aggregate_rows "$env" "$scen" up >> "$OUT"
            if [ -n "$SF" ]; then
                extract_rows "$SF" 1 | aggregate_rows "$env" "$scen" down >> "$OUT"
            fi
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
