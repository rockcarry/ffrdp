#!/bin/bash
# keylog.sh —— 从 test_ffrdp 的输出里只留"判断乱序掩码该不该加宽"要看的那些行，
#              并给出最后一块的汇总 + 逐块趋势。
#
#   bash tools/keylog.sh run [test_ffrdp 的参数...]   # 跑起来：全量存 tf-<时间>.log，关键行存 .key，末尾打印汇总+趋势
#   bash tools/keylog.sh <已有日志>                    # 只过滤：写出 <日志>.key，并打印汇总+趋势
#   BLOCKSTEP=10 bash tools/keylog.sh <日志>           # 趋势每 10 块抽 1 块（默认 5）
#
# 用法要点（与 docs/notes.md 5.23 一致）：
#   · 停止请用 quit / exit（Ctrl+C 不会打印 verify 汇总，见 notes 5.21）；
#   · dump 里的计数是**累计**的（test.c 传 ffrdp_dump(..., 0)）⇒ 本脚本只取"最后一块"看总量、
#     看逐块趋势，**绝不求和**；要增量请自己对相邻两块作差；
#   · 一个日志里若混了多段运行，跨运行的数字不要互相比较（块数偏多时脚本会提醒）。
set -u

BLOCKSTEP="${BLOCKSTEP:-5}"

# 只留这些行。想更窄就删几个字段；想更全就把 counter_ 那段放宽（例如改成 counter_）。
KEY='^(version & build|build|elapsed|total_send|rtt_min|swnd|wait_snd|inflight|in-flight limit|probe trigger|probe rtt|queue test|confirmed rate|lap period|rto_floor|counter_(recv_ooo|ooo_d|ooo_64reach|ooo_128reach|mack_bits|resend_|ack_(mack|hole|dup)|recv_dup|timeout_evt|send_1sttime|send_failed|reach_maxrto|probe|giveback))'

summarize() {   # $1 = key 文件
    awk -v step="$BLOCKSTEP" '
    function val(  s) { s=$0; sub(/^[^:]*:[ ]*/,"",s); gsub(/\r/,"",s); return s }
    function num(s)   { gsub(/[^0-9.]/,"",s); return s+0 }

    # 收一块（用"上一块"已累计好的那组值），按 %step 抽样放进趋势
    function sample() {
        if (blk > 0 && (blk == 1 || blk % step == 0))
            tr[++nt] = sprintf("    %4ss   perMB %-6s  rto %-5d fast %-4d  ooo %-6d (25..64 %-5d >64 %-5d)   infl %s",
                               el, pmb, m["rto"], m["fast"], m["ooo"], m["b2"], m["b3"], ifl)
    }

    # 块头：dump 现在把 version 与 build 合成一行 `version & build : …` ✓；
    # 老二进制与旧日志仍是 `build  : …` ⇒ 两种都认 ✓（否则这里一块都数不出来 ✗）。
    /^(version & build|build)  *:/ { sample(); blk++; b=$0; sub(/.*compiled /,"compiled ",b); sub(/\r/,"",b); next }
    /^elapsed /  { el=val(); sub(/ .*/,"",el) }

    # 下面这些都是"最后出现的那个值"生效 —— 块内计数是累计的，所以最后一块 = 该进程的累计终点。
    # 不能取全程最大值：一个日志里可能有多段运行，最大值会把不同进程的数字拼在一起。
    /^total_send, total_recv:/ { t=val(); sub(/,.*/,"",t); m["sent"]=num(t) }
    /^rtt_min, rtts, rttd :/   { v=val(); split(v,a,/ *, */); m["rmin"]=a[1]; m["rtts"]=a[2]; m["rttd"]=a[3] }
    /^swnd, cwnd, ssthresh:/   { v=val(); split(v,a,/ *, */); m["swnd"]=a[1]; m["cwnd"]=a[2] }
    /^wait_snd /               { m["wait"]=val() }
    /^confirmed rate /         { m["crate"]=val() }
    /^inflight, cap /          { v=val(); split(v,a," "); gsub(/,/,"",a[1]); gsub(/,/,"",a[2]); m["infl"]=a[1]; m["cap"]=a[2]; ifl=a[1]"/"a[2] }
    /^counter_send_1sttime/    { m["first"]=num(val()) }
    /^counter_send_failed /    { m["sfail"]=num(val()) }
    /^counter_resend_rto /     { m["rto"]=num(val()) }
    /^counter_resend_fast /    { m["fast"]=num(val()) }
    /^counter_resend_perMB/    { m["pmb"]=val(); pmb=val() }
    /^counter_resend_waste/    { m["waste"]=val() }
    /^counter_recv_ooo /       { m["ooo"]=num(val()) }
    /^counter_recv_far /       { m["far"]=num(val()) }
    /^counter_ooo_d1_24 /      { m["b1"]=num(val()) }
    /^counter_ooo_d25_64 /     { m["b2"]=num(val()) }
    /^counter_ooo_dgt64 /      { m["b3"]=num(val()) }
    /^counter_ooo_d65_128 /    { m["d65"]=num(val()) }
    /^counter_ooo_dgt128 /     { m["d128"]=num(val()) }
    /^counter_mack_bits_tx/    { m["mbs"]=num(val()) }   # 计数器改名了（sent → tx ✓）
    /^counter_mack_bits_rx/    { m["mbr"]=num(val()) }   # （recv → rx ✓）
    /^counter_ack_mack /       { m["amack"]=num(val()) }
    /^counter_ack_hole /       { m["ahole"]=num(val()) }
    /^counter_recv_dup /       { m["rdup"]=num(val()) }
    /^counter_ack_dup /        { m["adup"]=num(val()) }
    /^counter_timeout_evt /    { m["tev"]=num(val()) }

    END {
        sample()
        o=m["ooo"]; b1=m["b1"]; b2=m["b2"]; b3=m["b3"]; d65=m["d65"]; d128=m["d128"]
        printf "\n==== 汇总（取最后一块；%s，elapsed %s s；本文件共 %d 块 dump）====\n", b, el, blk
        if (blk > 25) printf "注意：共 %d 块 ⇒ 可能混了多段运行；跨运行的数字不要互相比较（最好一次运行一个日志）\n", blk
        printf "【本端发送方向】首发 %d 帧 / 累计 %s MB   重传 rto %d + fast %d = %d（每MB %.1f）\n",
               m["first"], m["sent"], m["rto"], m["fast"], m["rto"]+m["fast"], m["pmb"]
        printf "                 白费 %s   超时事件 %d   背压失败 %d   rtt_min/rtts/rttd %d/%d/%d ms\n",
               m["waste"], m["tev"], m["sfail"], m["rmin"], m["rtts"], m["rttd"]
        printf "                 swnd/cwnd %d/%d   wait_snd %d   inflight/cap %d/%d   确认速率 %d 帧/s\n",
               m["swnd"], m["cwnd"], m["wait"], m["infl"], m["cap"], m["crate"]
        printf "【本端收到的乱序】(= 对端发送方向) recv_ooo %d\n", o
        if (o) {
            printf "                 1..24 %d (%.1f%%)  25..64 %d (%.1f%%)  65..128 %d (%.1f%%)  >128 %d (%.1f%%)\n",
                   b1, 100*b1/o, b2, 100*b2/o, d65, 100*d65/o, d128, 100*d128/o
            printf "                 ⇒ 今天(24位)可达 %.1f%%   64位 %.1f%%   128位 %.1f%%\n",
                   100*b1/o, 100*(b1+b2)/o, 100*(b1+b2+d65)/o
        }
        printf "【上报】mack 位数：发出 %d / 收到 %d   （带位图 ACK %d 条，报洞 %d 条）\n", m["mbs"], m["mbr"], m["amack"], m["ahole"]
        printf "【浪费】本端收到的重复帧 %d   本端被确认白费的重传 %d\n", m["rdup"], m["adup"]
        printf "\n判据（三条同时成立才值得加宽，见 notes 5.23）：\n"
        printf "  ① 发送侧每MB重传是否明显高？（坏链路参考 18.7~124.5）     本次 %.1f%s\n", m["pmb"],
               (m["pmb"] >= 5 ? "  ← 偏高" : "")
        if (m["rto"]+m["fast"] > 0)
            printf "  ② fast 占重传比例（>20%% 才与位图有关，全 rto 则无关）     本次 %.1f%%%s\n",
                   100*m["fast"]/(m["rto"]+m["fast"]), (100*m["fast"]/(m["rto"]+m["fast"]) > 20 ? "  ← 成立" : "  ← 不成立")
        else printf "  ② fast 占重传比例：本次没有重传，无从判断\n"
        if (o) printf "  ③ 接收侧报不回去的乱序占比（今天只够 1..24）              本次 %.1f%%%s\n",
                    100*(o-b1)/o, (100*(o-b1)/o > 20 ? "  ← 成立" : "  ← 不成立")
        else   printf "  ③ 接收侧没有乱序（0 次）⇒ 加宽掩码毫无意义  ← 不成立\n"

        if (nt) {
            printf "\n==== 逐块趋势（每 %d 块抽 1 块；计数是累计的，看是否在涨，不比绝对值）====\n", step
            for (i=1;i<=nt;i++) print tr[i]
        }
    }' "$1"
}

case "${1:-}" in
"")
    sed -n '3,10p' "$0"
    ;;
run)
    shift
    ts=$(date +%Y%m%d-%H%M%S)
    log="tf-$ts.log"
    echo "[keylog] 运行 ./test_ffrdp $*  ⇒ 全量 $log，关键行 $log.key"
    echo "[keylog] 停止请在该终端输入 quit 或 exit（Ctrl+C 不会打印 verify 汇总）"
    ./test_ffrdp "$@" 2>&1 | tee "$log"
    grep -aE "$KEY" "$log" > "$log.key"
    echo "[keylog] 关键行已写入 $log.key（$(wc -l < "$log.key") 行，原日志 $(wc -l < "$log") 行）"
    summarize "$log.key"
    ;;
*)
    f="$1"
    [ -f "$f" ] || { echo "找不到文件：$f"; exit 1; }
    grep -aE "$KEY" "$f" > "$f.key"
    echo "[keylog] 关键行已写入 $f.key（$(wc -l < "$f.key") 行，原日志 $(wc -l < "$f") 行）"
    summarize "$f.key"
    ;;
esac
