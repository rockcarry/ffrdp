#!/bin/bash
# Build every test twice: against git HEAD (ffrdp_base.c) and against the current
# working tree (ffrdp_new.c), then run both.  The clock is virtualised and loss is
# hash driven, so the two runs see byte-identical network conditions - a difference
# is a real difference, not run-to-run noise.
#
#   bash run.sh            # everything
#   TESTS="t_rto" bash run.sh
set -e
cd "$(dirname "$0")"

# Virtualise the clock by replacing that ONE statement of ffrdp_get_timestamp32_ms (utils.c). Matched on
# its whole shape rather than on "ts.tv_nsec", because that field also appears in the layer's own deadline
# arithmetic: a looser match once rewrote a comment and put a bare statement at file scope.
# The clock is one implementation for the whole process, so one rewritten copy serves both sides -- unlike
# ffrdp.c, which is compiled once per side and therefore keeps its -base/-new pair below.
vtick() { sed 's|^\( *\)uint32_t ms = ((uint32_t)ts\.tv_sec \* 1000 + ts\.tv_nsec / 1000000);|\1uint32_t ms = sim_tick();|' ; }
if [ -n "$REF" ]; then cp "$REF" ffrdp_base.c; else git -C ../.. show HEAD:ffrdp.c > ffrdp_base.c; fi
cp ../../ffrdp.c ffrdp_new.c
vtick < ../../utils.c > utils_sim.c
grep -q sim_tick utils_sim.c || { echo "FATAL: failed to virtualise the clock"; exit 1; }

# The header is versioned too, and each side is pointed at its own copy. It only matters while an interface
# change is in flight -- ffrdp_send/recv gained a timeout argument, so HEAD's implementation and the working
# tree's no longer agree on what the header declares, and compiling both against one header is a
# conflicting-types error rather than a difference worth reporting.
git -C ../.. show HEAD:ffrdp.h > ffrdp_base.h
cp ../../ffrdp.h ffrdp_new.h
sed -i 's|#include "ffrdp.h"|#include "ffrdp_base.h"|' ffrdp_base.c
sed -i 's|#include "ffrdp.h"|#include "ffrdp_new.h"|'  ffrdp_new.c

# The transport header is versioned for the same reason. This change drops a slot from the lowlevel
# vtable (addr_parse -- the text->addr convention moved to utils.h, where both layers can see it), so
# HEAD's implementation and the working tree's no longer agree on the shape of FFRDP_LOWLEVEL. Each
# side gets its own copy; the fake lowlevel is compiled once per side below, so what it fills in
# always matches the header that side calls through.
git -C ../.. show HEAD:lowlevel.h > lowlevel_base.h
cp ../../lowlevel.h lowlevel_new.h
sed -i 's|#include "lowlevel.h"|#include "lowlevel_base.h"|' ffrdp_base.c
sed -i 's|#include "lowlevel.h"|#include "lowlevel_new.h"|'  ffrdp_new.c

# ASAN=1 turns on the address sanitizer. A read one byte past a buffer usually does not crash, so
# without this the fuzz test can only see the fragments that kill the process outright.
#
# halt_on_error=1 is the important one, and it is not a preference. The sanitizer's default is to report
# an error and then CARRY ON -- and in a crash test the process carrying on is by definition one whose
# state is already broken (the fragment that gets here is the one that made a 4GB memcpy land on a 190KB
# ring buffer). It then faults again inside the sanitizer's own handler, the handler re-enters, and the
# same line is printed for ever: measured on the vulnerable code, 90 seconds of that produced 1.5GB of
# identical output -- 51 million DEADLYSIGNAL lines, 17MB/s -- which matches the ~19GB a previous
# unbounded run accumulated before anyone looked. With halt_on_error=1 the same run stops after 81KB and
# names the 8 fragments that crashed. detect_leaks=0 drops report noise from a harness that does not
# bother freeing in a child that is about to exit, and the core limit removes the other way a crash can
# leave bulk behind. Do not add handle_segv=0 style options here: measured, they make the whole run die
# on the first crash instead of reporting it.
#
# RUN is empty normally and becomes a setarch prefix on every sanitized binary when ASAN=1, and that prefix
# is not optional either. On this kernel the sanitizer sometimes cannot place its shadow memory at startup,
# because the address space it is handed is randomised further than that layout can absorb: measured on the
# sanitized t_rto binary, 5 runs out of 20 died with "AddressSanitizer:DEADLYSIGNAL" before main() was
# reached, and because the handler re-enters, the whole failure is that one line repeated until the pipe
# closes -- the same flood described above, but from a crash that has nothing to do with the code under
# test, which is exactly the sort of thing a crash test must not have to distinguish. It is not this code:
# with ASLR off for that process the same binary ran 20 out of 20 clean. setarch -R does that for the one
# process, needs no privileges, and does not weaken what the sanitizer is being used for. halt_on_error=1
# does not cover this case: the flood comes from the handler re-entering, not from a report repeating.
ASANFLAGS=""
RUN=""
if [ -n "$ASAN" ]; then
    ASANFLAGS="-fsanitize=address -fno-omit-frame-pointer -g"
    export ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}detect_leaks=0:halt_on_error=1"
    ulimit -c 0
    if command -v setarch > /dev/null 2>&1; then
        RUN="setarch $(uname -m) -R"
    else
        echo "warning: setarch not found, the sanitizer can fail to start at random under high entropy ASLR"
    fi
fi

CFLAGS="-O1 -w -I../.. -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -DFFRDP_DIAG=1 -DFFRDP_FEC_ENABLE=1 -include simdefs.h \
 -D'calloc(n,s)=sim_calloc(n,s)' -D'free(p)=sim_free(p)' \
 -D'sendto(s,b,l,f,d,a)=sim_send(b,l)' -D'recvfrom(s,b,l,f,d,a)=sim_recv(b,l)' -D'select(n,r,w,e,t)=sim_select()' \
 -D'ffrdp_udp_lowlevel_init(p,cb,ctx)=sim_ll_init(p,cb,ctx)'"

gcc -O1 -w -c simnet.c -o simnet.o
# The working tree no longer owns a socket: ffrdp_init asks lowlevel for a transport, so the fake
# socket macros above only matter to the HEAD side. The new side is given a fake lowlevel instead --
# simll.c -- and the function-like macro is what redirects the one call ffrdp_init makes for it.
# The fake is compiled once per side, against that side's lowlevel.h (-DSIMLL_BASE picks which).
# Whether it also has to fill in the old addr_parse slot is NOT a property of the side -- it is a
# property of that side's header, and while an interface change is in flight HEAD can be on either
# side of it. So probe the header. (The first version of this assumed "base always has the slot" and
# stopped compiling the moment the change landed in HEAD itself.)
HAVE_N=""; grep -q '(*addr_parse)' lowlevel_new.h  && HAVE_N="-DSIMLL_NEW_HAS_ADDR_PARSE"
HAVE_B=""; grep -q '(*addr_parse)' lowlevel_base.h && HAVE_B="-DSIMLL_BASE_HAS_ADDR_PARSE"
gcc -O1 -w $HAVE_N -c simll.c -o simll.o
gcc -O1 -w $HAVE_B -DSIMLL_BASE -c simll.c -o simll_base.o
# The addr convention (ffrdp_addr_make / _split / _parse_text) now lives in utils.c and the working
# side resolves "ip:port" through it, so it has to be linked in.
# utils.c lives one level up (run.sh works from tools/sim).
# -I../..: the copy sits here, so its quoted includes ("utils.h") need the repo root on the path.
gcc -O1 -w -I../.. -c utils_sim.c -o utils.o
TESTS="${TESTS:-t_rto t_pseudo t_cwnd t_ack t_fuzz t_peer t_handshake t_crypto t_recvdata t_flush t_probe t_sweep t_cap t_keepalive t_init t_dump t_fec hs-e2e e2e-raw e2e}"

# 每一侧的输出同时进终端与一份日志：末尾的"判定汇总"从日志里读，免得靠人眼看统计数字。
# 统计行（resend%、rto_floor、ooo 直方图……）随负载与旋钮变化，不是判据。
SIMLOG="${SIMLOG:-/tmp/ffrdp-sim-harness.log}"
: > "$SIMLOG"
run() { "$@" 2>&1 | tee -a "$SIMLOG" || true; }
for t in $TESTS; do
    # v2-only 的测试（接口只有工作区那份才有）⇒ 不编 HEAD 那一份 ✗
    # 有的 TESTS 项只是"派发用的场景组"，复用别的测试的二进制（hs-e2e ⇒ e2e ✓）
    BUILD_T=$t; case $t in hs-e2e|e2e-raw) BUILD_T=e2e ;; esac
    V2ONLY=""; case $BUILD_T in t_handshake|t_crypto|t_recvdata|t_keepalive|t_init|t_dump|t_fec) V2ONLY=1 ;; esac
    for v in base new; do
        if [ -n "$V2ONLY" ] && [ "$v" = base ]; then continue; fi
        sed "s|#include \"../../ffrdp.c\"|#include \"ffrdp_$v.c\"|; s|#include \"../../ffrdp.h\"|#include \"ffrdp_$v.h\"|" ${BUILD_T}.c > ${BUILD_T}_$v.c
        EXTRA=""; grep -q counter_ack_recv ffrdp_$v.c && EXTRA="$EXTRA -DNEW_COUNTERS"
        grep -q counter_recv_ooo ffrdp_$v.c && EXTRA="$EXTRA -DRECV_OOO"
        grep -q counter_recv_dup ffrdp_$v.c && EXTRA="$EXTRA -DDUP_REPORT"
          grep -q counter_to_hadack ffrdp_$v.c && EXTRA="$EXTRA -DHAVE_TO_DIAG"
        grep -q counter_ssth_floor ffrdp_$v.c && EXTRA="$EXTRA -DHAVE_SSTH_FLOOR"
        grep -q counter_conf_max ffrdp_$v.c && EXTRA="$EXTRA -DHAVE_CONF_HIST"
        grep -q counter_rto_spurious ffrdp_$v.c && EXTRA="$EXTRA -DSPUR"
        grep -q 'uint32_t rmss, smss, swnd, inflight' ffrdp_$v.c && EXTRA="$EXTRA -DHAVE_INFLIGHT"
        grep -q counter_ooo_d1_24 ffrdp_$v.c && EXTRA="$EXTRA -DOOO_HIST"
        grep -q counter_ooo_d65_128 ffrdp_$v.c && EXTRA="$EXTRA -DOOO_HIST2"
        grep -q counter_tailprobe ffrdp_$v.c && EXTRA="$EXTRA -DHAVE_TAILPROBE"
        grep -q counter_cap_rise ffrdp_$v.c && EXTRA="$EXTRA -DHAVE_CAPRISE"
        # ffrdp_send/recv gained a timeout argument at some point, and the tests are compiled against both
        # sides, so tell them which shape they are looking at (the tests' TEST_SEND/TEST_RECV wrappers
        # mean "do not wait" on both, which is what HEAD does).
        grep -q 'int timeout_ms)' ffrdp_$v.c && EXTRA="$EXTRA -DFFRDP_API_HAS_TIMEOUT"
        # ffrdp_init 的 (ip, port) 收敛成了单个 addr 字符串：两侧形状不同，测试用 TEST_INIT 适配。
        grep -q 'void\* ffrdp_init(char \*addr' ffrdp_$v.c && EXTRA="$EXTRA -DFFRDP_API_HAS_ADDR"
        # 结构拆分（P1）：连接级字段挂到 ctxt->conn 下之后，测试用 CONN() 适配 · 按形状探测 ✓
        grep -q 'FFRDPCONN' ffrdp_$v.c && EXTRA="$EXTRA -DCONN_SPLIT"
        # v2 接口形状（params + 回调 + 连接句柄）⇒ 测试改用 TEST_* 的 v2 分支 ✓
        grep -q 'ffrdp_init(void \*params' ffrdp_$v.c && EXTRA="$EXTRA -DFFRDP_API_V2"
        # A side that carries lowlevel (the working tree) is stepped by sim_ll_run() instead of
        # ffrdp_update(), and needs the fake lowlevel linked in; HEAD is still stepped by ffrdp_update().
        LL=""
        if grep -q ffrdp_udp_lowlevel_init ffrdp_$v.c; then
            EXTRA="$EXTRA -DFFRDP_LL_DRIVEN"
            # One fake per side: each is compiled against its own lowlevel.h, so the struct handed
            # back has exactly the slots that side's calls expect.
            if [ "$v" = base ]; then LL="simll_base.o utils.o"; else LL="simll.o utils.o"; fi
        fi
        # -lpthread: utils.o carries the address convention and also the module-thread helpers, both of
        # which live in the same file and the latter needs pthread at link time.
        eval gcc $CFLAGS $ASANFLAGS $EXTRA ${BUILD_T}_$v.c simnet.o $LL -lpthread -o ${BUILD_T}_$v
    done
done

for t in $TESTS; do
    case $t in
    t_rto)
        echo "===== t_rto: rto inflation ====="
        echo "-- HEAD --" | tee -a "$SIMLOG"; run $RUN ./t_rto_base
        echo "-- work --" | tee -a "$SIMLOG"; run $RUN ./t_rto_new ;;
    t_pseudo)
        # 伪超时：ACK 晚到但不丢帧 ⇒ 不该被当成拥塞（cwnd 不该回 1 ✗）
        echo
        echo "===== t_pseudo: 迟到的 ack 与拥塞判断 ====="
        echo "-- HEAD --" | tee -a "$SIMLOG"; run $RUN ./t_pseudo_base
        echo "-- work --" | tee -a "$SIMLOG"; run $RUN ./t_pseudo_new ;;
    t_cwnd)
        # 塌陷恢复代价：ssthresh 被"已经塌掉的 cwnd"重算，值不值（两侧并排看 ✓）
        echo
        echo "===== t_cwnd: 塌陷之后的恢复代价 ====="
        echo "-- HEAD --" | tee -a "$SIMLOG"; run $RUN ./t_cwnd_base
        echo "-- work --" | tee -a "$SIMLOG"; run $RUN ./t_cwnd_new ;;
    t_ack)
        echo
        echo "===== t_ack: ack diagnostics ====="
        echo "-- HEAD --" | tee -a "$SIMLOG"; run $RUN ./t_ack_base
        echo "-- work --" | tee -a "$SIMLOG"; run $RUN ./t_ack_new ;;
    t_fuzz)
        echo
        echo "===== t_fuzz: hostile fragments (a crash names the bytes)"
        echo "-- HEAD --" | tee -a "$SIMLOG"; run $RUN ./t_fuzz_base
        echo "-- work --" | tee -a "$SIMLOG"; run $RUN ./t_fuzz_new ;;
    t_peer)
        # 夹具自测：按对端地址分发是否成立。它不含 ffrdp.c（测的是 simnet + 假 lowlevel 这一层），
        # 所以只跑一份；用 "-- work --" 标记，让它计入判定汇总的 work 侧。
        echo
        echo "===== t_peer: multi-peer routing by addr (fake lowlevel) ====="
        echo "-- work --" | tee -a "$SIMLOG"; run $RUN ./t_peer_new ;;
    t_keepalive)
        # keepalive：空闲探活 + 判死（活的不误杀 ✓、哑的在 idle_ms 后断开 ✓）
        echo
        echo "===== t_keepalive: idle_ms 探活与判死（空闲×活着 / 空闲×对端哑掉）====="
        echo "-- work --" | tee -a "$SIMLOG"; run $RUN ./t_keepalive_new ;;
    t_fec)
        # FEC：冗余帧能不能把丢帧补回来（fec:3 能 ✓ / fec:4 计失败 ✓ / fec:2 现状 ✗）
        echo
        echo "===== t_fec: 冗余帧与丢帧恢复 ====="
        echo "-- work --" | tee -a "$SIMLOG"; run $RUN ./t_fec_new ;;
    t_init)
        # 参数解析：全部校验分支 + 键边界（纯负面用例 ✓）
        echo
        echo "===== t_init: ffrdp_init 的参数校验与键边界 ====="
        echo "-- work --" | tee -a "$SIMLOG"; run $RUN ./t_init_new ;;
    t_dump)
        # ffrdp_dump 的句柄/槽边界（含空 ctxt 与已放掉的槽 ✓）
        echo
        echo "===== t_dump: ffrdp_dump 的边界 ====="
        echo "-- work --" | tee -a "$SIMLOG"; run $RUN ./t_dump_new ;;
    t_recvdata)
        # RECVDATA：交付前异步通知；CONSUMED ⇒ 协议不留（但帧照常确认 ⇒ 传输不受影响 ✓）
        echo
        echo "===== t_recvdata: FFRDP_RECVDATA（NOT_CONSUMED / CONSUMED 两条路）====="
        echo "-- work --" | tee -a "$SIMLOG"; run $RUN ./t_recvdata_new ;;
    t_flush)
        # ffrdp_flush（v2）：只刷指定的那条连接（服务端用的是 1 号及以后的 accept 句柄 ✓）
        # + **一次性请求**（调过一次之后不该把"短帧立刻发"永久打开 ✗）
        # HEAD 侧固定刷 table[0]、且 FLAG_FLUSH 从不撤销 ⇒ 两段都该红 ✓（对照的意义）。
        echo
        echo "===== t_flush: 半帧冲刷（指定连接 / 一次性）====="
        echo "-- HEAD --" | tee -a "$SIMLOG"; run $RUN ./t_flush_base
        echo "-- work --" | tee -a "$SIMLOG"; run $RUN ./t_flush_new ;;
    t_probe)
        # A′：尾部探测 —— 健康链路零动作；静默（RTO 已退避）后提前 1/4 RTO 恢复
        echo
        echo "===== t_probe: 尾部探测（RTO ≥ 4×RTT 才出手）====="
        echo "-- HEAD --" | tee -a "$SIMLOG"; run $RUN ./t_probe_base
        echo "-- work --" | tee -a "$SIMLOG"; run $RUN ./t_probe_new ;;
    t_sweep)
        # A′ 的收益扫参：尾巴丢失后"放开链路 → 第一次收到数据"的步数，随 RTT / 静默时长。
        # ⚠️ 两侧要真不同：A′ 已进 HEAD ⇒ base 侧要用 REF=<加 A′ 之前的 ffrdp.c> 重编
        #    （REF=/tmp/x.c TESTS=t_sweep bash run.sh），否则 base 与 work 逐字相同 ——
        #    差 0 是"没有对照"，不是"没有收益"。这里只负责把两侧的数字并排打出来。
        echo
        echo "===== t_sweep: 尾巴丢失扫参（恢复步数，越低越好）====="
        for d in 2 4 10 40; do
            for s in 150 400 900; do
                printf "  HEAD " | tee -a "$SIMLOG"; run $RUN ./t_sweep_base $d $s
                printf "  work " | tee -a "$SIMLOG"; run $RUN ./t_sweep_new  $d $s
            done
        done ;;
    t_cap)
        # B2：闸门"能升" —— 干净高 RTT 上 cap 64→(>64)；有队列的链路不许升；升过之后探测还能砍回来 ✓
        echo
        echo "===== t_cap: 在途闸门「能升」（顶死 + 没队列 + 没浪费 / revert 后不升）====="
        echo "-- HEAD --" | tee -a "$SIMLOG"; run $RUN ./t_cap_base
        echo "-- work --" | tee -a "$SIMLOG"; run $RUN ./t_cap_new ;;
    t_crypto)
        # 加密回调：协议层把帧载荷交给应用（原地改、长度不变 ⇒ 对 send/recv 透明 ✓）
        # 两条出口（攒满 smss / 超时短帧）都必须挂上，否则一种帧明文出去 ✗。
        echo
        echo "===== t_crypto: 加密回调（两条出口 + 不强制加密的对照）====="
        echo "-- work --" | tee -a "$SIMLOG"; run $RUN ./t_crypto_new ;;
    t_handshake)
        # 握手 + 多连接：一个服务端 + 两个客户端，验 ACCEPT/CONNECT 与"不串台" ✓
        echo
        echo "===== t_handshake: 三次握手 + 多连接（两个对端不串台）====="
        echo "-- work --" | tee -a "$SIMLOG"; run $RUN ./t_handshake_new ;;
    hs-e2e)
        # 端到端**走握手**（v2 起就是默认路径 ✓，这里不再设 E2E_HS）。
        # 一小撮场景的快速组；完整的 17 个场景在 e2e 组（现在也是握手路径 ✓）。
        echo
        echo "===== hs-e2e: end to end over the handshake (hs:1) ====="
        for cfg in "0.0 0 4 0 4000 16384" "0.05 0 4 0 4000 16384" "0.0 0 40 0 4000 16384" "0.0 0 4 1 4000 16384 1200 200"; do
            echo "-- $cfg --"
            printf "  HEAD " | tee -a "$SIMLOG"; run $RUN ./e2e_base $cfg
            printf "  work " | tee -a "$SIMLOG"; run $RUN ./e2e_new  $cfg
        done ;;
    e2e-raw)
        # 老路径（hs:0 = 不下发握手，与 v1 线上行为一致 ✓）。这一组是"老路径一字未变"的
        # 回归网：它的输出应当和**翻默认之前**逐字相同 ✓（翻默认只该动握手那条路 ✓）。
        echo
        echo "===== e2e-raw: same scenarios, handshake switched OFF (hs:0) ====="
        for cfg in "0.0 0 4 0 4000 16384" "0.05 0 4 0 4000 16384" "0.0 0 40 0 4000 16384" "0.0 0 4 1 4000 16384 1200 200"; do
            echo "-- $cfg --"
            printf "  HEAD " | tee -a "$SIMLOG"; run env E2E_HS=0 $RUN ./e2e_base $cfg
            printf "  work " | tee -a "$SIMLOG"; run env E2E_HS=0 $RUN ./e2e_new  $cfg
        done ;;
    e2e)
        echo
        echo "===== e2e: loss burst delay jitter ====="
        # loss burst delay jitter steps maxchunk shaper_rate(KB/s) shaper_queue(KB)
        for cfg in "0.0 0 4 0 6000 16384" "0.0 0 10 0 6000 16384" "0.05 0 4 0 6000 16384" "0.02 6 4 0 6000 16384" "0.10 0 4 0 6000 16384" "0.0 0 4 1 6000 16384 1200 200" "0.0 0 4 1 6000 16384 1200 64" "0.0 0 4 1 6000 16384 600 200" "0.0 0 4 1 6000 2048 1200 200" "0.0 0 4 40 6000 16384" "0.0 0 4 60 6000 16384" "0.0 0 4 100 6000 16384" "0.0 0 4 1 6000 16384 1200 200 400 4" "0.0 0 4 0 6000 16384 0 0 0 0 15 300" "0.0 0 4 0 6000 16384 1200 200 0 0 15 300" "0.0 0 40 0 6000 16384 0 0 0 0 5 200" "0.0 0 30 0 6000 16384 0 0 0 0 3 150"; do
            echo "-- $cfg --"
            printf "  HEAD " | tee -a "$SIMLOG"; run $RUN ./e2e_base $cfg
            printf "  work " | tee -a "$SIMLOG"; run $RUN ./e2e_new  $cfg
        done ;;
    esac
done

# ---- 判定汇总：只看"判定"，不看统计 ----
# 侧别由日志里的 "-- HEAD --" / "-- work --" 决定；e2e 自己的 "-- <cfg> --" 不改变侧别。
# 判定行有两种形状：t_ack/t_fuzz 的 "... -> PASS"，e2e 的 "verdict -> OK/FAIL"。
awk '
  BEGIN { side = "(开头)" }
  /^-- HEAD --$/ { side = "HEAD"; next }
  /^-- work --$/ { side = "work"; next }
  /^-- /         { next }
  /^  HEAD /     { side = "HEAD" }   # e2e 的侧别是行首前缀，不是独立一行；不能 next（本行可能带判定）
  /^  work /     { side = "work" }
  /-> PASS/       { p[side]++; next }
  /verdict -> OK/ { p[side]++; next }
  /-> FAIL|FAIL:|verdict -> FAIL/ { f[side]++; bad[side] = bad[side] "\n        " $0 }
  END {
    printf "\n===== 判定汇总（只统计判定行；统计数字仅供参考，随负载与旋钮变化）=====\n";
    for (s in p) printf "  %-5s 判定通过 %d\n", s, p[s];
    for (s in f) printf "  %-5s 判定失败 %d%s\n", s, f[s], bad[s];
    wf = ("work" in f) ? f["work"] : 0;
    printf "  ⇒ work 侧: %s\n", (wf ? "有 FAIL（见上）" : "全通过");
    exit (wf ? 1 : 0);
  }' "$SIMLOG"
