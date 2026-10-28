#!/bin/bash
# 生成 configure / Makefile.in（本工程不生成 config.h，故不调 autoheader）

set -e
cd "$(dirname "$0")"

mkdir -p m4 build-aux
aclocal
libtoolize --automake --copy
# 部分 libtoolize 版本不认 AC_CONFIG_AUX_DIR([build-aux])，会把 ltmain.sh 丢在源码根，
# 而 automake 只去 build-aux 找 ⇒ 挪进去（build-aux 里已有就不动）。
[ -f ltmain.sh ] && mv -f ltmain.sh build-aux/
automake --foreign --add-missing --copy
autoconf
