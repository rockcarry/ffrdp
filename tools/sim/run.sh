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

vtick() { sed 's|.*ts.tv_nsec.*|    return sim_tick();|' ; }
if [ -n "$REF" ]; then vtick < "$REF" > ffrdp_base.c; else git -C ../.. show HEAD:ffrdp.c | vtick > ffrdp_base.c; fi
vtick < ../../ffrdp.c > ffrdp_new.c
grep -q sim_tick ffrdp_base.c || { echo "FATAL: failed to virtualise the clock"; exit 1; }
grep -q sim_tick ffrdp_new.c  || { echo "FATAL: failed to virtualise the clock"; exit 1; }

# The header is versioned too, and each side is pointed at its own copy. It only matters while an interface
# change is in flight -- ffrdp_send/recv gained a timeout argument, so HEAD's implementation and the working
# tree's no longer agree on what the header declares, and compiling both against one header is a
# conflicting-types error rather than a difference worth reporting.
git -C ../.. show HEAD:ffrdp.h > ffrdp_base.h
cp ../../ffrdp.h ffrdp_new.h
sed -i 's|#include "ffrdp.h"|#include "ffrdp_base.h"|' ffrdp_base.c
sed -i 's|#include "ffrdp.h"|#include "ffrdp_new.h"|'  ffrdp_new.c

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

CFLAGS="-O1 -w -I../.. -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -include simdefs.h \
 -D'calloc(n,s)=sim_calloc(n,s)' -D'free(p)=sim_free(p)' \
 -D'sendto(s,b,l,f,d,a)=sim_send(b,l)' -D'recvfrom(s,b,l,f,d,a)=sim_recv(b,l)' -D'select(n,r,w,e,t)=sim_select()' \
 -D'ffrdp_udp_lowlevel_init(p,cb,ctx)=sim_ll_init(p,cb,ctx)'"

gcc -O1 -w -c simnet.c -o simnet.o
# The working tree no longer owns a socket: ffrdp_init asks lowlevel for a transport, so the fake
# socket macros above only matter to the HEAD side. The new side is given a fake lowlevel instead --
# simll.c -- and the function-like macro is what redirects the one call ffrdp_init makes for it.
gcc -O1 -w -c simll.c  -o simll.o
TESTS="${TESTS:-t_rto t_ack t_fuzz e2e}"
for t in $TESTS; do
    for v in base new; do
        sed "s|#include \"../../ffrdp.c\"|#include \"ffrdp_$v.c\"|; s|#include \"../../ffrdp.h\"|#include \"ffrdp_$v.h\"|" $t.c > ${t}_$v.c
        EXTRA=""; grep -q counter_ack_recv ffrdp_$v.c && EXTRA="$EXTRA -DNEW_COUNTERS"
        grep -q counter_recv_ooo ffrdp_$v.c && EXTRA="$EXTRA -DRECV_OOO"
        grep -q counter_recv_dup ffrdp_$v.c && EXTRA="$EXTRA -DDUP_REPORT"
        grep -q counter_rto_spurious ffrdp_$v.c && EXTRA="$EXTRA -DSPUR"
        grep -q 'uint32_t rmss, smss, swnd, inflight' ffrdp_$v.c && EXTRA="$EXTRA -DHAVE_INFLIGHT"
        grep -q counter_ooo_d1_24 ffrdp_$v.c && EXTRA="$EXTRA -DOOO_HIST"
        grep -q counter_ooo_d65_128 ffrdp_$v.c && EXTRA="$EXTRA -DOOO_HIST2"
        # ffrdp_send/recv gained a timeout argument at some point, and the tests are compiled against both
        # sides, so tell them which shape they are looking at (the tests' TEST_SEND/TEST_RECV wrappers
        # mean "do not wait" on both, which is what HEAD does).
        grep -q 'int timeout_ms)' ffrdp_$v.c && EXTRA="$EXTRA -DFFRDP_API_HAS_TIMEOUT"
        # A side that carries lowlevel (the working tree) is stepped by sim_ll_run() instead of
        # ffrdp_update(), and needs the fake lowlevel linked in; HEAD is still stepped by ffrdp_update().
        LL=""
        if grep -q ffrdp_udp_lowlevel_init ffrdp_$v.c; then
            EXTRA="$EXTRA -DFFRDP_LL_DRIVEN"
            LL="simll.o"
        fi
        eval gcc $CFLAGS $ASANFLAGS $EXTRA ${t}_$v.c simnet.o $LL -o ${t}_$v
    done
done

for t in $TESTS; do
    case $t in
    t_rto)
        echo "===== t_rto: rto inflation ====="
        echo "-- HEAD --"; $RUN ./t_rto_base || true
        echo "-- work --"; $RUN ./t_rto_new  || true ;;
    t_ack)
        echo
        echo "===== t_ack: ack diagnostics ====="
        echo "-- HEAD --"; $RUN ./t_ack_base || true
        echo "-- work --"; $RUN ./t_ack_new  || true ;;
    t_fuzz)
        echo
        echo "===== t_fuzz: hostile fragments (a crash names the bytes)"
        echo "-- HEAD --"; $RUN ./t_fuzz_base || true
        echo "-- work --"; $RUN ./t_fuzz_new  || true ;;
    e2e)
        echo
        echo "===== e2e: loss burst delay jitter ====="
        # loss burst delay jitter steps maxchunk shaper_rate(KB/s) shaper_queue(KB)
        for cfg in "0.0 0 4 0 6000 16384" "0.0 0 10 0 6000 16384" "0.05 0 4 0 6000 16384" "0.02 6 4 0 6000 16384" "0.10 0 4 0 6000 16384" "0.0 0 4 1 6000 16384 1200 200" "0.0 0 4 1 6000 16384 1200 64" "0.0 0 4 1 6000 16384 600 200" "0.0 0 4 1 6000 2048 1200 200" "0.0 0 4 40 6000 16384" "0.0 0 4 60 6000 16384" "0.0 0 4 100 6000 16384" "0.0 0 4 1 6000 16384 1200 200 400 4" "0.0 0 4 0 6000 16384 0 0 0 0 15 300" "0.0 0 4 0 6000 16384 1200 200 0 0 15 300" "0.0 0 40 0 6000 16384 0 0 0 0 5 200" "0.0 0 30 0 6000 16384 0 0 0 0 3 150"; do
            echo "-- $cfg --"
            echo -n "  HEAD "; $RUN ./e2e_base $cfg
            echo -n "  work "; $RUN ./e2e_new  $cfg
        done ;;
    esac
done
