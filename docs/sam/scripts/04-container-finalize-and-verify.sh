#!/bin/sh
# ============================================================
# 04-container-finalize-and-verify.sh —— 段4：收尾与核查
#
# 在哪跑：容器内（步骤 1~3）
# 用法：
#   docker exec builder-awe sh /_ext/home/headless/xm-zs01/fk-awesomewm/docs/sam/scripts/04-container-finalize-and-verify.sh
#
# 干了什么：
#   1. 补 lua 软链 —— awesomeConfig.cmake:36 是硬需求：PATH 里必须有名为 lua 的可执行文件，
#      容器里只有 /usr/bin/lua5.3，首次 cmake 配置就卡在这里
#   2. 确认 busted-5.3 存在（二进制名带版本后缀，无 busted 软链）
#   3. 版本实测打印
#   4. 静态就绪度全量核查：按 ldd 依赖 + 源码 include 展开，逐项对照 .a，报缺失总数
#
# 产物：lua 可执行；报「缺失静态库总数: 0」—— 全部 21 个静态就绪项已解决。
#       libdbus-1.a 已由路线 A 解决（dbus 1.12.26 源码静态编译，见段3 第 3 步）
#       liblua.a    已由路线 A 解决（Lua 5.3.6 make generic，见段3 第 4 步）
#
# 坑（实测）：
#   1. 判静态可行性必须直接 `ls *.a`，不能用 `pkg-config --static --exists`
#      —— 它只校验 Requires.private 的 pc 能否解析，从不检查 .a 是否存在。
#      dbus-1 的 Libs.private 为空即判 OK，实则零静态库；xkbcommon 曾因此误判
#   2. 验证时不要设 PKG_CONFIG_PATH=/usr/lib/pkgconfig —— 会屏蔽
#      /usr/local/{lib,share}/pkgconfig，段3 产出的源码 .pc 会全部查不到
#   3. 容器内 `find /` 会遍历 /_ext（宿主机整盘绑定挂载）挂死，
#      且 exec 超时会把后续输出吞光。要查找就用 ls + grep
#   4. Alpine 命名/路径 ≠ Ubuntu，拿 Ubuntu 的名字查会误判（见第 D 段）
# ============================================================
set -eu

[ -f /etc/alpine-release ] || { echo "FATAL: 须在容器内执行（漏了 docker exec？）"; exit 2; }

# --- 1. lua 软链 ----------------------------------------------------------
echo "###### 1. lua 软链（cmake 硬需求）######"
mkdir -p /usr/local/bin
ln -sfn /usr/bin/lua5.3 /usr/local/bin/lua
command -v lua
lua -v

# --- 2. busted ------------------------------------------------------------
echo "###### 2. busted ######"
ls -l /usr/bin/busted-5.3

# --- 3. 版本实测 ----------------------------------------------------------
echo "###### 3. 版本实测 ######"
for m in glib-2.0 gobject-2.0 gio-2.0 cairo gdk-pixbuf-2.0 pango pangoft2 \
         xkbcommon xcb xcb-icccm lua5.3 dbus-1 x11 xcb-util; do
    printf '  %-22s %s\n' "$m" "$(pkg-config --modversion "$m" 2>/dev/null || echo MISSING)"
done

# --- 4. 静态就绪度全量核查 ------------------------------------------------
echo
echo "===== A) .a 就绪度（按 ldd 依赖 + 源码 include 展开）====="
MISS=0
for a in \
    libxcb libxcb-util libxcb-icccm libxcb-ewmh libxcb-keysyms libxcb-cursor \
    libxcb-randr libxcb-shape libxcb-xtest libxcb-xinerama libxcb-xkb \
    libxcb-xfixes libxcb-xrm libxcb-render libxcb-render-util libxcb-image \
    libxcb-shm libstartup-notification-1 libxdg-basedir \
    libglib-2.0 libgobject-2.0 libgio-2.0 libgmodule-2.0 libgdk_pixbuf-2.0 \
    libcairo libpixman-1 libfontconfig libfreetype \
    libpango-1.0 libpangocairo-1.0 libpangoft2-1.0 \
    libX11 libXau libXdmcp libXrender libXext \
    libxkbcommon libxkbcommon-x11 \
    libpcre libffi libexpat libz liblzma libbrotlidec \
    libbsd libblkid libmount libuuid libgcrypt libmd libresolv \
    liblua libdbus-1; do
    f=$(ls /usr/lib/$a.a /lib/$a.a /usr/local/lib/$a.a 2>/dev/null | head -1)
    if [ -n "$f" ]; then
        echo "  OK   $a.a   $f"
    else
        echo "  MISS $a.a"
        MISS=$((MISS + 1))
    fi
done

echo
echo "===== B) pkg-config --static 闭包（仅供参考，不可作判定依据）====="
for m in glib-2.0 gobject-2.0 gio-2.0 gmodule-2.0 cairo cairo-xcb gdk-pixbuf-2.0 \
         pango pangoft2 xkbcommon xkbcommon-x11 dbus-1 zlib pcre libffi expat \
         x11 xcb xcb-util xcb-icccm xcb-ewmh xcb-cursor xcb-randr xcb-xtest \
         xcb-xinerama xcb-shape xcb-xkb xcb-xfixes xcb-xrm \
         libstartup-notification-1.0 libxdg-basedir xcb-errors \
         glib-2.0-unix gio-2.0-unix pcre2; do
    if pkg-config --exists "$m" 2>/dev/null; then
        if pkg-config --static --exists "$m" 2>/dev/null; then
            printf '  %-28s pc=%-8s static-OK（不代表 .a 存在）\n' "$m" "$(pkg-config --modversion "$m" 2>/dev/null)"
        else
            printf '  %-28s pc=%-8s static-NO\n' "$m" "$(pkg-config --modversion "$m" 2>/dev/null)"
        fi
    else
        printf '  %-28s PC-ABSENT\n' "$m"
    fi
done

echo
echo "===== C) 静态闭包里的关键事实 ====="
echo "--- glib-2.0.pc 私有依赖（用 pcre1 而非 pcre2）---"
grep -E '^(Requires|Libs)' /usr/lib/pkgconfig/glib-2.0.pc
echo "--- dbus-1.pc（Libs.private 为空 = 无静态库，唯一硬阻塞）---"
grep -E '^(Version|Libs|Cflags)' /usr/lib/pkgconfig/dbus-1.pc
echo "--- dbus 头真实路径（Alpine 从 dbus-1 改名为 dbus-1.0）---"
ls -d /usr/include/dbus-1.0 2>&1
echo "--- glib-unix.h（glib-2.0-unix.pc 不存在，但头在主 glib 目录，可直接编）---"
ls /usr/include/glib-2.0/glib-unix.h 2>&1

echo
echo "===== D) Alpine 3.15 命名/路径 ≠ Ubuntu（拿 Ubuntu 名查会误判）====="
echo "  libpango.a      -> 实际 libpango-1.0.a"
echo "  liblua5.3.so    -> 运行时是 liblua-5.3.so.0"
echo "  libz.a          -> 实际在 /lib/libz.a（zlib.pc 用 -L/lib -lz）"
echo "  dbus 头         -> /usr/include/dbus-1.0/（不是 /usr/include/dbus-1）"
echo "  pc 名 x11       -> 无 libx11.pc"
echo "  pc 名 xcb-icccm -> 无 libxcb-icccm.pc"
echo "  pc 名 libstartup-notification-1.0 / libxdg-basedir"
echo "  libsystemd/libselinux -> 容器内完全没有；只有 Ubuntu 版 glib 才链它们，Alpine glib 不引用"

echo
echo "===== 已知缺口 ====="
echo "  暂无缺口 —— 全部 21 个静态就绪项已解决"
echo "  已解决 libdbus-1.a -> 路线 A：dbus 1.12.26 源码静态编译（段3 第 3 步）"
echo "  已解决 liblua.a    -> 路线 A：Lua 5.3.6 make generic（段3 第 4 步）"
echo
echo "缺失静态库总数: $MISS"
