# awm 静态包构建镜像
# 分段调 build.sh：cache/b_deps/awm 独立层；产物 rootfs 供 undock 提取
ARG REPO=registry.cn-shenzhen.aliyuncs.com
ARG BASE_IMG=${REPO}/infrastlabs/x11-base:alpine-builder-gtk224

FROM ${BASE_IMG} AS awm-build
ARG TARGETPLATFORM

# build.sh 单文件提前：cache/b_deps 层只依赖它，项目全量改动不破坏这两层缓存
COPY ./.github/scripts/build.sh /src/awesome/.github/scripts/build.sh
# L0 源码包预下载（含按架构静态 upx）
RUN bash /src/awesome/.github/scripts/build.sh cache
# L1 apk 依赖 + 源编（b_deps 内并行）
RUN bash /src/awesome/.github/scripts/build.sh b_deps

# L2 最终编译 + rootfs 组装（项目全量 COPY 放最后，只影响 awm 层）
COPY . /src/awesome
RUN bash /src/awesome/.github/scripts/build.sh awm

FROM alpine:3.15
COPY --from=awm-build /rootfs /rootfs
ENV STATIC=/usr/local/static
WORKDIR ${STATIC}
RUN test -f /rootfs/usr/local/static/awm/bin/awesome \
 && echo "awm package embedded OK"
