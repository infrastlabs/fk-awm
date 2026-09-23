#!/bin/sh
# ============================================================
# 03-container-build-static-libs.sh —— 段3：补静态库
#
# 在哪跑：容器内（编译全程在容器里，不是宿主机）
# 用法：
#   docker exec builder-awe sh /_ext/home/headless/xm-zs01/fk-awesomewm/docs/sam/scripts/03-container-build-static-libs.sh
#
# 干了什么：apk 给不了 .a 的三个库，自己编
#   1. libxcb-util.a     —— 复用 x11base 的 v-tint2/build.sh（容器内走 gitee，全程容器）
#   2. libxdg-basedir.a  —— 容器内 autogen + configure --disable-shared + make
#   3. libdbus-1.a       —— dbus 1.12.26 源码静态编译（路线 A），关 systemd/selinux/x11/glib
#   4. liblua.a          —— Lua 5.3.6 make generic（避开 readline），.pc 手写
#
# 产物：
#   /usr/local/lib/libxcb-util.a     + /usr/local/lib/pkgconfig/xcb-util.pc (0.4.0)
#   /usr/local/lib/libxdg-basedir.a  + /usr/local/lib/pkgconfig/libxdg-basedir.pc
#   /usr/local/lib/libdbus-1.a       + /usr/local/lib/pkgconfig/dbus-1.pc + include/dbus-1.0/
#   /usr/local/lib/liblua.a          + /usr/local/lib/pkgconfig/lua5.3.pc + include/lua*.h
#
# ⚠️ 前置（必须在宿主机做，容器内 github 被拦、ghfast.top 被限流）：
#   libxdg-basedir：
#     git clone --depth=1 https://ghfast.top/https://github.com/davmac314/libxdg-basedir /_ext/tmp/libxdg-basedir
#   dbus（注意：ghfast 不代理 dbus 仓库，会返回 401 要凭证；直接下官方 tarball）：
#     curl -sSLo /_ext/tmp/dbus-1.12.26.tar.gz https://dbus.freedesktop.org/releases/dbus/dbus-1.12.26.tar.gz
#   lua（Lua.org 官方站点，容器内可直连）：
#     curl -sSLo /_ext/tmp/lua-5.3.6.tar.gz https://www.lua.org/ftp/lua-5.3.6.tar.gz
#   容器检测到缺失时会直接 FATAL 并把命令打出来，不会瞎编。
#   容器内 github 被拦，ghfast.top 在容器内也被限流，所以 libxdg-basedir 的源码
#   必须在宿主机 clone 到共享卷：
#     git clone --depth=1 https://ghfast.top/https://github.com/davmac314/libxdg-basedir /_ext/tmp/libxdg-basedir
#   容器检测到目录缺失时会直接 FATAL 并把这条命令打给你，不会瞎编。
#
# 坑（实测）：
#   1. x11base 约定：组件脚本树软链到 /src 再调用
#   2. v-tint2/build.sh 的 xcbutil target 自带完整流程（gitee clone → 改 .gitmodules
#      走 gitee → submodule → autogen → make → make install），约 18 秒
#   3. xdg-basedir 上游是 davmac314 fork（Alpine aports 同款，1.2.2），导出驼峰 API：
#      xdgConfigHome / xdgDataHome / xdgDataFind / xdgCacheHome / xdgConfigOpen /
#      xdgDataOpen / xdgRuntimeDirectory / xdgMakePath / xdgInitHandle / xdgWipeHandle
#      头文件 basedir.h / basedir_fs.h，形如 const char * xdgConfigHome(xdgHandle *h)
#      【没有】xdg_base_dir_lookup_config_home 那套下划线 API（nicbarker 原始版才有）
#      ⚠️ 待确认：awesome 源码用哪套；若用下划线 API 则与本库不兼容
#   4. 产物装到 /usr/local 而非 /usr —— /usr/local 在 pkg-config 搜索路径里靠前，
#      源码 .pc 会优先于 apk 版，这正是我们要的
#   5. dbus 关 systemd 是静态化的关键：--disable-systemd 后 .la 的 dependency_libs 为空，
#      nm 里零 sd_* 符号，无需 libsystemd.a。参考本仓库已有先例
#      docker-x11base/docs/core/pulse07-xrdp.md:130 那串 --disable-systemd-* 参数
#   6. x11base 全树没有 dbus 构建脚本（唯一一处是 v-lxde/build.sh 的 --enable-dbus=yes，
#      那是消费方不是构建方）；仓库里 dbus 的历史做法是 --disable-dbus / -Ddbus=disabled
#   7. dbus 官方 tarball 包内已带 configure，免 autogen（libxdg-basedir 需要 autogen）
#   8. 验证静态可用性必须真编一遍：gcc -static + pkg-config --static --libs dbus-1，
#      看 ldd 是否报 "Not a valid dynamic program"。只看 .a 在不在不够
#   9. Lua 5.3.6 的 `make linux` 会去编 lua/luac 解释器，需要 readline-dev；容器里没装，
#      解释器会失败。但静态库目标 `make generic` 是纯 C 编译，不依赖 readline，且产物
#      只装 src/liblua.a。用 make generic 而不是 make linux
#  10. Lua 源码没有 configure，用 make。make generic 是跨平台最小 target（只编 core + auxlib
#      + 标准库）。装到 /usr/local/include/（不是 include/lua5.3/），因为源码不带版本子目录。
#      这与 apk 的 /usr/include/lua5.3/lua.h 不同路径 —— 静态编译时 -I/usr/local/include
#      优先，避免和 apk 的 .so 版混淆
#  11. Lua 源码不生成 .pc 文件（没有 Makefile install-pc 之类），需手写 lua5.3.pc；
#      Libs 里必须显式带 -lm，否则链接 libm 函数会缺符号
#  12. Lua 的 .a 名字是 liblua.a（不带版本后缀），而 apk 装的是 liblua-5.3.so.0；
#      awesomeConfig.cmake 里 -llua 就是找这个名字，正好对上
# ============================================================
set -eu

[ -f /etc/alpine-release ] || { echo "FATAL: 须在容器内执行（漏了 docker exec？）"; exit 2; }

SRC_TREE=${SRC_TREE:-/_ext/home/headless/xm-zs01/docker-x11base/compile/src}
XDG=${XDG:-/_ext/tmp/libxdg-basedir}
DBUS_TAR=${DBUS_TAR:-/_ext/tmp/dbus-1.12.26.tar.gz}
DBUS_SRC=${DBUS_SRC:-/_ext/tmp/dbus-1.12.26}
LUA_TAR=${LUA_TAR:-/_ext/tmp/lua-5.3.6.tar.gz}
LUA_SRC=${LUA_SRC:-/_ext/tmp/lua-5.3.6}

# --- 1. libxcb-util.a -----------------------------------------------------
echo "###### 1. libxcb-util.a ######"
if [ -f /usr/local/lib/libxcb-util.a ]; then
    echo "  [skip] 已在"
else
    [ -d "$SRC_TREE" ] || { echo "FATAL: 找不到 x11base 组件树 $SRC_TREE"; exit 1; }
    ln -sfn "$SRC_TREE" /src
    bash /src/v-tint2/build.sh xcbutil
    ls -l /usr/local/lib/libxcb-util.a
    printf '  pc version: %s\n' "$(pkg-config --modversion xcb-util)"
fi

# --- 2. libxdg-basedir.a --------------------------------------------------
echo "###### 2. libxdg-basedir.a ######"
if [ -f /usr/local/lib/libxdg-basedir.a ]; then
    echo "  [skip] 已在"
else
    if [ ! -d "$XDG/.git" ]; then
        echo "FATAL: 源码目录 $XDG 不存在"
        echo "       先在【宿主机】执行（容器内 github 被拦、ghfast 被限流）："
        echo "       git clone --depth=1 https://ghfast.top/https://github.com/davmac314/libxdg-basedir $XDG"
        exit 1
    fi
    cd "$XDG"
    bash autogen.sh
    ./configure --prefix=/usr/local --disable-shared --enable-static
    make -j
    make install
    ls -l /usr/local/lib/libxdg-basedir.a /usr/local/lib/pkgconfig/libxdg-basedir.pc
fi

# --- 3. libdbus-1.a（路线 A：源码静态编译）----------------------------------
echo "###### 3. libdbus-1.a（dbus 1.12.26 静态编译）######"
if [ -f /usr/local/lib/libdbus-1.a ]; then
    echo "  [skip] 已在"
else
    if [ ! -f "$DBUS_TAR" ]; then
        echo "FATAL: 找不到源码包 $DBUS_TAR"
        echo "       先在【宿主机】执行（ghfast 不代理 dbus 仓库，返回 401 要凭证）："
        echo "       curl -sSLo $DBUS_TAR https://dbus.freedesktop.org/releases/dbus/dbus-1.12.26.tar.gz"
        exit 1
    fi
    # 官方 tarball 包内已带 configure，免 autogen
    [ -d "$DBUS_SRC" ] || tar xzf "$DBUS_TAR" -C "$(dirname "$DBUS_TAR")"
    cd "$DBUS_SRC"
    # 关 systemd 是静态化的关键：之后 .la 的 dependency_libs 为空、nm 里零 sd_* 符号，
    # 无需 libsystemd.a
    ./configure --prefix=/usr/local --disable-shared --enable-static \
        --disable-systemd --disable-selinux --disable-elogind \
        --disable-x11 --disable-glib \
        --disable-tests --disable-docs --disable-man
    make -j
    make install
    ls -l /usr/local/lib/libdbus-1.a
    printf '  dependency_libs=%s\n' "$(sed -n 's/^dependency_libs=//p' dbus/libdbus-1.la)"
fi

# --- 4. liblua.a（Lua 5.3.6，避开 readline）---------------------------------
echo "###### 4. liblua.a（Lua 5.3.6 静态编译）######"
if [ -f /usr/local/lib/liblua.a ]; then
    echo "  [skip] 已在"
else
    if [ ! -f "$LUA_TAR" ]; then
        echo "FATAL: 找不到源码包 $LUA_TAR"
        echo "       先在【宿主机】执行（Lua.org 官方站点，容器内也可直连）："
        echo "       curl -sSLo $LUA_TAR https://www.lua.org/ftp/lua-5.3.6.tar.gz"
        exit 1
    fi
    [ -d "$LUA_SRC" ] || tar xzf "$LUA_TAR" -C "$(dirname "$LUA_TAR")"
    cd "$LUA_SRC"
    # make generic 而非 make linux：generic 只编 core 库，不编 lua/luac 解释器，
    # 因此不需要 readline-dev。-fPIC 是给将来嵌进 .so 预留（当前纯静态不需要，但无害）。
    # MYCFLAGS 里的 -DLUA_USE_POSIX 让 Lua 走 POSIX sockets/fcntl 分支（Alpine 默认）——
    # 必须放 MYCFLAGS（编译期）；放 MYLDFLAGS 不生效，liolib.c 编成无 POSIX 版本
    # （io.popen 报 'popen' not supported，record5 八·4.6）。
    # MYLIBS=-lm 是 libm，纯静态链接必需，不带会在链接时缺 sqrt/pow 符号。
    make generic MYCFLAGS="-fPIC -DLUA_USE_POSIX" MYLDFLAGS="" MYLIBS="-lm"
    make install generic
    ls -l /usr/local/lib/liblua.a
    # Lua 不生成 .pc，手写一个：Libs 显式带 -lm
    mkdir -p /usr/local/lib/pkgconfig
    cat > /usr/local/lib/pkgconfig/lua5.3.pc <<'PC'
prefix=/usr/local
exec_prefix=${prefix}
libdir=${exec_prefix}/lib
includedir=${prefix}/include

Name: Lua
Description: An Extensible Extension Language (static build)
Version: 5.3.6
Requires:
Libs: -L${libdir} -llua -lm
Cflags: -I${includedir}
PC
    printf '  pc version: %s\n' "$(pkg-config --modversion lua5.3)"
fi

echo
echo "###### 静态链接冒烟（证明 .a 真能用，不只是存在）######"
cat > /tmp/t_dbus.c <<'C'
#include <dbus/dbus.h>
int main(void){ DBusConnection *c = dbus_bus_get(DBUS_BUS_SESSION, NULL); (void)c; return 0; }
C
if gcc -static -o /tmp/t_dbus /tmp/t_dbus.c $(pkg-config --cflags --static --libs dbus-1) 2>/dev/null; then
    echo "  ok dbus 全静态编译通过"
    file /tmp/t_dbus
else
    echo "  x dbus 全静态编译失败（检查 libdbus-1.a 闭包）"
fi

cat > /tmp/t_lua.c <<'C'
#include <stdio.h>
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
int main(void){ lua_State *L = luaL_newstate(); luaL_openlibs(L); printf("%s\n", LUA_VERSION); lua_close(L); return 0; }
C
if gcc -static -o /tmp/t_lua /tmp/t_lua.c $(pkg-config --cflags --libs lua5.3) 2>/dev/null; then
    echo "  ok lua  全静态编译通过  ($(ldd /tmp/t_lua 2>&1 | head -1))"
else
    echo "  x lua  全静态编译失败（检查 liblua.a 闭包）"
fi

echo
echo "下一步： docker exec builder-awe sh /_ext/home/headless/xm-zs01/fk-awesomewm/docs/sam/scripts/04-container-finalize-and-verify.sh"
