#!/bin/bash
# ============================================================================
# awm 三架构出包 buildx.sh（对齐 docker-x11base compile/buildx.sh 链路）
#
# 用法（在仓库根跑）：
#   bash .github/scripts/buildx.sh            # 三架构 build + undock 取物
#   bash .github/scripts/buildx.sh amd64      # 单架构（本地调试用）
#
# 前置：
#   - 03 前置物料在宿主机 /_ext/tmp：dbus-1.12.26.tar.gz、lua-5.3.6.tar.gz、
#     libxdg-basedir/（下载命令见 03 脚本头）
#   - registry 登录（或 env DOCKER_REGISTRY_USER/PW）
#
# 产物：
#   .github/workflows/.ws/pkgs/awm-static-musl-<ver>-<arch>.tar.gz
# ============================================================================
set -euo pipefail
cd "$(dirname "$0")/../.."   # 仓库根

NS=${NS:-infrastlabs}
REPO_HUB=${REPO_HUB:-registry.cn-shenzhen.aliyuncs.com}
IMG=${IMG:-x11-base:awm}
WS=.github/workflows/.ws
MAT=$WS/mat
PKGS=$WS/pkgs
CTX=.buildx-ctx

# ---------- 构建 context 准备 ----------
rm -rf "$CTX" && mkdir -p "$CTX/mat"
# 仓库源码（完整树含 .git：11 脚本 REPO 同时是 cmake 源码目录 + git describe 来源）
rm -rf "$CTX/repo" && mkdir -p "$CTX/repo"
tar --exclude='.buildx-ctx' --exclude='.github/workflows/.ws' \
    --exclude='.git/worktrees' -cf - . | tar -xf - -C "$CTX/repo"
# 03 前置物料
for f in dbus-1.12.26.tar.gz lua-5.3.6.tar.gz; do
    [ -f "/_ext/tmp/$f" ] && cp -a "/_ext/tmp/$f" "$CTX/mat/" \
        || { echo "FATAL: 缺 03 前置物料 /_ext/tmp/$f（下载命令见 03 脚本头）"; exit 1; }
done
[ -d /_ext/tmp/libxdg-basedir ] && cp -a /_ext/tmp/libxdg-basedir "$CTX/mat/" \
    || { echo "FATAL: 缺 libxdg-basedir（03 脚本头有 clone 命令）"; exit 1; }
echo "context ready: $(du -sh "$CTX" 2>/dev/null | cut -f1)"

# ---------- buildx build ----------
PLAT="linux/amd64,linux/arm64,linux/arm"
case "${1:-}" in
    amd64) PLAT="linux/amd64" ;;
    arm64) PLAT="linux/arm64" ;;
    armv7) PLAT="linux/arm" ;;
esac
CACHE_REF="$REPO_HUB/$NS/$IMG-cache"
CACHE="--cache-from type=registry,ref=$CACHE_REF"
CACHE="$CACHE --cache-to type=registry,ref=$CACHE_REF,mode=max"

docker buildx build $CACHE \
    --platform "$PLAT" \
    --provenance=false \
    --build-arg X11BASE_REPO=${X11BASE_REPO:-https://gitee.com/infrastlabs/docker-x11base} \
    -t "$REPO_HUB/$NS/$IMG" \
    -f .github/scripts/Dockerfile \
    "$CTX"

# ---------- undock 取物（参考 docker-x11base .github/workflows/undock.sh）----------
mkdir -p "$PKGS" "$CTX/undock-bin"
UNDOCK="$CTX/undock-bin/undock"
if [ ! -x "$UNDOCK" ]; then
    file=undock_0.8.0_linux_amd64.tar.gz
    curl -o "$CTX/undock-bin/$file" -k -fSL \
        https://gitee.com/g-golang/fk-undock/releases/download/v24.12/$file
    tar -zxf "$CTX/undock-bin/$file" -C "$CTX/undock-bin/"
fi

# 版本号（git describe，与 11 脚本同款）
DESCRIBE=$(git describe --tags --long 2>/dev/null || echo "4.3-0")
VER=${DESCRIBE#v}; VER=${VER%-g*}
[ -n "$VER" ] || VER="4.3-0"

ROOTFS="$CTX/rootfs"; rm -rf "$ROOTFS"
$UNDOCK --log-nocolor --include=/rootfs/usr/local/static --insecure \
    --all "$REPO_HUB/$NS/$IMG" "$ROOTFS/" 2>/dev/null || \
$UNDOCK --log-nocolor --include=/rootfs/usr/local/static --insecure \
    "$REPO_HUB/$NS/$IMG" "$ROOTFS/"

for arch in amd64 arm64 armv7; do
    rootfs="$ROOTFS/linux_$arch/rootfs"
    [ -d "$rootfs" ] || { echo "warn: $arch 无产物，跳过"; continue; }
    ( cd "$rootfs" && tar -zcf "$PKGS/awm-static-musl-$VER-$arch.tar.gz" usr/local/static/awm )
    echo "==> $arch done"
done

rm -rf "$CTX/rootfs"
echo "==> 产物：$PKGS/"
ls -lhS "$PKGS"/*.tar.gz 2>/dev/null || true
