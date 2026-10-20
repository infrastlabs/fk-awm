#!/bin/sh
# awm 构建脚本（容器内；逻辑拷贝自 docs/sam/scripts/{02,03,11}）
# 用法: build.sh [cache|b_deps|awm|<库名>]
set -e
REPO=/src/awesome
SRC=/tmp/awm-src
CACHE=/tmp/awm-cache
mkdir -p $SRC
log() { echo -e "\n>>> $*"; }

# L0 预下载源码包到 $CACHE（URL 不变则 Docker 层缓存命中）
cache() {
    log "cache: 预下载源码包"
    mkdir -p $CACHE
    curl -sSLo $CACHE/dbus-1.12.26.tar.gz https://dbus.freedesktop.org/releases/dbus/dbus-1.12.26.tar.gz
    curl -sSLo $CACHE/lua-5.3.6.tar.gz https://www.lua.org/ftp/lua-5.3.6.tar.gz
    curl -sSLo $CACHE/pango-1.49.3.tar.xz https://download.gnome.org/sources/pango/1.49/pango-1.49.3.tar.xz
    rm -rf $CACHE/xdg
    git clone --depth=1 https://gitee.com/g-system/fk-libxdg-basedir $CACHE/xdg
    # 预下载 libcroco/librsvg 源码（现场 clone 在 arm64/QEMU 下网络抖动
    # Connection reset，移到 cache 层下载，URL 不变层缓存命中）
    rm -rf $CACHE/libcroco $CACHE/librsvg
    git clone --depth=1 -b 0.6.13 https://gitee.com/g-system/fk-libcroco $CACHE/libcroco
    # librsvg 必须 2.40.21（C 版）——默认分支 2.42 是 Rust 重写版，configure 硬查 cargo/rustc
    git clone --depth=1 -b 2.40.21 https://gitee.com/g-system/fk-librsvg $CACHE/librsvg
    # xcb-util 带 submodule（util-common-m4 gitee 版），cache 一次性拉全
    rm -rf $CACHE/xcb__util
    git clone --depth=1 https://gitee.com/g-system/fk-xcb__util $CACHE/xcb__util
    cd $CACHE/xcb__util \
        && sed -i "s^git://anongit.freedesktop.org/xcb/util-common-m4.git^https://gitee.com/g-system/util-common-m4.git^g" .gitmodules \
        && git submodule update --init --depth=1
    cd /
    case ${TARGETPLATFORM} in
        linux/arm64)  arch=arm64 ;;
        linux/arm/v7) arch=arm ;;
        *)            arch=amd64 ;;
    esac
    curl -fSL -k -o $CACHE/upx.tar.xz \
        https://github.com/upx/upx/releases/download/v4.2.2/upx-4.2.2-${arch}_linux.tar.xz
    rm -rf $CACHE/upx && mkdir -p $CACHE/upx
    tar -Jxf $CACHE/upx.tar.xz -C $CACHE/upx --strip-components 1
    ls -la $CACHE/
}

# L1 apk 依赖
apkdeps() {
    log "apk 依赖"
    # 覆盖写入多源（非追加）：tuna 优先 + 腾讯兜底——基础镜像 USTC 在
    # GH Actions arm64/QEMU 下连接不稳；apk 按声明顺序请求，前列挂自动降级
    cat > /etc/apk/repositories <<'REPO'
https://mirrors.tuna.tsinghua.edu.cn/alpine/v3.15/main
https://mirrors.tuna.tsinghua.edu.cn/alpine/v3.15/community
https://mirrors.tencent.com/alpine/v3.15/main
https://mirrors.tencent.com/alpine/v3.15/community
REPO
    apk update && apk add --no-cache \
        lua5.3 lua5.3-dev lua5.3-busted lua5.3-lgi \
        xcb-util-dev xcb-util-wm-dev xcb-util-image-dev \
        xcb-util-keysyms-dev xcb-util-cursor-dev xcb-util-xrm-dev \
        startup-notification-dev libxdg-basedir-dev \
        dbus-dev libxkbcommon-dev libxkbcommon-static \
        glib-static cairo-static pcre-dev expat-static zlib-static \
        freetype-static fontconfig-static libpng-static libjpeg-turbo-static \
        pkgconf util-macros \
        cmake ninja build-base icu-static \
        harfbuzz-static pixman-static brotli-static \
        fribidi-dev fribidi-static meson \
        libxml2-dev autoconf automake libtool curl gawk git \
        imagemagick
}

# 源编库（读 $CACHE）
xdgbasedir() {
    log "libxdg-basedir"
    test -s /usr/local/lib/libxdg-basedir.a && return 0
    rm -rf $SRC/xdg && cp -a $CACHE/xdg $SRC/xdg
    cd $SRC/xdg && ./autogen.sh \
        && ./configure --disable-shared --enable-static --prefix=/usr/local \
        && make && make install
}
dbus() {
    log "dbus"
    test -s /usr/local/lib/libdbus-1.a && return 0
    rm -rf $SRC/dbus && mkdir -p $SRC/dbus
    tar -xzf $CACHE/dbus-1.12.26.tar.gz --strip 1 -C $SRC/dbus
    cd $SRC/dbus && ./configure --disable-shared --enable-static --prefix=/usr/local \
        --disable-systemd --disable-selinux --disable-elogind \
        --disable-x11 --disable-glib \
        --disable-tests --disable-docs --disable-man \
        && make -j && make install
}
lua() {
    log "lua"
    test -s /usr/local/lib/liblua.a && return 0
    rm -rf $SRC/lua && mkdir -p $SRC/lua
    tar -xzf $CACHE/lua-5.3.6.tar.gz --strip 1 -C $SRC/lua
    cd $SRC/lua && make generic MYCFLAGS="-fPIC -DLUA_USE_POSIX" MYLDFLAGS="" MYLIBS="-lm" \
        && make install generic
    cat > /usr/local/lib/pkgconfig/lua5.3.pc <<'PC'
prefix=/usr/local
libdir=${prefix}/lib
includedir=${prefix}/include
Name: Lua
Version: 5.3.6
Libs: -L${libdir} -llua -lm
Cflags: -I${includedir}
PC
}
xcbutil()  {
    log "xcb-util（自实现，源码来自 cache 含 submodule；失败即停）"
    test -s /usr/local/lib/libxcb-util.a && { log "skip xcb-util"; return 0; }
    apk add --no-cache util-macros
    rm -rf /tmp/xcb-util && cp -a $CACHE/xcb__util /tmp/xcb-util
    cd /tmp/xcb-util && ./autogen.sh \
        && make && make install
    test -s /usr/local/lib/libxcb-util.a || { echo "FATAL: libxcb-util.a 未产出"; exit 1; }
}
libcroco() {
    log "libcroco（自实现，源码来自 cache；set -e 失败即停）"
    test -s /usr/local/lib/libcroco-0.6.a && { log "skip libcroco"; return 0; }
    rm -rf /tmp/libcroco && cp -a $CACHE/libcroco /tmp/libcroco
    cd /tmp/libcroco && ./autogen.sh \
        && ./configure --enable-static --disable-shared --prefix=/usr/local \
        && make && make install
    test -s /usr/local/lib/libcroco-0.6.a || { echo "FATAL: libcroco .a 未产出"; exit 1; }
}
librsvg() {
    log "librsvg（自实现，源码来自 cache；失败即停）"
    test -s /usr/local/lib/librsvg-2.a \
        && { log "skip librsvg"; return 0; }
    rm -rf /tmp/librsvg && cp -a $CACHE/librsvg /tmp/librsvg
    cd /tmp/librsvg && ./autogen.sh \
        && ./configure --enable-static --disable-shared --prefix=/usr/local \
        && make && make install
    # .a 校验即代表 make install 完整成功（头文件随 .a 一起装）
    test -s /usr/local/lib/librsvg-2.a || { echo "FATAL: librsvg .a 未产出"; exit 1; }
}
pango() {
    log "pango"
    test -s /usr/lib/libpango-1.0.a && return 0
    rm -rf /tmp/pango-ci && mkdir -p /tmp/pango-ci
    tar -xJf $CACHE/pango-1.49.3.tar.xz --strip 1 -C /tmp/pango-ci
    cd /tmp/pango-ci && meson build -Ddefault_library=static -Dintrospection=disabled \
        -Dgtk_doc=false -Dprefix=/usr \
        && meson compile -C build && meson install --no-rebuild -C build
}

# L1b 源编并行
b_deps() {
    apkdeps
    log "源编并行"
    xdgbasedir & dbus & lua & pango & xcbutil &
    wait
    libcroco
    librsvg &
    wait
    # 产物校验：缺 .a 即 FATAL（先前构建脚本吞失败一路放行，
    # 必须在源头拦截，别等到 awm 编译才炸）
    log "b_deps 产物校验"
    MISS=0
    for a in libxcb-util libxdg-basedir libdbus-1 liblua libcroco-0.6 librsvg-2; do
        f=$(ls /usr/local/lib/$a.a /usr/lib/$a.a 2>/dev/null | head -1)
        if [ -n "$f" ]; then echo "  ok  $a.a"; else echo "  MISS $a.a"; MISS=1; fi
    done
    [ -f /usr/lib/libpango-1.0.a ] && echo "  ok  libpango-1.0.a" || { echo "  MISS libpango-1.0.a"; MISS=1; }
    [ "$MISS" -eq 0 ] || { echo "FATAL: b_deps 产物不全"; exit 1; }
}

# L2 最终编译 + rootfs 组装
awm() {
    log "cmake configure"
    export AWESOME_IGNORE_LGI=1
    rm -rf /tmp/awm-build && mkdir -p /tmp/awm-build /tmp/awm-pkgs
    cmake -S $REPO -B /tmp/awm-build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_EXE_LINKER_FLAGS="-static -Wl,--as-needed -Wl,--strip-all" \
        -DAWESOME_STATIC_DEPS=ON -DAWESOME_LGISHIM=ON \
        -DLUA_EXECUTABLE=/usr/bin/lua5.3 \
        -DCMAKE_INSTALL_PREFIX=/usr/local/static/awm \
        -DSYSCONFDIR=/usr/local/static/awm/etc \
        -DXDG_CONFIG_DIR=/usr/local/static/awm/etc/xdg \
        -DAWESOME_DATA_PATH=/usr/local/static/awm/share/awesome \
        -DAWESOME_DOC_PATH=/usr/local/static/awm/share/doc/awesome \
        -DAWESOME_MAN_PATH=/usr/local/static/awm/share/man \
        -DAWESOME_XSESSION_PATH=/usr/local/static/awm/share/xsessions
    log "ninja + upx"
    ninja -C /tmp/awm-build awesome
    cp -a /tmp/awm-build/awesome /tmp/awm-build/awesome.pristine
    $CACHE/upx/upx -9 -q -o /tmp/awm-build/awesome.upx /tmp/awm-build/awesome.pristine 2>/dev/null \
        && mv /tmp/awm-build/awesome.upx /tmp/awm-build/awesome || true
    file -b /tmp/awm-build/awesome
    log "install"
    cmake --install /tmp/awm-build
    log "rootfs 组装"
    mkdir -p /rootfs/usr/local/static
    rm -rf /rootfs/usr/local/static/awm
    cp -a /usr/local/static/awm /rootfs/usr/local/static/
    echo "rootfs awm: $(find /rootfs/usr/local/static/awm -type f | wc -l) files"
}

case "$1" in
    cache) cache ;;
    b_deps) b_deps ;;
    awm) awm ;;
    apkdeps) apkdeps ;;
    xdgbasedir|dbus|lua|xcbutil|libcroco|librsvg|pango) $1 ;;
    *) echo "usage: build.sh [cache|b_deps|awm|apkdeps|xdgbasedir|dbus|lua|xcbutil|libcroco|librsvg|pango]"; exit 1 ;;
esac
