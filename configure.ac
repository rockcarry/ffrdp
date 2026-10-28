#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.69])

# 版本只有**一个来源**：ffrdp.h 里的 FFRDP_VERSION（原来 build.sh 也靠解析它生成 .pc）。
# 这里在 autoconf 阶段取一次；改了版本要重跑 ./autogen.sh（与 ffp2p 的写死做法相比，
# 好处是不会出现"库 vX 而 .pc 报 vY"）。
m4_define([ffrdp_version],
          m4_esyscmd_s([sed -n 's/^[[:space:]]*#[[:space:]]*define[[:space:]][[:space:]]*FFRDP_VERSION[[:space:]][[:space:]]*"\([^"]*\)".*/\1/p' ffrdp.h | head -n 1 | tr -d '\r\n' | sed 's/^v//']))
m4_if(ffrdp_version, [], [m4_define([ffrdp_version], [0.0.0])])

AC_INIT([ffrdp], [ffrdp_version], [chenk@apical.com.cn])
AC_CONFIG_SRCDIR([ffrdp.c])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_MACRO_DIRS([m4])
AC_CONFIG_FILES([Makefile libffrdp.pc])

AM_INIT_AUTOMAKE([foreign subdir-objects -Wall])
m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])

# ------------------------------------------------------------------
# 程序与工具链
# ------------------------------------------------------------------
AC_PROG_CC
AM_PROG_AR
LT_INIT
PKG_PROG_PKG_CONFIG

# ------------------------------------------------------------------
# 目标平台（由 --host 决定；交叉编译时 build.sh 从 envsetup.sh 取 BUILD_HOST）
# socket.c / utils.c / lowlevel-udp.c 的 Linux 实现被 #ifdef LINUX 挡着，
# mingw 那一支不需要它 —— 这里统一定义，省掉手写 -DLINUX。
# ------------------------------------------------------------------
AC_CANONICAL_HOST
case $host_os in
    *mingw* | *cygwin* | *msys*) win32="yes" ;;
    *)                           linux="yes" ;;
esac
AM_CONDITIONAL([WIN32], [test "x$win32" = "xyes"])
AM_CONDITIONAL([LINUX], [test "x$linux" = "xyes"])

AC_SEARCH_LIBS([pthread_create], [pthread], [],
    [AC_MSG_WARN([pthread not found: ffrdp 的工作线程需要线程支持])])

# ------------------------------------------------------------------
# 可选后端：ffp2p 打洞（ffrdpsonffp2p:// / ffrdpconffp2p://）
# 有 libffp2p.pc 就编进去，没有就只是不支持这两个 scheme —— UDP 路径零影响。
# 交叉编译时 pkg-config 常常只认宿主库 ⇒ 直接给 FFP2P_CFLAGS / FFP2P_LIBS
# 也能使能（见 build.sh 的说明）。
# ------------------------------------------------------------------
have_ffp2p="no"
if test -n "${FFP2P_LIBS:-}"; then
    # 显式给库：不依赖 pkg-config（交叉编译常用）
    libffp2p_CFLAGS="${FFP2P_CFLAGS:-}"
    libffp2p_LIBS="$FFP2P_LIBS"
    have_ffp2p="yes"
    AC_MSG_NOTICE([p2p backend: 使用显式给的 FFP2P_LIBS])
else
    PKG_CHECK_MODULES([libffp2p], [libffp2p], [have_ffp2p="yes"],
        [AC_MSG_NOTICE([libffp2p not found (pkg-config): 关闭 p2p 后端，*onffp2p:// 不可用])])
fi
AM_CONDITIONAL([HAVE_FFP2P], [test "x$have_ffp2p" = "xyes"])

# libffrdp.pc：静态链接上层还要补的依赖
if test "x$win32" = "xyes"; then
    FFRDP_PC_LIBS_PRIVATE="-lpthread -lws2_32"
else
    FFRDP_PC_LIBS_PRIVATE="-lpthread"
fi
if test "x$have_ffp2p" = "xyes"; then
    FFRDP_PC_REQUIRES="libffp2p"
else
    FFRDP_PC_REQUIRES=""
fi
AC_SUBST([FFRDP_PC_LIBS_PRIVATE])
AC_SUBST([FFRDP_PC_REQUIRES])

AC_MSG_NOTICE([---------------------------------------------------])
AC_MSG_NOTICE([  ffrdp $PACKAGE_VERSION on $host ($host_os)])
AC_MSG_NOTICE([  p2p backend (ffp2p)  : $have_ffp2p])
AC_MSG_NOTICE([  test_ffrdp           : always])
AC_MSG_NOTICE([---------------------------------------------------])

AC_OUTPUT
