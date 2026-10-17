#!/bin/bash

# environment needs:
# TOOLCHAIN_NAME       - indicate toolchain name
# BUILD_PACKAGE_OUTDIR - indicate package binary output dir, if empty $PWD/out will be used

set -e

TOPDIR=$PWD

if [ "$BUILD_PACKAGE_OUTDIR"x = ""x ]; then
    BUILD_PACKAGE_OUTDIR=$PWD/out
fi

build_package()
{
    if [ "$TOOLCHAIN_NAME"x = ""x ]; then
        echo -e "\n\033[31menv TOOLCHAIN_NAME not set, please check your toolchain is correctly installed !\033[0m\n"
        exit -1
    fi

    local CFLAGS="-Wall -Werror -O2"
    local LDFLAGS="-lpthread"
    case "$CROSS_COMPILE" in
    *mingw*)
        LDFLAGS="$LDFLAGS -lws2_32 -lwinmm"
        ;;
    esac

    ${CROSS_COMPILE}gcc -c $CFLAGS ffrdp.c
    ${CROSS_COMPILE}ar rcs libffrdp.a ffrdp.o
    ${CROSS_COMPILE}gcc $CFLAGS test.c libffrdp.a $LDFLAGS -o test
}

case "$1" in
"")
    build_package
    ;;
clean|distclean)
    rm -rf *.o *.a *.exe 
    ;;
esac
