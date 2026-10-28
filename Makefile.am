AUTOMAKE_OPTIONS = foreign subdir-objects

# 源码都在顶层，头文件互相直接 #include "xxx.h"
AM_CPPFLAGS = -I$(top_srcdir)
AM_CFLAGS   = -Wall -Werror -Wextra -Wunused-macros

if WIN32
AM_CPPFLAGS += -DWIN32
AM_LDFLAGS   = -lws2_32
else
# socket.c / utils.c / lowlevel-udp.c 的 Linux 实现被 #ifdef LINUX 挡着（原来 build.sh 手加）；
# test.c 的崩溃处理器靠 -rdynamic 导出符号打栈。
AM_CPPFLAGS += -DLINUX
AM_LDFLAGS   = -rdynamic
endif

# ------------------------------------------------------------------
# 对外头文件与 pkg-config
# 布局与旧 build.sh 一致：<prefix>/include/ffrdp.h、<prefix>/lib/libffrdp.a
# ------------------------------------------------------------------
include_HEADERS = ffrdp.h lowlevel.h

pkgconfigdir   = $(libdir)/pkgconfig
pkgconfig_DATA = libffrdp.pc

# ------------------------------------------------------------------
# 库：协议层 + 传输层 + socket/utils（少一份就是运行期才发现的 undefined reference）
# 加密模块刻意不进库（库保持零依赖、也不知道密码学）
# ------------------------------------------------------------------
lib_LTLIBRARIES     = libffrdp.la
libffrdp_la_SOURCES = ffrdp.c lowlevel-udp.c socket.c utils.c
libffrdp_la_LDFLAGS = -version-info 0:0:0
libffrdp_la_LIBADD  = -lpthread

# ------------------------------------------------------------------
# p2p 后端（可选）：configure 找到 libffp2p.pc（或显式给了 FFP2P_LIBS）才编
# ------------------------------------------------------------------
if HAVE_FFP2P
libffrdp_la_CPPFLAGS = $(AM_CPPFLAGS) -DFFRDP_WITH_FFP2P=1 $(libffp2p_CFLAGS)
# 刻意**不**把 $(libffp2p_LIBS) 加进 LIBADD：静态归档不解析符号，加了只会让 libtool
# 把 libffp2p 的整条传递依赖（-lcjson -lssl -lfaac ...）写进 libffrdp.la 的
# dependency_libs，上层即便不用 p2p 也被迫去凑这些库。真正需要它的只有最终链接
# （test_ffrdp / 上层应用），由下面的 LDADD 与 .pc 的 Requires.private 负责。
else
libffrdp_la_CPPFLAGS = $(AM_CPPFLAGS)
endif

# ------------------------------------------------------------------
# 联调测试程序（不安装；in-tree 构建时就在源码目录，check.sh / tools 照旧能找到）
# ------------------------------------------------------------------
noinst_PROGRAMS    = test_ffrdp
test_ffrdp_SOURCES = test.c ffcrypto.c
test_ffrdp_LDADD   = libffrdp.la

if HAVE_FFP2P
test_ffrdp_CPPFLAGS = $(AM_CPPFLAGS) -DFFRDP_WITH_FFP2P=1 $(libffp2p_CFLAGS)
test_ffrdp_LDADD   += $(libffp2p_LIBS)
else
test_ffrdp_CPPFLAGS = $(AM_CPPFLAGS)
endif
