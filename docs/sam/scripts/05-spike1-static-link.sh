#!/bin/bash
# 05-spike1-static-link.sh — Spike 1 全静态链接迭代（host 侧驱动）
#
# 目的：不做 shim，仅用「已就绪的静态库 + awesome 原始源码」尝试全静态链接，
#       找出真正的硬门槛（无法静态化的依赖，如 dlopen）。
#
# 用法:
#   bash docs/sam/scripts/05-spike1-static-link.sh [build_dir]
#   bash docs/sam/scripts/05-spike1-static-link.sh reset   # 清空历史轮次
#
# 参考配方: docker-x11base/compile/src/v-pcmanfm/build.sh 的 OB_LIBS（tmux-3 同源）
#
# 产出:
#   /_ext/tmp/spike1/round<N>.log      每轮完整链接日志
#   /_ext/tmp/spike1/round<N>.undef    每轮未定义符号（去重排序）
#   /_ext/tmp/spike1/analysis<N>.txt   每轮符号可解析性分析
set -u

BUILDDIR=${1:-/_ext/tmp/awe-build-static}
REPO=/_ext/home/headless/xm-zs01/fk-awesomewm
OUTDIR=/_ext/tmp/spike1
mkdir -p "$OUTDIR"

if [ "$BUILDDIR" = "reset" ]; then
  docker exec builder-awe bash -c "rm -rf $OUTDIR && mkdir -p $OUTDIR && chown -R 1000:1000 $OUTDIR 2>/dev/null"
  echo "reset done"
  exit 0
fi

ROUND=0
for f in "$OUTDIR"/round*.log; do [ -e "$f" ] && ROUND=$((ROUND+1)); done

echo "=== Spike 1 round $ROUND ==="
docker exec builder-awe bash -c "bash $REPO/docs/sam/scripts/05b-spike1-round.sh $ROUND '$BUILDDIR'"
docker exec builder-awe chown -R 1000:1000 "$OUTDIR" 2>/dev/null || true

echo "  --- analysis$ROUND.txt (tail) ---"
tail -n 25 "$OUTDIR/analysis$ROUND.txt" 2>/dev/null | sed 's/^/    /'
echo "=== round $ROUND done ==="
