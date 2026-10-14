#!/usr/bin/env bash
# check.sh —— 一条命令跑完全部判据（真机口径编译 / 仿真 / ASAN / 本机两进程 / v1 互通）✓
#
#   用法:
#     bash tools/check.sh                 # 全量（含 ASAN 与 e2e 两组；几分钟 ✓）
#     QUICK=1 bash tools/check.sh         # 只跑快的那部分（跳过 ASAN 与 e2e ✓；约 1 分钟）
#     REBUILD_REF=1 bash tools/check.sh   # 强制重建 v1 参考二进制（默认：没有才建 ✓）
#     V1_REF=/path/to/v1_bin bash tools/check.sh    # 用指定的 v1 参考（不自动建 ✓）
#     KEEP=1 bash tools/check.sh          # 保留临时 worktree（默认清理 ✓）
#
#   结果: 全绿 exit 0；任何一项红 exit 1 ✓；明细日志在 logs/check-<时间戳>/
#
#   为什么要有它: 改协议（尤其**性能优化**那种改动 ✓）最容易在三处悄悄退步 ——
#     · 老路径（hs:0）行为 ✗   · 内存安全（只有 ASAN 会露 ✗）   · v1 互通 ✗
#   这三样都不会在"跑一下看看吞吐"里露出来 ✓，所以把六步串成一条命令 ✓，挂 pre-push 或流水线 ✓。
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1

STAMP="${STAMP:-$(date +%m%d-%H%M%S)}"
OUT="${OUT:-$ROOT/logs/check-$STAMP}"
QUICK="${QUICK:-0}"
DUR="${DUR:-8}"
PORT="${PORT:-8330}"
REF_BRANCH="${REF_BRANCH:-v1.2.x}"
REF_WT="${REF_WT:-/tmp/check-v1ref}"
REF_CACHE="${REF_CACHE:-$ROOT/logs/v1ref-$(basename "$REF_BRANCH").bin}"
KEEP="${KEEP:-0}"
mkdir -p "$OUT"

FAILS=0
NAMES=(); STATUS=(); NOTES=()
row() {   # $1=名字 $2=PASS/FAIL/SKIP $3=摘要
    NAMES+=("$1"); STATUS+=("$2"); NOTES+=("$3")
    [ "$2" = FAIL ] && FAILS=$((FAILS+1))
    printf '  %-40s %-4s %s\n' "$1" "$2" "$3"
}
step() { printf '\n\033[1m── %s\033[0m\n' "$*"; }

SIM_FAST="t_rto t_ack t_fuzz t_peer t_handshake t_crypto t_recvdata t_keepalive t_init t_dump t_fec"
SIM_ALL="$SIM_FAST e2e"
# 已知的基线失败（HEAD 侧一模一样 ⇒ 不是回归 ✓）：丢包容差那条 ✓
KNOWN_FAIL='observed loss c1->c0 far from the configured rate'

note_sim() {   # $1=日志 ⇒ 设 SIM_OK / SIM_NOTE
    local log="$1" err known unknown hits wfail
    err=$(grep -c 'error:' "$log" 2>/dev/null || true)
    known=$(grep 'FAIL:' "$log" 2>/dev/null | grep -c "$KNOWN_FAIL" || true)   # 说明用（两侧合起来数）
    hits=$(grep -c 'ERROR: AddressSanitizer' "$log" 2>/dev/null || true)
    # "未知失败"以 run.sh 的**判定汇总**为准 ✓：数 work 侧判定失败中、无法归因到已知基线的那几条 ✓。
    # 旧写法只数 `FAIL:` 行 ✗ ⇒ "只在 work 侧存在、又没有 FAIL: 行"的失败会被静静放过 ⇒ 假绿 ✗。
    # （2026-10-14 实测踩过：v2-only 的 t_dump 三条新断言全红，而 check.sh 报的是"全绿/未知失败 0" ✗。）
    # 汇总里每条失败是一对行：`FAIL: <原因>` + `verdict -> FAIL` ⇒ 按"原因是否匹配已知基线"配对 ✓。
    set -- $(awk -v known="$KNOWN_FAIL" '
        /^===== 判定汇总/          { in_sum = 1; next }
        in_sum && /判定通过/        { side = ""; next }
        in_sum && /work +判定失败/   { side = "work"; next }
        in_sum && /HEAD +判定失败/   { side = "head"; next }
        in_sum && /^  ⇒ /           { side = ""; next }
        in_sum && side == "work" {
            if ($0 ~ /^[[:space:]]+FAIL:/)      { k = ($0 ~ known) ? 1 : 0; next }
            if ($0 ~ /-> FAIL/)                 { if (k == 1) kn++; else un++; k = 0 }
        }
        END { printf "%d %d\n", kn + 0, un + 0 }
    ' "$log")
    unknown=${2:-0}
    wfail=0
    grep -q '判定汇总' "$log" || wfail=1      # 连汇总都没有（harness 中途死了 ✗）⇒ 不能算全绿 ✓
    SIM_OK=1
    [ "${err:-0}"     != 0 ] && SIM_OK=0
    [ "${unknown:-0}" != 0 ] && SIM_OK=0
    [ "${wfail:-0}"   != 0 ] && SIM_OK=0
    [ "${hits:-0}"    != 0 ] && SIM_OK=0
    SIM_NOTE="编译错误 ${err:-0}；已知基线失败 ${known:-0}；未知失败 $((unknown + wfail))；ASAN 命中 ${hits:-0}"
}
pass_count() { grep -c 'verdict -> OK' "$1" 2>/dev/null || true; }

echo "check.sh  $(date '+%F %T')    QUICK=$QUICK    OUT=$OUT"

# ── 1 真机口径编译 ───────────────────────────────────────────────────────────
step "1/6 真机口径编译（build.sh：ffrdp.c + lowlevel-udp.c + socket.c + utils.c + test.c）"
if TOOLCHAIN_NAME=gcc bash build.sh > "$OUT/build.log" 2>&1; then
    row "build.sh（真机口径 5 单元）" PASS "test_ffrdp md5 $(md5sum test_ffrdp | awk '{print $1}')"
else
    row "build.sh（真机口径 5 单元）" FAIL "见 $OUT/build.log"
fi

# ── 2 仿真：默认模型（HEAD 与工作区逐条对照 ✓）───────────────────────────────
if [ "$QUICK" = 1 ]; then TESTS_RUN="$SIM_FAST"; else TESTS_RUN="$SIM_ALL"; fi
step "2/6 仿真（默认模型；HEAD vs 工作区逐条对照 ✓）"
TESTS="$TESTS_RUN" timeout 1200 bash tools/sim/run.sh > "$OUT/sim.log" 2>&1
note_sim "$OUT/sim.log"
if [ "$SIM_OK" = 1 ]; then
    row "仿真 $(echo "$TESTS_RUN" | wc -w) 项（默认模型）" PASS "$SIM_NOTE；verdict OK $(pass_count "$OUT/sim.log") 个"
else
    row "仿真 $(echo "$TESTS_RUN" | wc -w) 项（默认模型）" FAIL "$SIM_NOTE（见 $OUT/sim.log）"
fi

# ── 3 仿真：ASAN（内存类 bug 只有这一遍露 ✓）────────────────────────────────
step "3/6 仿真 · ASAN（地址消毒剂 + 自动关 ASLR ✓）"
if [ "$QUICK" = 1 ]; then
    row "仿真 ASAN" SKIP "QUICK=1"
else
    ASAN=1 TESTS="$SIM_ALL" timeout 3000 bash tools/sim/run.sh > "$OUT/asan.log" 2>&1
    note_sim "$OUT/asan.log"
    if [ "$SIM_OK" = 1 ]; then
        row "仿真 ASAN（全套）" PASS "$SIM_NOTE"
    else
        row "仿真 ASAN（全套）" FAIL "$SIM_NOTE（见 $OUT/asan.log）"
    fi
fi

# ── 4 本机两进程：四条路径 ──────────────────────────────────────────────────
step "4/6 本机两进程·真 socket（默认 / 老路径 / 空闲+挥手 / 多客户端）"
PAIR_OK=1; PAIR_NOTE=""

pair_run() {   # $1=标签（只给人看 ✓）$2=env $3=期望(通|空闲) $4=端口
    # 文件名里只放 ASCII ✓：标签里的中文进文件名会让 `for f in $(ls "$OUT"/*)` 断掉 ✗
    # （2026-10-14 实测：日志目录里出现 pair_空闲挥手_client.log 之类 ✓）。
    case "$1" in
        默认hs1)  slug=default_hs1 ;;
        老路径)   slug=legacy_hs0 ;;
        空闲挥手) slug=idle_close ;;
        *)        slug=pair ;;
    esac
    local tag="$1" envs="${2:-}" want="$3" P="$4" sp rc cs ss
    pkill -f "test_ffrdp --server=127.0.0.1:$P" 2>/dev/null
    ( sleep 14; echo quit ) | timeout 40 env $envs stdbuf -oL ./test_ffrdp --server=127.0.0.1:$P > "$OUT/pair_${slug}_server.log" 2>&1 &
    sp=$!
    sleep 1
    ( sleep 9; echo quit ) | timeout 40 env $envs stdbuf -oL ./test_ffrdp --client=127.0.0.1:$P > "$OUT/pair_${slug}_client.log" 2>&1
    rc=$?
    wait $sp 2>/dev/null
    cs=$(grep -c 'verify: OK' "$OUT/pair_${slug}_client.log" 2>/dev/null || true)
    ss=$(grep -c 'verify: OK' "$OUT/pair_${slug}_server.log" 2>/dev/null || true)
    PAIR_OK=1
    if [ "$want" = "通" ]; then
        { [ "$cs" = 1 ] && [ "$ss" = 1 ] && [ "${rc:-1}" = 0 ]; } || PAIR_OK=0
        PAIR_NOTE="两侧 verify OK=${cs}/${ss}，客户端 rc=$rc"
    else
        grep -q 'idle mode *: on' "$OUT/pair_${slug}_client.log" 2>/dev/null || PAIR_OK=0
        grep -q 'close: 连接已断'  "$OUT/pair_${slug}_client.log" 2>/dev/null || PAIR_OK=0
        grep -q 'lost !'          "$OUT/pair_${slug}_client.log" 2>/dev/null && PAIR_OK=0
        PAIR_NOTE="'lost !' $(grep -c 'lost !' "$OUT/pair_${slug}_client.log" 2>/dev/null || true) 次（要 0 ✓），DISCONN $(grep -c 'DISCONN' "$OUT/pair_${slug}_client.log" 2>/dev/null || true) 次"
    fi
}

pair_run "默认hs1" "" "通" "$PORT"
[ "$PAIR_OK" = 1 ] && row "两进程：默认 hs:1" PASS "$PAIR_NOTE" || row "两进程：默认 hs:1" FAIL "$PAIR_NOTE（见 $OUT/pair_默认hs1_*.log）"

pair_run "老路径" "FFRDP_HS=0" "通" "$((PORT+1))"
[ "$PAIR_OK" = 1 ] && row "两进程：FFRDP_HS=0（老路径，受控 A/B 用）" PASS "$PAIR_NOTE" || row "两进程：FFRDP_HS=0（老路径）" FAIL "$PAIR_NOTE（见 $OUT/pair_老路径_*.log）"

pair_run "空闲挥手" "FFRDP_IDLE=1 FFRDP_CLOSE=1" "空闲" "$((PORT+2))"
[ "$PAIR_OK" = 1 ] && row "两进程：IDLE+CLOSE（空闲不误杀 + 四次挥手）" PASS "$PAIR_NOTE" || row "两进程：IDLE+CLOSE" FAIL "$PAIR_NOTE（见 $OUT/pair_空闲挥手_*.log）"

MC=3; MCP=$((PORT+3)); MC_PIDS=""
pkill -f "test_ffrdp --server=127.0.0.1:$MCP" 2>/dev/null
( sleep 16; echo quit ) | timeout 45 stdbuf -oL ./test_ffrdp --server=127.0.0.1:$MCP > "$OUT/mc_server.log" 2>&1 &
MSP=$!
sleep 1
mci=1
while [ "$mci" -le "$MC" ]; do
    ( sleep 10; echo quit ) | timeout 45 stdbuf -oL ./test_ffrdp --client=127.0.0.1:$MCP > "$OUT/mc_client$mci.log" 2>&1 &
    MC_PIDS="$MC_PIDS $!"
    mci=$((mci+1))
done
for p in $MC_PIDS; do wait "$p" 2>/dev/null; done
wait $MSP 2>/dev/null
mc_srv=$(grep -c 'verify: OK' "$OUT/mc_server.log" 2>/dev/null || true)
mc_cli=$(cat "$OUT"/mc_client*.log 2>/dev/null | grep -c 'verify: OK' || true)
if [ "$mc_srv" = "$MC" ] && [ "${mc_cli:-0}" = "$MC" ]; then
    row "两进程：多客户端 1×$MC" PASS "服务端 $mc_srv 条 link 各自 verify OK ✓；各客户端也都 OK ✓"
else
    row "两进程：多客户端 1×$MC" FAIL "服务端 verify OK=$mc_srv（要 $MC ✗）；客户端 OK=${mc_cli:-0}（要 $MC ✗）"
fi

# ── 5 v1 互通（V1_REF 三格 ✓）───────────────────────────────────────────────
step "5/6 v2 ↔ v1 互通自检（hs:0 两个方向必须通；默认 hs:1 必须不通）"
V1BIN="${V1_REF:-$REF_CACHE}"
if [ ! -x "$V1BIN" ]; then
    echo "     参考二进制不存在 ⇒ 从分支 $REF_BRANCH 现建一个：$V1BIN"
    git worktree remove --force "$REF_WT" 2>/dev/null; rm -rf "$REF_WT"
    if git worktree add --detach "$REF_WT" "$REF_BRANCH" >/dev/null 2>&1; then
        if ( cd "$REF_WT" && TOOLCHAIN_NAME=gcc bash build.sh > "$OUT/refbuild.log" 2>&1 ); then
            cp "$REF_WT/test_ffrdp" "$V1BIN"
            echo "     ✓ 建好 $(md5sum "$V1BIN" | awk '{print $1}')"
        else
            echo "     ✗ 参考分支编译失败（见 $OUT/refbuild.log）"
        fi
    else
        echo "     ✗ 建不出分支 $REF_BRANCH 的 worktree"
    fi
    [ "$KEEP" = 1 ] || { git worktree remove --force "$REF_WT" 2>/dev/null; rm -rf "$REF_WT"; }
fi
if [ -x "$V1BIN" ]; then
    if V1_REF="$V1BIN" DUR="$DUR" PORT="$((PORT+10))" bash test.sh ./test_ffrdp x > "$OUT/interop.log" 2>&1; then
        row "v1 互通（①②通 + ③不通）" PASS "$(grep -E '期望' "$OUT/interop.log" | grep -c '✅')/3 格 ✅（参考 $V1BIN）"
    else
        row "v1 互通（①②通 + ③不通）" FAIL "见 $OUT/interop.log"
        grep -E '①|②|③' "$OUT/interop.log" | sed 's/^/        /'
    fi
else
    row "v1 互通" FAIL "没有可用的 v1 参考二进制（用 V1_REF=... 指定 ✓）"
fi

# ── 6 汇总 ──────────────────────────────────────────────────────────────────
step "6/6 汇总"
printf '  %-40s %-4s %s\n' "项目" "结果" "摘要"
i=0
while [ "$i" -lt "${#NAMES[@]}" ]; do
    printf '  %-40s %-4s %s\n' "${NAMES[$i]}" "${STATUS[$i]}" "${NOTES[$i]}"
    i=$((i+1))
done
echo
if [ "$FAILS" = 0 ]; then
    echo "✅ 全绿（$STAMP）—— 明细日志在 $OUT"
    exit 0
fi
echo "✗ 有 $FAILS 项没过（$STAMP）—— 明细日志在 $OUT"
exit 1
