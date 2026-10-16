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
    # Version 只有**一个来源**：ffrdp.h 里的 FFRDP_VERSION ✓。这里原来是写死的 "1.1.0" ——
    # 现在恰好与库（v1.1.0 ✓）一致，但它是**手写的第二份版本号** ✗ ⇒ 下次升级必然脱节 ✗。
    # 解析失败就报错退出，**绝不静默生成一个假版本** ✓。
    local ver
    ver=$(sed -n 's/^[[:space:]]*#[[:space:]]*define[[:space:]][[:space:]]*FFRDP_VERSION[[:space:]][[:space:]]*"\([^"]*\)".*/\1/p' "$TOPDIR/ffrdp.h" | head -n 1)
    ver=${ver#v}   # pkg-config 的 Version 走 RPM 式版本比较 ⇒ 去掉可选的 "v" 前缀：v1.1.0 ⇒ 1.1.0 ✓
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

    local CFLAGS="-Wall -Werror"
    local LDFLAGS="-lpthread"
    case "$CROSS_COMPILE" in
    *mingw*)
        LDFLAGS="$LDFLAGS -lws2_32 -lwinmm"
        ;;
    esac

    gen_libffrdp_pkgconfig_file
    ${CROSS_COMPILE}gcc -c $CFLAGS ffrdp.c
    ${CROSS_COMPILE}ar rcs libffrdp.a ffrdp.o
    ${CROSS_COMPILE}gcc $CFLAGS test.c libffrdp.a $LDFLAGS -o test_ffrdp
    mkdir -p $BUILD_PACKAGE_OUTDIR/$TOOLCHAIN_NAME/include
    mkdir -p $BUILD_PACKAGE_OUTDIR/$TOOLCHAIN_NAME/lib/pkgconfig
    cp $TOPDIR/ffrdp.h     $BUILD_PACKAGE_OUTDIR/$TOOLCHAIN_NAME/include/
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

