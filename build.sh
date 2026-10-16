#!/bin/bash

# environment needs:
# TOOLCHAIN_NAME       - indicate toolchain name
# BUILD_PACKAGE_OUTDIR - indicate package binary output dir, if empty $PWD/out will be used

set -e

TOPDIR=$PWD

if [ "$BUILD_PACKAGE_OUTDIR"x = ""x ]; then
    BUILD_PACKAGE_OUTDIR=$PWD/out
fi

gen_libffrdp_pkgconfig_file()
{
    # Version 只有**一个来源**：ffrdp.h 里的 FFRDP_VERSION ✓。这里以前是写死的 "1.1.0" ✗ ——
    # 库升到 v1.2.0 之后生成的 .pc 还在报 1.1.0 ✗（pkg-config 查出来的版本与库不符 ✗）。
    # 解析失败就报错退出，**绝不静默生成一个假版本** ✓。
    local ver
    ver=$(sed -n 's/^[[:space:]]*#[[:space:]]*define[[:space:]][[:space:]]*FFRDP_VERSION[[:space:]][[:space:]]*"\([^"]*\)".*/\1/p' "$TOPDIR/ffrdp.h" | head -n 1)
    ver=${ver#v}   # pkg-config 的 Version 走 RPM 式版本比较 ⇒ 去掉可选的 "v" 前缀：v1.2.0 ⇒ 1.2.0 ✓
    if [ -z "$ver" ]; then
        echo -e "\n\033[31mcannot parse FFRDP_VERSION from $TOPDIR/ffrdp.h (want: #define FFRDP_VERSION \"vX.Y.Z\")\033[0m\n" >&2
        exit -1
    fi

    echo "prefix=$BUILD_PACKAGE_OUTDIR/$TOOLCHAIN_NAME"  > $TOPDIR/libffrdp.pc
    echo "exec_prefix=\${prefix}"                       >> $TOPDIR/libffrdp.pc
    echo "libdir=\${exec_prefix}/lib"                   >> $TOPDIR/libffrdp.pc
    echo "includedir=\${prefix}/include"                >> $TOPDIR/libffrdp.pc
    echo ""                                             >> $TOPDIR/libffrdp.pc
    echo "Name: libffrdp"                               >> $TOPDIR/libffrdp.pc
    echo "Description: ffrdp library"                   >> $TOPDIR/libffrdp.pc
    echo "Version: $ver"                                >> $TOPDIR/libffrdp.pc
    echo "Libs: -L\${libdir} -lffrdp"                   >> $TOPDIR/libffrdp.pc
    echo "Cflags: -I\${includedir}"                     >> $TOPDIR/libffrdp.pc
}

build_package()
{
    if [ "$TOOLCHAIN_NAME"x = ""x ]; then
        echo -e "\n\033[31menv TOOLCHAIN_NAME not set, please check your toolchain is correctly installed !\033[0m\n"
        exit -1
    fi

    # -DFFRDP_DIAG=1：dev/判据构建要完整仪表（dump + 只给 dump 用的计数器）；
    # -DFFRDP_FEC_ENABLE=1：dev/判据构建要 FEC 全功能。
    # 出货**两个都不传** ⇒ dump/计数器/FEC 三块一起编译掉，且门槛仍是 -Wall -Werror -Wextra -Wunused-macros。
    local CFLAGS="-Wall -Werror -Wextra -Wunused-macros -DFFRDP_DIAG=1 -DFFRDP_FEC_ENABLE=1"
    local LDFLAGS="-lpthread"
    case "$CROSS_COMPILE" in
    *mingw*)
        LDFLAGS="$LDFLAGS -lws2_32"
        ;;
    *)
        # socket.c / utils.c / lowlevel-udp.c 的 Linux 实现被 #ifdef LINUX 挡着：不给这个宏，它们编出来
        # 是空函数（或者干脆编不过），而这三个文件是传输层本身。mingw 那一支不需要它。
        CFLAGS="$CFLAGS -DLINUX"
        ;;
    esac

    gen_libffrdp_pkgconfig_file
    # 协议层依赖传输层：ffrdp.c 现在只认 lowlevel 的接口（send / timer / 回调），实现是 lowlevel-udp.c，
    # 它自己又用 socket.c 与 utils.c（工作线程）。所以这几份一起进库 —— 少一份就是运行期才发现的
    # undefined reference。
    ${CROSS_COMPILE}gcc -c $CFLAGS ffrdp.c
    ${CROSS_COMPILE}gcc -c $CFLAGS lowlevel-udp.c
    ${CROSS_COMPILE}gcc -c $CFLAGS socket.c
    ${CROSS_COMPILE}gcc -c $CFLAGS utils.c
    ${CROSS_COMPILE}ar rcs libffrdp.a ffrdp.o lowlevel-udp.o socket.o utils.o
    ${CROSS_COMPILE}gcc $CFLAGS test.c libffrdp.a $LDFLAGS -o test_ffrdp
    mkdir -p $BUILD_PACKAGE_OUTDIR/$TOOLCHAIN_NAME/include
    mkdir -p $BUILD_PACKAGE_OUTDIR/$TOOLCHAIN_NAME/lib/pkgconfig
    cp $TOPDIR/ffrdp.h     $BUILD_PACKAGE_OUTDIR/$TOOLCHAIN_NAME/include/
    cp $TOPDIR/lowlevel.h  $BUILD_PACKAGE_OUTDIR/$TOOLCHAIN_NAME/include/
    mv $TOPDIR/libffrdp.a  $BUILD_PACKAGE_OUTDIR/$TOOLCHAIN_NAME/lib/
    mv $TOPDIR/libffrdp.pc $BUILD_PACKAGE_OUTDIR/$TOOLCHAIN_NAME/lib/pkgconfig
}

case "$1" in
"")
    build_package
    ;;
clean|distclean)
    rm -rf *.o *.a *.exe *.pc test_ffrdp out/
    ;;
esac

