# ============================================================================
# awm 静态包多阶段构建镜像（参考 docker-x11base compile/src/Dockerfile 链路）
#
# 产物约定（对齐 x11base undock 取物）：
#   stage awm-build（本文件内）→ 安装树 /usr/local/static/awm
#   stage final（alpine）→ COPY --from=awm-build /rootfs /rootfs
#     ⇒ 镜像内路径 /rootfs/usr/local/static/awm/
#     ⇒ undock --include=/rootfs/usr/local/static 提取 → tar 出
#       awm-static-musl-<ver>-<arch>.tar.gz（解包即 /usr/local/static/awm，
#       prefix 与编译期 config.h 一致）
#
# 层缓存：L1 apk 依赖（几乎不变）→ L2 源编 .a（03+pango，库版本不变则命中）
#        → L3 11 出包（每跑必变：时间戳/代码）。三架构共享 registry 缓存，
#        QEMU 慢的部分（L1/L2）只编一次。
# ============================================================================
ARG REPO=registry.cn-shenzhen.aliyuncs.com
ARG BASE_IMG=${REPO}/infrastlabs/x11-base:alpine-builder-gtk224

# ---------- stage awm-build：依赖 + 源编 + 出包 ----------
FROM ${BASE_IMG} AS awm-build

ARG X11BASE_REPO=https://gitee.com/infrastlabs/docker-x11base

# L0 x11base 组件树（v-tint2/openbox 配方）
RUN git clone --depth=1 ${X11BASE_REPO} /x11base \
    && test -f /x11base/compile/src/v-tint2/build.sh

# 仓库脚本（02/03/11）从构建上下文复制
COPY docs/sam/scripts /scripts

# L1 apk 依赖（02-container-install-deps.sh 的 apk 清单 + 3.1-3.4 补缺）
# 02 清单（v3.15 源）：lua5.3 链 / xcb 链 / 桌面库 / 静态库
RUN apk update \
 && apk add --no-cache \
        lua5.3 lua5.3-dev lua5.3-busted lua5.3-lgi \
        xcb-util-dev xcb-util-wm-dev xcb-util-image-dev \
        xcb-util-keysyms-dev xcb-util-cursor-dev xcb-util-xrm-dev \
        startup-notification-dev libxdg-basedir-dev \
        dbus-dev libxkbcommon-dev libxkbcommon-static \
        glib-static cairo-static \
        pcre-dev expat-static zlib-static \
        freetype-static fontconfig-static \
        libpng-static libjpeg-turbo-static \
        pkgconf util-macros \
 # 3.1-3.4 补缺（02 旧清单缺口 + pango 编译 deps）
        cmake ninja build-base icu-static \
        harfbuzz-static pixman-static brotli-static \
        fribidi-dev fribidi-static meson \
        libxml2-dev autoconf automake libtool

# L2 源编静态库：03（xcb-util/xdg/dbus/lua）+ v-tint2（libcroco/librsvg）+ pango meson
# 03 的前置物料（dbus/lua tarball + libxdg-basedir）在 buildx.sh 挂载进 context，
# 这里从构建上下文复制（不在容器内网络拉取，避免 QEMU 下外网抖动）
COPY mat/ /mat/
RUN SRC_TREE=/x11base/compile/src \
    XDG=/mat/libxdg-basedir \
    DBUS_TAR=/mat/dbus-1.12.26.tar.gz \
    LUA_TAR=/mat/lua-5.3.6.tar.gz \
    bash /scripts/03-container-build-static-libs.sh \
 && ln -sfn /x11base/compile/src /src \
 && bash /src/v-tint2/build.sh libcroco \
 && bash /src/v-tint2/build.sh librsvg \
 && mkdir -p /tmp/pango-ci \
 && curl -sSL https://download.gnome.org/sources/pango/1.49/pango-1.49.3.tar.xz \
      | tar -xJ --strip 1 -C /tmp/pango-ci \
 && cd /tmp/pango-ci \
 && meson build -Ddefault_library=static -Dintrospection=disabled \
        -Dgtk_doc=false -Dprefix=/usr \
 && meson compile -C build \
 && meson install --no-rebuild -C build

# L3 出包（11 all；REPO 指向带 .git 的仓库——buildx.sh 挂载）
COPY repo/ /repo/
RUN set -e; \
    cd /repo && git rev-parse --short=7 HEAD >/tmp/sha.txt; \
    REPO=/repo \
    BUILD_DIR=/out/build \
    OUT_DIR=/out/pkgs \
    bash /scripts/11-package-static-release.sh all \
 && ls -la /out/pkgs/ \
 && mkdir -p /rootfs/usr/local/static \
 && cp -a /usr/local/static/awm /rootfs/usr/local/static/ \
 && echo "==> rootfs awm tree:" && find /rootfs/usr/local/static/awm -type f | wc -l

# ---------- stage final：alpine 根镜像（产物在 /rootfs） ----------
FROM alpine:3.15
COPY --from=awm-build /rootfs /rootfs
ENV STATIC=/usr/local/static
WORKDIR ${STATIC}
# 校验产物
RUN test -f /rootfs/usr/local/static/awm/bin/awesome \
 && echo "awm package embedded OK"
