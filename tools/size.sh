#!/bin/bash
# tools/size.sh —— 编译期内存账（RAM ✓）
#
# 为什么要有它：本库的目标是"**内存由宏在编译期定下来**"（嵌入式 MCU ✓）⇒
# 每改一个宏、每做一次减法，都要能**立刻**看到 sizeof 与每端点的总账 ✓。
# 手工写探针既慢又容易忘（瘦身过程中我重建过三次 ✗）⇒ 落进仓库 ✓。
#
# 用法：
#   bash tools/size.sh            出货口径（DIAG=0 ✓）+ dev 口径（DIAG=1 ✓）对照
#   bash tools/size.sh --padded   额外把结构体里的**填充**（padding ✗ 白占）报出来 ✓
#
# 口径说明 ✓：出货 = 不传 -DFFRDP_DIAG（诊断整块不存在 ✓）；dev = -DFFRDP_DIAG=1（判据那套 ✓）。
cd "$(dirname "$0")/.." || exit 1
TMP=$(mktemp -d) || exit 1
trap 'rm -rf "$TMP"' EXIT

PADDED=""
[ "$1" = "--padded" ] && PADDED="-Wpadded"

cat > "$TMP/probe.c" <<'EOF'
#include <stdio.h>
#include <stddef.h>
#include "ffrdp.c"          /* 协议层：类型 + 全部编译期宏 ✓（含 FFRDP_MAXCONN / FFRDP_RECVBUF_SIZE） */
#include "lowlevel-udp.c"   /* 传输层：UDP_LOWLEVEL ✓（与其 link 两次，不如包含一次 ✓） */
int main(void)
{
    const size_t conn = sizeof(FFRDPCONN), ctx = sizeof(FFRDPCONTEXT);
    const size_t low  = sizeof(UDP_LOWLEVEL), node = sizeof(FFRDP_FRAME_NODE);
    const size_t fec  = sizeof(((FFRDPCONN *)0)->fec_txbuf) + sizeof(((FFRDPCONN *)0)->fec_rxbuf);
    printf("DIAG=%d\n", FFRDP_DIAG);
    printf("  sizeof(FFRDPCONN)            %6zu B   （每连接结构体；MAXCONN=%d ⇒%8zu B）\n",
           conn, FFRDP_MAXCONN, conn * FFRDP_MAXCONN);
    printf("    └ 其中 FEC 收发缓冲         %6zu B   （fec:0 时不用 ✗）\n", fec);
    printf("  sizeof(FFRDPCONTEXT)         %6zu B   （每端点一份）\n", ctx);
    printf("  sizeof(UDP_LOWLEVEL)         %6zu B   （每端点一份）\n", low);
    printf("  sizeof(FFRDP_FRAME_NODE)     %6zu B   （动态分配：按在途帧数 ✓）\n", node);
    printf("  FFRDP_RECVBUF_SIZE           %6d B   （= 96 帧 × MSS；每连接 malloc 一份 ✓）\n",
           (int)FFRDP_RECVBUF_SIZE);
    printf("  ── 每连接合计（结构体 + 接收窗）── %zu B ≈ %zu KB\n",
           conn + (size_t)FFRDP_RECVBUF_SIZE, (conn + (size_t)FFRDP_RECVBUF_SIZE) / 1024);
    printf("  ── 每端点合计（ctx + lowlevel）── %zu B ≈ %zu KB\n", ctx + low, (ctx + low) / 1024);
    return 0;
}
EOF

echo "── 编译期内存账（见 §26.82 ✓）───────────────────────────────────────────"
for d in 0 1; do
    if [ "$d" = 0 ]; then LBL="出货口径（DIAG=0 ✓ 诊断整块不存在）"; else LBL="dev 口径（DIAG=1 ✓ 判据那套）"; fi
    echo
    echo "=== $LBL ==="
    if gcc -w -DLINUX -DFFRDP_DIAG=$d -I. "$TMP/probe.c" utils.c socket.c -lpthread -o "$TMP/p$d" 2>"$TMP/e$d"; then
        "$TMP/p$d" | tail -n +2 | sed 's/^/  /'
    else
        echo "  ✗ 探针编译失败："; head -5 "$TMP/e$d" | sed 's/^/    /'
    fi
done

if [ -n "$PADDED" ]; then
    echo
    echo "=== 结构体填充（-Wpadded ✓：这些字节谁也没用到 ✗）==="
    for d in 0 1; do
        echo "  --- DIAG=$d ---"
        gcc -Wpadded -DLINUX -DFFRDP_DIAG=$d -I. -c "$TMP/probe.c" -o /dev/null 2>&1 \
            | grep -i 'padding\|padded' | sed 's/^/    /' | head -25
    done
fi
