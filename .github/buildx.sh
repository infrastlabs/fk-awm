#!/bin/bash
# awm 三架构出包（对齐 x11base buildx.sh：buildx 三架构构建 + undock 取物）
# 用法: buildx.sh [amd64|arm64|armv7]（默认三架构）
set -euo pipefail
cd "$(dirname "$0")/.."

NS=${NS:-infrastlabs}
# x11base env 体系：REPO_HUB(dockerhub)/REPO_ALI(aliyun)/REPO_TEN_HK(腾讯香港 cache)
REPO_HUB=${REPO_HUB:-registry-1.docker.io}
REPO_ALI=${REPO_ALI:-registry.cn-shenzhen.aliyuncs.com}
REPO_TEN_HK=${REPO_TEN_HK:-hkccr.ccs.tencentyun.com}
# push 目标（默认 aliyun，同 x11base REPO=$REPO_ALI）
REPO=${REPO:-$REPO_ALI}
IMG=${IMG:-x11-base:awm}
REPO_ROOT=$(pwd)
WS=$REPO_ROOT/.github/workflows/.ws
PKGS=$WS/pkgs

# x11base 组件树由 build.sh cache 层在容器内 clone（不在此处准备）
PLAT="linux/amd64"
PLAT="linux/amd64,linux/arm64"
PLAT="linux/amd64,linux/arm64,linux/arm"

# cache 存腾讯香港（ali 存不了 cache）
CACHE_REF="$REPO_TEN_HK/$NS/$IMG-cache"
CACHE="--cache-from type=registry,ref=$CACHE_REF"
CACHE="$CACHE --cache-to type=registry,ref=$CACHE_REF,mode=max"

docker buildx build $CACHE \
    --platform "$PLAT" \
    --provenance=false \
    --push \
    -t "$REPO/$NS/$IMG" \
    -f .github/Dockerfile \
    .

mkdir -p "$PKGS" "$WS/undock-bin"
UNDOCK="$WS/undock-bin/undock"
if [ ! -x "$UNDOCK" ]; then
    file=undock_0.8.0_linux_amd64.tar.gz
    curl -o "$WS/undock-bin/$file" -k -fSL \
        https://gitee.com/g-golang/fk-undock/releases/download/v24.12/$file
    tar -zxf "$WS/undock-bin/$file" -C "$WS/undock-bin/"
fi

# 版本号：固定与 v4.3 对比（--match 忽略 lib-vendor.lgishim 等其他 tag——GH 仓库
# tag 集与本地不同，裸 describe 选最近 tag 导致版本错乱如 vlib-vendor.lgishim-104）
DESCRIBE=$(git describe --tags --long --match "v4.3" 2>&1)
if [ -z "$DESCRIBE" ] || echo "$DESCRIBE" | grep -q "fatal\|No names found"; then
    echo "warn: git describe 失败（$DESCRIBE）——用 v4.3-0 代替"
    VER="4.3-0"
else
    VER=${DESCRIBE#v}; VER=${VER%-g*}
    [ -n "$VER" ] || VER="4.3-0"
fi
SHA=$(git rev-parse --short=7 HEAD 2>/dev/null || echo "nogit")
# 包名对齐本地规范（11 脚本）：awm-static-musl-<yymmdd-HHMMSS>-v<ver>-<sha7>-<arch>.tar.gz
TS=$(TZ=UTC-8 date +%y%m%d-%H%M%S)
PKGOUT() { echo "awm-static-musl-${TS}-v${VER}-${SHA}-${1}.tar.gz"; }

ROOTFS="$WS/rootfs"; rm -rf "$ROOTFS"
$UNDOCK --log-nocolor --include=/rootfs/usr/local/static --insecure \
    --all "$REPO/$NS/$IMG" "$ROOTFS/" 2>/dev/null || \
$UNDOCK --log-nocolor --include=/rootfs/usr/local/static --insecure \
    "$REPO/$NS/$IMG" "$ROOTFS/"

# 展开形态（实测）：
#   多架构 index（--all）→ $ROOTFS/linux_<arch>/rootfs/usr/local/static/awm
#   单架构 OCI manifest → $ROOTFS/rootfs/usr/local/static/awm（无 linux_<arch> 层）
for arch in amd64 arm64 armv7; do
    rootfs="$ROOTFS/linux_$arch/rootfs"
    if [ -d "$ROOTFS/rootfs/usr/local/static/awm" ] && [ ! -d "$rootfs" ] && [ "$arch" = "amd64" ]; then
        rootfs="$ROOTFS/rootfs"   # 单架构展开（默认只 build amd64）
    fi
    [ -d "$rootfs" ] || { echo "warn: $arch 无产物，跳过"; continue; }
    ( cd "$rootfs" && tar -zcf "$PKGS/$(PKGOUT "$arch")" usr/local/static/awm )
    echo "==> $arch done"
done

rm -rf "$ROOTFS"
echo "==> 产物：$PKGS/"
ls -lhS "$PKGS"/*.tar.gz 2>/dev/null || true
