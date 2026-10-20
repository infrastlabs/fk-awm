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
    log "cache: clone x11base 组件树(/src) + 预下载"
    # x11base 组件树（v-tint2/common.sh），铺到 /src（保留 /src/awesome；不 rm /src！）
    if [ ! -f /src/common.sh ]; then
        rm -rf /tmp/x11base
        git clone --depth=1 https://gitee.com/infrastlabs/docker-x11base /tmp/x11base
        mkdir -p /src
        cp -a /tmp/x11base/compile/src/. /src/
        rm -rf /tmp/x11base
    fi
    mkdir -p $CACHE
    curl -sSLo $CACHE/dbus-1.12.26.tar.gz https://dbus.freedesktop.org/releases/dbus/dbus-1.12.26.tar.gz
    curl -sSLo $CACHE/lua-5.3.6.tar.gz https://www.lua.org/ftp/lua-5.3.6.tar.gz
    curl -sSLo $CACHE/pango-1.49.3.tar.xz https://download.gnome.org/sources/pango/1.49/pango-1.49.3.tar.xz
    rm -rf $CACHE/xdg
    git clone --depth=1 https://gitee.com/g-system/fk-libxdg-basedir $CACHE/xdg
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
    cd $SRC/lua && make generic MYCFLAGS="-fPIC" MYLDFLAGS="-DLUA_USE_POSIX" MYLIBS="-lm" \
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
xcbutil()  { log "xcb-util";  bash /src/v-tint2/build.sh xcbutil; }
libcroco() { log "libcroco"; STATIC=yes bash /src/v-tint2/build.sh libcroco; }
librsvg()  { log "librsvg";  STATIC=yes bash /src/v-tint2/build.sh librsvg; }
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
