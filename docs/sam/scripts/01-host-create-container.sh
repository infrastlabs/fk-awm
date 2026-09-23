#!/bin/sh
# ============================================================
# 01-host-create-container.sh —— 段1：建容器
#
# 在哪跑：宿主机（docker 命令只有宿主能执行，这也是唯一必须在宿主的步骤）
# 用法：
#   sh /_ext/home/headless/xm-zs01/fk-awesomewm/docs/sam/scripts/01-host-create-container.sh
#   sh /_ext/home/headless/xm-zs01/fk-awesomewm/docs/sam/scripts/01-host-create-container.sh --clean   # 先删容器再建
#
# 干了什么：
#   1. 查 builder-awe 是否已存在 → 已存在则跳过（环境装在容器 fs 里，别误删）
#   2. docker run -d --name builder-awe --network=host --privileged \
#        -v /_ext:/_ext <x11-base:alpine-builder-gtk224> bash -c 'while :; do sleep 3600; done'
#   3. 打印容器状态
#
# 产物：容器运行中，/_ext 已挂载（容器内路径与宿主一致，脚本免路径转换）
#
# 坑（实测）：
#   1. --privileged 必须有 —— 缺失时 apk update 卡在
#      `fetch http://.../v3.15/main/x86_64/APKINDEX.tar.gz`，无错误无后续输出，纯挂死
#   2. --network=host 必须有 —— 容器自己那套 docker 网桥很慢
#   3. 只挂一个卷 -v /_ext:/_ext —— 容器内路径与宿主一致，省掉路径转换
#   4. 不加 --rm —— 调试期用长期命名容器，环境装在容器 fs 里
#   5. 不 commit、不 push 镜像 —— infrastlabs 命名空间 push 实测 denied
#   6. --clean 会清空容器 fs，段2~段4 的所有成果全没，得重跑 02/03/04
# ============================================================
set -eu

C=builder-awe
IMG=registry.cn-shenzhen.aliyuncs.com/infrastlabs/x11-base:alpine-builder-gtk224

command -v docker >/dev/null 2>&1 || { echo "FATAL: 需在宿主机执行（找不到 docker）"; exit 2; }

# --- 1. 清理（可选）-------------------------------------------------------
if [ "${1:-}" = "--clean" ]; then
    echo "!!! 删除容器 $C —— 容器 fs 全部清空，已装的包与源码编译产物全没"
    docker rm -f "$C"
fi

# --- 2. 建容器（幂等）------------------------------------------------------
if docker ps -a --format '{{.Names}}' | grep -qx "$C"; then
    echo "[skip] $C 已存在，复用容器 fs（要重来请加 --clean）"
else
    docker run -d --name "$C" --network=host --privileged \
        -v /_ext:/_ext "$IMG" bash -c 'while :; do sleep 3600; done'
    echo "[ok] $C 已创建"
fi

# --- 3. 状态 ---------------------------------------------------------------
docker ps -a --filter "name=$C" --format '  {{.Names}}  {{.Status}}'
echo
echo "下一步： docker exec builder-awe sh /_ext/home/headless/xm-zs01/fk-awesomewm/docs/sam/scripts/02-container-install-deps.sh"
