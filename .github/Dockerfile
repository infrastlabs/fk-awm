# awm 静态包构建镜像（对齐 x11base：/src=组件树 /src/awesome=项目）
# 分段调 build.sh：cache/b_deps/awm 独立层；产物 rootfs 供 undock 提取
ARG REPO=registry.cn-shenzhen.aliyuncs.com
ARG BASE_IMG=${REPO}/infrastlabs/x11-base:alpine-builder-gtk224

FROM ${BASE_IMG} AS awm-build
ARG TARGETPLATFORM

COPY . /src/awesome

# L0 x11base 组件树 clone(/src) + 源码包预下载（含按架构静态 upx）
RUN bash /src/awesome/.github/scripts/build.sh cache
# L1 apk 依赖 + 源编（b_deps 内并行）
RUN bash /src/awesome/.github/scripts/build.sh b_deps
# L2 最终编译 + rootfs 组装
RUN bash /src/awesome/.github/scripts/build.sh awm

FROM alpine:3.15
COPY --from=awm-build /rootfs /rootfs
ENV STATIC=/usr/local/static
WORKDIR ${STATIC}
RUN test -f /rootfs/usr/local/static/awm/bin/awesome \
 && echo "awm package embedded OK"
