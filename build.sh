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
    echo "prefix=$BUILD_PACKAGE_OUTDIR/$TOOLCHAIN_NAME"  > $TOPDIR/libffrdp.pc
    echo "exec_prefix=\${prefix}"                       >> $TOPDIR/libffrdp.pc
    echo "libdir=\${exec_prefix}/lib"                   >> $TOPDIR/libffrdp.pc
    echo "includedir=\${prefix}/include"                >> $TOPDIR/libffrdp.pc
    echo ""                                             >> $TOPDIR/libffrdp.pc
    echo "Name: libffrdp"                               >> $TOPDIR/libffrdp.pc
    echo "Description: ffrdp library"                   >> $TOPDIR/libffrdp.pc
    echo "Version: 1.1.0"                               >> $TOPDIR/libffrdp.pc
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

