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
#   EXTRA_CFLAGS='-DFFRDP_RX_WINDOW_FRAMES=64 ...' bash tools/size.sh
#                                 按**自定义宏**算账（嵌入式配方见 docs/ffrdp-v2.md §9.7 ✓）
#
# 口径说明 ✓：出货 = 不传 -DFFRDP_DIAG（诊断整块不存在 ✓）；dev = -DFFRDP_DIAG=1（判据那套 ✓）。
#
# ①-46 起的模型：两环**各自定容**（发送 FFRDP_SND_FRAME_RING / 接收 FFRDP_RCV_FRAME_RING）
#   · 常驻 = FFRDPCONTEXT（内含 0 号槽那条内嵌连接）+ UDP_LOWLEVEL，**不含任何收发缓冲** ✓
#   · 每条连接两块环（发送 / 接收），**第一帧真进出时才分配**（懒）⇒ 空闲连接零字节 ✓
#   · 每槽 = ring_block_size(载荷) = 节点头 + 4 + 载荷 + 2（FEC 尾），8 对齐 ✓
#   · 接收环按**收**方向上限（FFRDP_MAX_RX_MSS）建、发送环按该连接 smss 建 ⇒ 发送侧通常更省 ✓
#   · 两环的槽数**不再共用一个宏**：发送环受受理闸（MAX_WAITSND）约束、接收环受乱序窗口
#     （RECV_QUEUE_MAX / 通告窗）约束 ⇒ 各配各的，不必按大的那个付钱 ✓
#   · FEC 帧多一块临时落点（rx_scratch，一条连接最多一块接收槽）⇒ 只有 FEC 流量才付 ✓
cd "$(dirname "$0")/.." || exit 1
TMP=$(mktemp -d) || exit 1
trap 'rm -rf "$TMP"' EXIT

PADDED=""
[ "$1" = "--padded" ] && PADDED="-Wpadded"

cat > "$TMP/probe.c" <<'EOF'
#include <stdio.h>
#include <stddef.h>
#include "ffrdp.c"          /* 协议层：类型 + 全部编译期宏 + ring_block_size() ✓ */
#include "lowlevel-udp.c"   /* 传输层：UDP_LOWLEVEL ✓（与其 link 两次，不如包含一次 ✓） */
int main(void)
{
    const size_t conn = sizeof(FFRDPCONN), ctx = sizeof(FFRDPCONTEXT);
    const size_t low  = sizeof(UDP_LOWLEVEL), node = sizeof(FFRDP_FRAME_NODE);
    const size_t sbd  = (size_t)ring_block_size(FFRDP_MAX_MSS);      // 发送槽（最大 smss）
    const size_t rbd  = (size_t)ring_block_size(FFRDP_MAX_RX_MSS);   // 接收槽（收方向上限）
    const size_t sring = sbd * FFRDP_SND_FRAME_RING;
    const size_t rring = rbd * FFRDP_RCV_FRAME_RING;
    const size_t scratch = rbd;                                     // FEC 临时落点（最多一块）
    const size_t live  = sring + rring;                             // 双向都在跑（不含 FEC 临时块）
    const size_t full  = live + scratch;                            // 再加上 FEC 临时块
#if FFRDP_FEC_ENABLE
    const size_t fec  = sizeof(((FFRDPCONN *)0)->fec_txbuf) + sizeof(((FFRDPCONN *)0)->fec_rxbuf);
#else
    const size_t fec  = 0;
#endif
    printf("DIAG=%d FEC=%d\n", FFRDP_DIAG, FFRDP_FEC_ENABLE);
    printf("  环：发送 %d 槽 / 接收 %d 槽（各自独立 ✓），通告窗 %d 帧，RECV_QUEUE_MAX=%d 帧，受理闸 %d 帧\n",
           FFRDP_SND_FRAME_RING, FFRDP_RCV_FRAME_RING, FFRDP_RX_WINDOW_FRAMES,
           FFRDP_RECV_QUEUE_MAX, FFRDP_MAX_WAITSND);
    printf("  sizeof(FFRDP_FRAME_NODE)     %6zu B   每槽节点头\n", node);
    printf("  发送槽 / 接收槽              %6zu B / %6zu B   （= 头 %zu + 4 + 载荷 %d/%d + 2，8 对齐）\n",
           sbd, rbd, node, FFRDP_MAX_MSS, FFRDP_MAX_RX_MSS);
    printf("  发送环 / 接收环              %6zu KB / %6zu KB  （%d / %d 槽，**懒分配：该方向第一帧时才建** ✓）\n",
           sring / 1024, rring / 1024, FFRDP_SND_FRAME_RING, FFRDP_RCV_FRAME_RING);
    printf("  FEC 临时落点(rx_scratch)     %6zu KB  （只有 FEC 帧才分配 ✓；非 FEC 流量 = 0 ✓）\n", scratch / 1024);
    printf("  sizeof(FFRDPCONN)            %6zu B   （每条连接的指针状态；0 号槽那份内嵌在 CONTEXT 里 ✓）\n", conn);
    printf("    └ 其中 FEC 收发缓冲         %6zu B   （FFRDP_FEC_ENABLE=0 时不存在 ✓；=1 而 fec:0 时白占 ✗）\n", fec);
    printf("  sizeof(FFRDPCONTEXT)         %6zu B   （每端点一份；含 table[%d] + gen[] + 内嵌 0 号连接 ✓）\n",
           ctx, FFRDP_MAXCONN);
    printf("  sizeof(UDP_LOWLEVEL)         %6zu B   （每端点一份）\n", low);
    printf("  ── 每端点常驻（ctx + lowlevel，**没有任何缓冲**）── %zu B ≈ %zu KB\n",
           ctx + low, (ctx + low) / 1024);
    printf("  ── 每连接 · 双向都在跑 ── %zu KB（= 2 环；发送环按最大 smss 算 ⇒ 实际上限 ✓）\n", live / 1024);
    printf("  ── 每连接 · 双向 + FEC 帧 ── %zu KB（= 2 环 + 1 块临时落点 ✓）\n", full / 1024);
    printf("  ── 每连接 · 只用了一个方向 ── %zu / %zu KB（收 / 发 ✓ 各一块）\n", rring / 1024, sring / 1024);
    printf("  ── 示例：服务端 1 端点 + 8 条连接全双工 ── %zu KB\n",
           (ctx + low + 8 * live) / 1024);
    return 0;
}
EOF

echo "── 编译期内存账（①-46 模型：两环各自定容，常驻无缓冲 ✓）──────────────────"
# 三个口径：出货（诊断 + FEC 都编译掉 ✓ = 最小内存 ✓）、出货+FEC、dev（判据那套 ✓）
for cfg in "0 0 出货口径（DIAG=0 + FEC=0 ✓ 两样都编译掉 = 最小内存 ✓）" \
           "0 1 出货 + FEC（DIAG=0 + FEC=1 ✓ 只有 FEC 的 3 KB ✓）" \
           "1 1 dev 口径（DIAG=1 + FEC=1 ✓ 判据/dev 那套 ✓）"; do
    set -- $cfg; d=$1; f=$2; shift 2; LBL="$*"
    echo
    echo "=== $LBL ==="
    if gcc -w -DLINUX -DFFRDP_DIAG=$d -DFFRDP_FEC_ENABLE=$f ${EXTRA_CFLAGS:-} -I. "$TMP/probe.c" utils.c socket.c \
            -lpthread -o "$TMP/p_${d}${f}" 2>"$TMP/e_${d}${f}"; then
        "$TMP/p_${d}${f}" | tail -n +2 | sed 's/^/  /'
    else
        echo "  ✗ 探针编译失败："; head -5 "$TMP/e_${d}${f}" | sed 's/^/    /'
    fi
done

if [ -n "$PADDED" ]; then
    echo
    echo "=== 结构体填充（-Wpadded ✓：这些字节谁也没用到 ✗）==="
    for d in 0 1; do
        echo "  --- DIAG=$d ---"
        gcc -Wpadded -DLINUX -DFFRDP_DIAG=$d ${EXTRA_CFLAGS:-} -I. -c "$TMP/probe.c" -o /dev/null 2>&1 \
            | grep -i 'padding\|padded' | sed 's/^/    /' | head -25
    done
fi
