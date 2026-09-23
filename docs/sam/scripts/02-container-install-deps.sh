#!/bin/sh
# ============================================================
# 02-container-install-deps.sh —— 段2：装依赖
#
# 在哪跑：容器内
# 用法：
#   docker exec builder-awe sh /_ext/home/headless/xm-zs01/fk-awesomewm/docs/sam/scripts/02-container-install-deps.sh
#   docker exec builder-awe sh /_ext/home/headless/xm-zs01/fk-awesomewm/docs/sam/scripts/02-container-install-deps.sh --force   # 忽略幂等检查重装
#
# 干了什么：
#   1. 写 /etc/apk/repositories → mirrors.tencent.com 的 main + community
#   2. apk update
#   3. apk add --no-cache 全部依赖（分四组：Lua 链 / xcb 链 / 桌面库 / 静态库）
#   4. 校验 22 项 pkg-config（即 awesomeConfig.cmake 的 AWESOME_DEPENDENCIES）
#   5. 校验 18 个 .a 静态库存在性
#   6. 编译冒烟：xcb-icccm + xcb-ewmh 头文件能否编过
#
# 产物：22 项 pc 全绿、.a 齐、冒烟通过，打印 ALL GREEN
#
# 坑（实测）：
#   1. apk update 不要用 --quiet —— apk-tools 2.12.7 下无输出、不返回，直接挂死
#   2. apk 输出不要走管道 —— apk 用 \r 刷进度，`| head`/`| tail` 会把输出吃成空；
#      要留日志就重定向到文件，再 `tr '\r' '\n'`
#   3. 脚本内不要 `cmd &` 后台化 —— 继承的 FD 不关闭，会把 docker exec 拖住不返回
#   4. 镜像源选 mirrors.tencent.com —— 容器内实测 2.50MB/s，最快可用
#      不可用：ustc(302 跳转) / 163,sohu,hust(404 无 v3.15) / zju,sjtu,tencentyun(FAIL)
#      其余测速：aliyun 2.55 / cloud.tencent 2.06 / huaweicloud 1.41 / nju 1.34
#                tsinghua 0.46 / dl-cdn.alpinelinux.org 0.05
#   5. egress 会间歇性限流（曾降到 ~1KB/s），卡住就等一阵重试
#   6. Alpine 3.15 没有 lua5.3-static 包；lua5.3-dev 只带 lua5.3.pc + 头，不带 .a
#   7. xcb-util-wm-dev 一个包就带 xcb-icccm.pc / xcb-ewmh.pc + 对应 .a
#   8. 判静态可行性必须直接 `ls *.a`，不能用 `pkg-config --static --exists`
# ============================================================
set -eu

[ -f /etc/alpine-release ] || { echo "FATAL: 须在容器内执行（漏了 docker exec？）"; exit 2; }

FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

have() { [ -f "/usr/lib/$1" ] || [ -f "/lib/$1" ]; }

# --- 1~2. apk 源 + update + add -------------------------------------------
if [ "$FORCE" -eq 0 ] && have libglib-2.0.a; then
    echo "[skip] 依赖已装（检测到 /usr/lib/libglib-2.0.a），跳过 apk。要重装加 --force"
else
    echo "###### 1. 写 apk 源 ######"
    cat > /etc/apk/repositories <<'REOF'
https://mirrors.tencent.com/alpine/v3.15/main
https://mirrors.tencent.com/alpine/v3.15/community
REOF

    echo "###### 2. apk update（勿加 --quiet）######"
    apk update || { echo "FATAL apk update failed"; exit 1; }

    echo "###### 3. apk add ######"
    apk add --no-cache \
        lua5.3 lua5.3-dev lua5.3-busted lua5.3-lgi \
        xcb-util-dev xcb-util-wm-dev xcb-util-image-dev \
        xcb-util-keysyms-dev xcb-util-cursor-dev xcb-util-xrm-dev \
        startup-notification-dev libxdg-basedir-dev \
        dbus-dev libxkbcommon-dev libxkbcommon-static \
        glib-static cairo-static \
        pcre-dev expat-static zlib-static \
        freetype-static fontconfig-static \
        libpng-static libjpeg-turbo-static \
        pkgconf util-macros || { echo "FATAL apk add"; exit 1; }
fi

# --- 4. 校验 AWESOME_DEPENDENCIES ----------------------------------------
MISS=0
echo "###### 4. 校验 22 项 pkg-config ######"
for m in glib-2.0 gdk-pixbuf-2.0 cairo x11 xcb xcb-cursor xcb-randr xcb-xtest \
         xcb-xinerama xcb-shape xcb-util xcb-keysyms xcb-icccm xcb-xfixes xcb-xkb \
         xkbcommon xkbcommon-x11 cairo-xcb libstartup-notification-1.0 xproto \
         libxdg-basedir xcb-xrm; do
    v=$(pkg-config --modversion "$m" 2>/dev/null) \
        || { echo "  x $m MISSING"; MISS=$((MISS + 1)); continue; }
    printf '  ok %-32s %s\n' "$m" "$v"
done

# --- 5. 校验 .a -----------------------------------------------------------
echo "###### 5. 校验 18 个 .a ######"
for a in libxcb-icccm libxcb-ewmh libxcb-keysyms libxcb-cursor libxcb-xrm \
         libxcb libstartup-notification-1 libxkbcommon libxkbcommon-x11 \
         libglib-2.0 libgobject-2.0 libgio-2.0 libcairo libpcre libffi \
         libexpat liblzma; do
    f=$(ls /usr/lib/$a.a /lib/$a.a 2>/dev/null | head -1)
    [ -n "$f" ] && echo "  ok $a.a" || { echo "  x $a.a MISSING"; MISS=$((MISS + 1)); }
done

# --- 6. 编译冒烟 ----------------------------------------------------------
echo "###### 6. ICCCm + EWMH 编译冒烟 ######"
cat > /tmp/t_icccm.c <<'CEOF'
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_ewmh.h>
int main(void){ return 0; }
CEOF
gcc -o /tmp/t_icccm $(pkg-config --cflags --libs xcb-icccm xcb-ewmh) /tmp/t_icccm.c \
    && echo "  ok ICCCm+EWMH COMPILE" || { echo "  x ICCCm COMPILE FAIL"; MISS=$((MISS + 1)); }

echo
echo "缺失总数: $MISS"
if [ "$MISS" -eq 0 ]; then
    echo "ALL GREEN"
else
    echo "HAS MISSING —— 段3 补源码静态库，段4 做全量核查"
fi
echo "下一步： docker exec builder-awe sh /_ext/home/headless/xm-zs01/fk-awesomewm/docs/sam/scripts/03-container-build-static-libs.sh"
