#!/bin/bash
# 06-spike2-shim-build.sh — Spike 2 Pango shim 静态构建（容器内执行）
#
# 在 Spike 1 已验证的全静态链接之上，接入 lib/vendor/lgishim_pango.c，
# 把 lgi.Pango / lgi.PangoCairo 换成调真实 Pango C API 的静态绑定。
#
# 两个坑（都已踩过，见 memory static_link_gotchas）：
#   1. 不要再 patch build.ninja。任何一次 cmake 重跑都会把 patch 冲掉，
#      而且半冲状态会把 LINK_LIBARIES 整行吃掉。库表统一写进 CMakeLists，
#      用 -DAWESOME_STATIC_DEPS -DAWESOME_LGISHIM 开关控制。
#   2. pkg_check_modules(... IMPORTED_TARGET) 会把 pango 解析成绝对路径
#      /usr/lib/libpango-1.0.so，与 -static 冲突。所以只取它的 include
#      dirs，库名在 CMakeLists 里手写 -l:libpango-1.0.a。
#
# 参数: <round> [build_dir]
set -u
ROUND=${1:-1}
BUILDDIR=${2:-/_ext/tmp/awe-build-static}
REPO=/_ext/home/headless/xm-zs01/fk-awesomewm
OUTDIR=/_ext/tmp/spike2
mkdir -p "$OUTDIR"

cd "$BUILDDIR" || exit 1

# --- 1. 配置 ---
AWESOME_IGNORE_LGI=1 cmake \
    -DAWESOME_STATIC_DEPS=ON -DAWESOME_LGISHIM=ON "$REPO" \
    > "$OUTDIR/round$ROUND.cmake.log" 2>&1
RC=$?
echo "cmake_exit=$RC"
if [ "$RC" -ne 0 ]; then
  tail -n 20 "$OUTDIR/round$ROUND.cmake.log" | sed 's/^/    /'
  exit 1
fi

# --- 2. 构建 ---
AWESOME_IGNORE_LGI=1 ninja awesome > "$OUTDIR/round$ROUND.log" 2>&1
RC=$?

# --- 3. 报告 ---
echo "ninja_exit=$RC"
grep -oE "undefined reference to \`[^']+'" "$OUTDIR/round$ROUND.log" \
  | sed "s/undefined reference to \`//" | sort -u \
  > "$OUTDIR/round$ROUND.undef"
echo "unique_undefined_symbols=$(wc -l < "$OUTDIR/round$ROUND.undef")"
if [ -f "$BUILDDIR/awesome" ]; then
  echo "binary: $(file -b "$BUILDDIR/awesome" | cut -c1-90)"
  echo "size: $(stat -c %s "$BUILDDIR/awesome") bytes"
fi
grep -E "attempted static link|undefined reference|cannot find -l|fatal error|Error [0-9]" \
  "$OUTDIR/round$ROUND.log" | head -n 10 | sed 's/^/    /'
echo "--- lgishim object ---"
grep -c "lgishim_pango.c.o" "$OUTDIR/round$ROUND.log" | sed 's/^/    refs: /'
