#!/usr/bin/env bash
# check.sh —— 一条命令跑完全部判据（真机口径编译 / 仿真 / ASAN / 本机两进程 / v1 互通）✓
#
#   用法:
#     bash tools/check.sh                 # 全量（含 ASAN 与 e2e 两组；几分钟 ✓）
#     QUICK=1 bash tools/check.sh         # 只跑快的那部分（跳过 ASAN / e2e / 加密那几格 ✓；约 1 分钟）
#     REBUILD_REF=1 bash tools/check.sh   # 强制重建 v1 参考二进制（默认：没有才建 ✓）
#     V1_REF=/path/to/v1_bin bash tools/check.sh    # 用指定的 v1 参考（不自动建 ✓）
#     KEEP=1 bash tools/check.sh          # 保留临时 worktree（默认清理 ✓）
#     CRYPTO_KEY=<64 hex> bash tools/check.sh       # 换掉加密那几格的 PSK（默认内置一把测试密钥 ✓）
#
#   结果: 全绿 exit 0；任何一项红 exit 1 ✓；明细日志在 logs/check-<时间戳>/
#
#   为什么要有它: 改协议（尤其**性能优化**那种改动 ✓）最容易在四处悄悄退步 ——
#     · 老路径（hs:0）行为 ✗   · 内存安全（只有 ASAN 会露 ✗）   · v1 互通 ✗
#     · 加密接线（句柄 / 盐）✗：句柄不是数组下标、盐必须每条连接每个方向各一个；
#       接错的典型表现是"第一条连接好、重连之后全乱"，而**明文跑永远看不出来** ✗
#   这四样都不会在"跑一下看看吞吐"里露出来 ✓，所以把六步串成一条命令 ✓，挂 pre-push 或流水线 ✓。
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
# 加密那几格用的 PSK（32 字节 = 64 hex）。只是测试用的一把常量密钥 ✓，不是秘密。
CRYPTO_KEY="${CRYPTO_KEY:-00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff}"
mkdir -p "$OUT"

FAILS=0
NAMES=(); STATUS=(); NOTES=()
row() {   # $1=名字 $2=PASS/FAIL/SKIP $3=摘要
    NAMES+=("$1"); STATUS+=("$2"); NOTES+=("$3")
    [ "$2" = FAIL ] && FAILS=$((FAILS+1))
    printf '  %-40s %-4s %s\n' "$1" "$2" "$3"
}
step() { printf '\n\033[1m── %s\033[0m\n' "$*"; }

SIM_FAST="t_rto t_pseudo t_cwnd t_ack t_fuzz t_peer t_handshake t_crypto t_recvdata t_flush t_keepalive t_init t_dump t_fec"
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

# ── 4 本机两进程：真 socket ─────────────────────────────────────────────────
step "4/6 本机两进程·真 socket（默认 / 老路径 / 空闲+挥手 / 多客户端 / 加密 / 重连）"
PAIR_OK=1; PAIR_NOTE=""

pair_run() {   # $1=标签（只给人看 ✓）$2=env $3=期望(通|空闲) $4=端口 $5=1 ⇒ 还要求两端真的开着加密
    # 文件名里只放 ASCII ✓：标签里的中文进文件名会让 `for f in $(ls "$OUT"/*)` 断掉 ✗
    # （2026-10-14 实测：日志目录里出现 pair_空闲挥手_client.log 之类 ✓）。
    case "$1" in
        默认hs1)  slug=default_hs1 ;;
        老路径)   slug=legacy_hs0 ;;
        空闲挥手) slug=idle_close ;;
        延迟量具) slug=latency ;;
        加密)     slug=crypto ;;
        *)        slug=pair ;;
    esac
    local tag="$1" envs="${2:-}" want="$3" P="$4" crypto="${5:-0}" sp rc cs ss
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
        # 加密格额外要求：两端启动都打了 `encryption : on` 且自检 ok ✓。
        # 少了这条，`ffc_new` 失败 / 自检不过时程序会**静默退回明文**，verify 照样全绿 ✗ ⇒ 假绿 ✗。
        if [ "$crypto" = 1 ]; then
            local on
            on=$(cat "$OUT/pair_${slug}_"*.log 2>/dev/null | grep -c 'encryption *: on,.*selftest ok' || true)
            [ "${on:-0}" = 2 ] || PAIR_OK=0
            PAIR_NOTE="$PAIR_NOTE；encryption on + selftest ok 的进程 ${on:-0}/2"
        fi
    else
        grep -q 'idle mode *: on' "$OUT/pair_${slug}_client.log" 2>/dev/null || PAIR_OK=0
        grep -q 'close: peer FIN received' "$OUT/pair_${slug}_client.log" 2>/dev/null || PAIR_OK=0
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

# 4b) A1：逐消息延迟量具（notes §26.93）。它给消息尾部加 4 字节发送时刻 ⇒ 只在**两端同开**
#     TEST_STAMP=1 时才改格式 ✓（v1 互通那一格用的是另一份旧格式的 test_ffrdp_v11_* ✓）。
#     判据仍是 verify OK（crc/seq 一条不少 ✓）；分位数只打出来给人看，暂不设阈值 ✗。
pair_run "延迟量具" "TEST_STAMP=1" "通" "$((PORT+4))"
if [ "$PAIR_OK" = 1 ]; then
    lat_line=$(grep -h 'latency ms:' "$OUT"/pair_latency_*.log 2>/dev/null | head -1)
    row "两进程：TEST_STAMP=1（逐消息延迟量具）" PASS "$PAIR_NOTE；${lat_line:-no latency line}"
else
    row "两进程：TEST_STAMP=1（逐消息延迟量具）" FAIL "$PAIR_NOTE（见 $OUT/pair_latency_*.log）"
fi

# 4c) 加密：ENCRYPT / DECRYPT 回调 + ffcrypto（接入方式见 docs/ffrdp-crypto.md）。
#     判据除了"两侧 verify OK"，还要求两端**真的**开着加密（pair_run 的第 5 个参数 ✓）：
#     `ffc_new` 失败或自检不过时程序会**静默退回明文**，verify 照样全绿 ✗ ⇒ 只看 verify 就是假绿 ✗。
if [ "$QUICK" = 1 ]; then
    row "两进程：加密（FFRDP_KEY）" SKIP "QUICK=1"
else
    pair_run "加密" "FFRDP_KEY=$CRYPTO_KEY" "通" "$((PORT+5))" 1
    [ "$PAIR_OK" = 1 ] && row "两进程：加密（FFRDP_KEY，ChaCha20）" PASS "$PAIR_NOTE" \
                       || row "两进程：加密（FFRDP_KEY）" FAIL "$PAIR_NOTE（见 $OUT/pair_crypto_*.log）"
fi

# 多客户端：`env` 一栏同时用来挂 FFRDP_KEY ⇒ 顺带覆盖"每条连接每个方向各一个盐"。
# 共用一段密钥流就是 two-time pad ✗，而 verify 依旧是绿的 ⇒ 只能靠"加密真的开着"这条盯着。
mc_run() {   # $1=标签 $2=slug $3=env $4=客户端数 ⇒ 设 MC_SRV / MC_CLI
    local slug="$2" envs="${3:-}" n="$4" MCP=$((PORT+3)) MSP pids="" i=1
    pkill -f "test_ffrdp --server=127.0.0.1:$MCP" 2>/dev/null
    ( sleep 16; echo quit ) | timeout 45 env $envs stdbuf -oL ./test_ffrdp --server=127.0.0.1:$MCP > "$OUT/mc_${slug}_server.log" 2>&1 &
    MSP=$!
    sleep 1
    while [ "$i" -le "$n" ]; do
        ( sleep 10; echo quit ) | timeout 45 env $envs stdbuf -oL ./test_ffrdp --client=127.0.0.1:$MCP > "$OUT/mc_${slug}_client$i.log" 2>&1 &
        pids="$pids $!"
        i=$((i+1))
    done
    for p in $pids; do wait "$p" 2>/dev/null; done
    wait $MSP 2>/dev/null
    MC_SRV=$(grep -c 'verify: OK' "$OUT/mc_${slug}_server.log" 2>/dev/null || true)
    MC_CLI=$(cat "$OUT"/mc_${slug}_client*.log 2>/dev/null | grep -c 'verify: OK' || true)
}

MC=3
mc_run "多客户端" plain "" "$MC"
if [ "$MC_SRV" = "$MC" ] && [ "${MC_CLI:-0}" = "$MC" ]; then
    row "两进程：多客户端 1×$MC" PASS "服务端 $MC_SRV 条 link 各自 verify OK ✓；各客户端也都 OK ✓"
else
    row "两进程：多客户端 1×$MC" FAIL "服务端 verify OK=$MC_SRV（要 $MC ✗）；客户端 OK=${MC_CLI:-0}（要 $MC ✗）"
fi

if [ "$QUICK" = 1 ]; then
    row "两进程：加密 + 多客户端 1×$MC" SKIP "QUICK=1"
else
    mc_run "加密多客户端" crypto "FFRDP_KEY=$CRYPTO_KEY" "$MC"
    mc_on=$(cat "$OUT"/mc_crypto_*.log 2>/dev/null | grep -c 'encryption *: on,.*selftest ok' || true)
    if [ "$MC_SRV" = "$MC" ] && [ "${MC_CLI:-0}" = "$MC" ] && [ "${mc_on:-0}" = "$((MC+1))" ]; then
        row "两进程：加密 + 多客户端 1×$MC" PASS "服务端 $MC_SRV 条 link verify OK ✓；客户端 $MC 个 verify OK ✓；开着加密的进程 ${mc_on} 个 ✓"
    else
        row "两进程：加密 + 多客户端 1×$MC" FAIL "verify OK 服务端=${MC_SRV:-0} 客户端=${MC_CLI:-0}（各要 $MC ✗）；开着加密的进程 ${mc_on:-0}（要 $((MC+1)) ✗）"
    fi
fi

# 4d) 重连 = 句柄复用。句柄 = 代号 × FFRDP_MAXCONN + 槽号，槽位回收后代号 +1 ⇒ **第 2 条连接起
#     句柄就是 17、33…**（不是 1、2、3 ✗）。拿句柄当数组下标就在这一刻越界，而越界在 ffcrypto 里是
#     **静默不加密** ⇒ 那几条连接线上是明文。所以这一格必须逐条看 crc/len：只看"两侧 verify OK"
#     发现不了（服务端收得没问题，回出去的那份才是明文 ✗）。
if [ "$QUICK" = 1 ]; then
    row "两进程：加密 + 重连 3 次（句柄 1→33）" SKIP "QUICK=1"
else
    RC_N=3; RCP=$((PORT+6)); RC_OK=1
    pkill -f "test_ffrdp --server=127.0.0.1:$RCP" 2>/dev/null
    ( sleep $((RC_N * 3 + 14)); echo quit ) | timeout $((RC_N * 3 + 45)) \
        env FFRDP_KEY=$CRYPTO_KEY stdbuf -oL ./test_ffrdp --server=127.0.0.1:$RCP > "$OUT/rc_server.log" 2>&1 &
    RSP=$!
    sleep 1
    rci=1; rc_cli=0
    while [ "$rci" -le "$RC_N" ]; do
        ( sleep 2; echo quit ) | timeout 40 env FFRDP_KEY=$CRYPTO_KEY stdbuf -oL ./test_ffrdp --client=127.0.0.1:$RCP > "$OUT/rc_client$rci.log" 2>&1
        grep -q 'verify: OK' "$OUT/rc_client$rci.log" && rc_cli=$((rc_cli + 1))
        rci=$((rci + 1))
    done
    wait $RSP 2>/dev/null
    rc_srv=$(grep -c 'verify: OK' "$OUT/rc_server.log" 2>/dev/null || true)
    rc_bad=$(cat "$OUT"/rc_*.log 2>/dev/null | grep 'verify: sent' | grep -vc 'crc_err 0, len_err 0' || true)
    rc_on=$(cat "$OUT"/rc_*.log 2>/dev/null | grep -c 'encryption *: on,.*selftest ok' || true)
    RC_NOTE="客户端 verify OK=$rc_cli/$RC_N；服务端 link verify OK=${rc_srv:-0}/$RC_N；crc/len 非零行 ${rc_bad:-0}；开着加密的进程 ${rc_on:-0}/$((RC_N + 1))"
    { [ "$rc_cli" = "$RC_N" ] && [ "${rc_srv:-0}" = "$RC_N" ] && [ "${rc_bad:-0}" = 0 ] && [ "${rc_on:-0}" = "$((RC_N + 1))" ]; } || RC_OK=0
    if [ "$RC_OK" = 1 ]; then
        row "两进程：加密 + 重连 $RC_N 次（句柄 1→33）" PASS "$RC_NOTE"
    else
        row "两进程：加密 + 重连 $RC_N 次（句柄 1→33）" FAIL "$RC_NOTE（见 $OUT/rc_*.log）"
    fi
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
