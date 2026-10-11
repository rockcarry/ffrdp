#!/bin/bash
# Build and run the lowlevel backend conformance test (tools/test-lowlevel-udp.c).
#
#   bash tools/test-lowlevel-udp.sh            # plain build, -Wall -Werror like build.sh
#   ASAN=1 bash tools/test-lowlevel-udp.sh     # address sanitizer
#
# Unlike the simulator harness, this test does its own init/exit cycles, so leak detection is turned ON:
# the point of the lifecycle section is that 70 cycles leave no file descriptor and no thread behind, and
# leak checking is what makes that a checked claim instead of an eyeballed one.
#
# The test only talks to 127.0.0.1 and picks its ports from its own pid, so it needs no network and no
# configuration. It is serial: run one copy at a time.

set -e
cd "$(dirname "$0")/.."

BIN=out/test-lowlevel-udp
SRC="tools/test-lowlevel-udp.c lowlevel-udp.c socket.c utils.c"
CFLAGS="-O2 -Wall -Wextra -Werror -DLINUX -I."
LDFLAGS="-lpthread"
RUNNER=""

ASANFLAGS=""
if [ -n "$ASAN" ]; then
    ASANFLAGS="-O1 -g -fsanitize=address -fno-omit-frame-pointer"
    export ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}detect_leaks=1:halt_on_error=1"
    ulimit -c 0
    # Run the sanitized binary with a predictable address space layout. Measured on this WSL kernel: the
    # sanitizer fails to place its shadow memory at startup in about two runs out of ten, dies with
    # "AddressSanitizer:DEADLYSIGNAL" before main() is reached, and because the handler re-enters, the
    # whole failure is that one line repeated for ever -- a tenth of a second of one of those runs
    # produced 300KB of it. It is not this code: the same binary runs cleanly whenever that process gets a
    # layout it can use. setarch -R turns ASLR off for this process only, needs no privileges, and does
    # not weaken what the sanitizer is being used for here.
    if command -v setarch > /dev/null 2>&1; then
        RUNNER="setarch $(uname -m) -R"
    else
        echo "warning: setarch not found; under high entropy ASLR the sanitizer can fail to start at random"
    fi
fi

mkdir -p out
echo "cc: $SRC"
echo "  -> $BIN${ASAN:+ (asan)}"
gcc $CFLAGS $ASANFLAGS $SRC $LDFLAGS -o "$BIN"

set +e
timeout 180 $RUNNER "$BIN" "$@"
rc=$?
set -e
if [ $rc -eq 124 ]; then
    echo "FAIL: timed out after 180s -- a hang counts as a failure"
fi
exit $rc
