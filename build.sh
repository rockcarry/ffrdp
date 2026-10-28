#!/bin/bash
# ffrdp 构建（autotools，与 ffp2p / winipc / lnxipc 的包构建习惯一致）
#
#   ./build.sh                    # = ./build.sh ship install
#   ./build.sh help               # 帮助
#   ./build.sh dev                # 调试档：-O2 -g + 仪表全开，只构建不安装
#   ./build.sh dev install        # 调试档 + 安装
#   ./build.sh ship               # 发布档：-O3 -DNDEBUG -DFFRDP_DIAG=0 + LTO
#   ./build.sh config [dev|ship]  # 强制重新 ./configure（例如从 dev 切 ship）
#   ./build.sh clean|distclean
#
# 平台由 --host 决定；source 对应项目的 envsetup.sh 后本脚本读这些变量：
#   TOOLCHAIN_NAME        工具链名（linux-x86_64 / i686-w64-mingw32-gcc-13.2.0）
#   BUILD_PACKAGE_OUTDIR  包产出根（winipc/lnxipc 里 = <proj>/bin-sdk）
#   BUILD_HOST            configure --host（i686-w64-mingw32 / x86_64-pc-linux-gnu）
#   CROSS_COMPILE         工具链前缀（只用于提示与 gcc-ar 探测）
#   CC / CXX              configure 直接使用
#   ARCH                  仅用于摘要打印
#   EXTRA_CFLAGS          追加编译选项
#   INSTALL_ROOT          安装根，给了就优先用它
#   PKG_CONFIG_PATH       依赖库搜索路径（libffp2p 是否使能由它决定）
#   FFP2P_CFLAGS/FFP2P_LIBS  交叉编译时直接指定 libffp2p（绕过 pkg-config）
#   JOBS                  make -j 并发数，缺省 = nproc
#
# 安装根取法（优先级从上到下）：
#   $INSTALL_ROOT
#   $BUILD_PACKAGE_OUTDIR/$TOOLCHAIN_NAME
#   $PWD/out/$TOOLCHAIN_NAME（没 source envsetup 时的本地默认）
#
# 安装布局：<root>/include/{ffrdp.h,lowlevel.h}、<root>/lib/libffrdp.a、
#           <root>/lib/pkgconfig/libffrdp.pc
#
# p2p 后端：configure 时自动检测 libffp2p.pc（或显式 FFP2P_LIBS），
#           有就编进 ffrdpsonffp2p:// / ffrdpconffp2p://，没有就只少了这两个 scheme。
set -e
cd "$(dirname "$0")"
TOP=$PWD

usage()
{
    sed -n '2,/^set -e$/p' "$TOP/build.sh" | sed '$d' | sed 's/^# \{0,1\}//'
}

MODE=ship
ACTION=install
FORCE_CONFIG=0

case "${1:-}" in
help | -h | --help)
    usage
    exit 0
    ;;
clean)
    [ -f Makefile ] && make clean
    exit 0
    ;;
distclean)
    if [ -f Makefile ]; then make distclean || true; fi
    rm -rf Makefile libffrdp.pc config.status config.log libtool aclocal.m4 \
           autom4te.cache build-aux configure depcomp install-sh missing \
           ltmain.sh compile config.guess config.sub test-driver
    rm -f m4/*.m4
    rm -rf "$TOP/.build-state"
    rm -f test_ffrdp test_ffrdp.exe
    exit 0
    ;;
config | reconfig)
    FORCE_CONFIG=1
    MODE=${2:-ship}
    ACTION=${3:-}
    ;;
dev | ship)
    MODE=$1
    ACTION=${2:-}
    ;;
install)
    ACTION=install
    ;;
"") ;;
*)
    usage >&2
    exit 2
    ;;
esac

# ---- 工具链 / 目标平台 -------------------------------------------------
TC=${TOOLCHAIN_NAME:-}
if [ -n "${BUILD_HOST:-}" ]; then
    HOST_ARG="--host=$BUILD_HOST"
elif [ -n "${CROSS_COMPILE:-}" ]; then
    echo "build: 设置了 CROSS_COMPILE=$CROSS_COMPILE 但没有 BUILD_HOST" >&2
    echo "       请先 source 对应项目的 envsetup.sh（它会导出 BUILD_HOST / TOOLCHAIN_NAME）" >&2
    exit 2
fi
HOST_ARG=${HOST_ARG:-}

# ---- 安装根 ------------------------------------------------------------
if [ -n "${INSTALL_ROOT:-}" ]; then
    ROOT=$INSTALL_ROOT
elif [ -n "${BUILD_PACKAGE_OUTDIR:-}" ] && [ -n "$TC" ]; then
    ROOT=$BUILD_PACKAGE_OUTDIR/$TC
else
    ROOT=$TOP/out/${TC:-linux-x86_64}
fi

# ---- 依赖库的 pkg-config 路径（决定 libffp2p 能否被检测到）-------------
add_pc_path()
{
    [ -d "$1" ] || return 0
    case ":${PKG_CONFIG_PATH:-}:" in
    *":$1:"*) ;;
    *) export PKG_CONFIG_PATH="${PKG_CONFIG_PATH:+$PKG_CONFIG_PATH:}$1" ;;
    esac
}

if [ -n "${BUILD_PACKAGE_OUTDIR:-}" ] && [ -n "$TC" ]; then
    add_pc_path "$BUILD_PACKAGE_OUTDIR/$TC/lib/pkgconfig"
fi
# 没 source envsetup 时的本地兜底
add_pc_path "$HOME/lnxipc/bin-sdk/linux-x86_64/lib/pkgconfig"
if [ -n "$TC" ]; then
    add_pc_path "$HOME/lnxipc/bin-sdk/$TC/lib/pkgconfig"
    add_pc_path "$HOME/winipc/bin-sdk/$TC/lib/pkgconfig"
fi

# ---- 编译档 ------------------------------------------------------------
# dev ：仪表全开（dump / ①-xx / check.sh 的读数都靠它），FEC 全功能
#       ⚠️ 仪表吃 CPU ⇒ dev 的吞吐/CPU 数字不能当性能结论
# ship：-DFFRDP_DIAG=0（dump 与"只给 dump 用"的计数器整段编掉）+ LTO
#       要报性能就用这一档
case "$MODE" in
dev)
    BASE_CFLAGS="-O2 -g -DFFRDP_DIAG=1 -DFFRDP_FEC_ENABLE=1"
    ;;
ship)
    BASE_CFLAGS="-O3 -DNDEBUG -DFFRDP_DIAG=0 -flto"
    # LTO 的 .o 里是 GIMPLE 字节码：要带 plugin 的 ar/ranlib 才认
    if command -v "${CROSS_COMPILE}gcc-ar" > /dev/null 2>&1; then
        export AR="${CROSS_COMPILE}gcc-ar"
        export RANLIB="${CROSS_COMPILE}gcc-ranlib"
    fi
    ;;
*)
    echo "unknown mode: $MODE" >&2
    usage >&2
    exit 2
    ;;
esac
export CFLAGS="${BASE_CFLAGS} ${EXTRA_CFLAGS:-}"
# 两档都保持警告当错（编译期的事，与优化级别无关）

echo "build: mode=$MODE host=${BUILD_HOST:-native} toolchain=${TC:-native} arch=${ARCH:-native}"
echo "  prefix: $ROOT"
echo "  cflags: $CFLAGS"
echo "  pkg-config: ${PKG_CONFIG_PATH:-<default>}"

# ---- configure ---------------------------------------------------------
# prefix / 模式 / host 变了就必须重配（configure 把 prefix 与 CFLAGS 固化进 Makefile，
# libffrdp.pc 的 prefix 也是 configure 时替换的）
STATE_FILE=$TOP/.build-state
WANT_STATE="$ROOT|$MODE|$HOST_ARG"
if [ "$FORCE_CONFIG" = 1 ] || [ ! -f Makefile ] || \
   [ -f "$STATE_FILE" -a "$(cat "$STATE_FILE")" != "$WANT_STATE" ]; then
    [ -f ./configure ] || ./autogen.sh
    ./configure --prefix="$ROOT" $HOST_ARG --enable-static --disable-shared
    echo "$WANT_STATE" > "$STATE_FILE"
fi

make -j"${JOBS:-$(nproc 2>/dev/null || echo 4)}"

case "$ACTION" in
"") ;;
install)
    make install
    echo "installed: $ROOT  (pkg-config: $ROOT/lib/pkgconfig/libffrdp.pc)"
    ;;
*)
    echo "usage: $0 [dev|ship] [install]" >&2
    exit 2
    ;;
esac
