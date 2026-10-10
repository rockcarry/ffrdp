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

CFLAGS="-O1 -w -I../.. -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -include simdefs.h \
 -D'calloc(n,s)=sim_calloc(n,s)' -D'free(p)=sim_free(p)' \
 -D'sendto(s,b,l,f,d,a)=sim_send(b,l)' -D'recvfrom(s,b,l,f,d,a)=sim_recv(b,l)' -D'select(n,r,w,e,t)=sim_select()'"

gcc -O1 -w -c simnet.c -o simnet.o
TESTS="${TESTS:-t_rto t_ack e2e}"
for t in $TESTS; do
    for v in base new; do
        sed "s|#include \"../../ffrdp.c\"|#include \"ffrdp_$v.c\"|" $t.c > ${t}_$v.c
        EXTRA=""; grep -q counter_ack_recv ffrdp_$v.c && EXTRA="$EXTRA -DNEW_COUNTERS"
        grep -q counter_recv_ooo ffrdp_$v.c && EXTRA="$EXTRA -DRECV_OOO"
        grep -q counter_recv_dup ffrdp_$v.c && EXTRA="$EXTRA -DDUP_REPORT"
        grep -q counter_rto_spurious ffrdp_$v.c && EXTRA="$EXTRA -DSPUR"
        grep -q 'uint32_t rmss, smss, swnd, inflight' ffrdp_$v.c && EXTRA="$EXTRA -DHAVE_INFLIGHT"
        grep -q counter_ooo_d1_24 ffrdp_$v.c && EXTRA="$EXTRA -DOOO_HIST"
        grep -q counter_ooo_d65_128 ffrdp_$v.c && EXTRA="$EXTRA -DOOO_HIST2"
        eval gcc $CFLAGS $EXTRA ${t}_$v.c simnet.o -o ${t}_$v
    done
done

for t in $TESTS; do
    case $t in
    t_rto)
        echo "===== t_rto: rto inflation ====="
        echo "-- HEAD --"; ./t_rto_base || true
        echo "-- work --"; ./t_rto_new  || true ;;
    t_ack)
        echo
        echo "===== t_ack: ack diagnostics ====="
        echo "-- HEAD --"; ./t_ack_base || true
        echo "-- work --"; ./t_ack_new  || true ;;
    e2e)
        echo
        echo "===== e2e: loss burst delay jitter ====="
        # loss burst delay jitter steps maxchunk shaper_rate(KB/s) shaper_queue(KB)
        for cfg in "0.0 0 4 0 6000 16384" "0.0 0 10 0 6000 16384" "0.05 0 4 0 6000 16384" "0.02 6 4 0 6000 16384" "0.10 0 4 0 6000 16384" "0.0 0 4 1 6000 16384 1200 200" "0.0 0 4 1 6000 16384 1200 64" "0.0 0 4 1 6000 16384 600 200" "0.0 0 4 1 6000 2048 1200 200" "0.0 0 4 40 6000 16384" "0.0 0 4 60 6000 16384" "0.0 0 4 100 6000 16384" "0.0 0 4 1 6000 16384 1200 200 400 4" "0.0 0 4 0 6000 16384 0 0 0 0 15 300" "0.0 0 4 0 6000 16384 1200 200 0 0 15 300" "0.0 0 40 0 6000 16384 0 0 0 0 5 200" "0.0 0 30 0 6000 16384 0 0 0 0 3 150"; do
            echo "-- $cfg --"
            echo -n "  HEAD "; ./e2e_base $cfg
            echo -n "  work "; ./e2e_new  $cfg
        done ;;
    esac
done
