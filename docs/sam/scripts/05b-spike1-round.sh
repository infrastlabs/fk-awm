#!/bin/bash
# 05b-spike1-round.sh — Spike 1 单轮静态链接（容器内执行，由 05-*.sh 调用）
#
# 参数: <round> <build_dir>
set -u
ROUND=$1
BUILDDIR=${2:-/_ext/tmp/awe-build-static}
REPO=/_ext/home/headless/xm-zs01/fk-awesomewm
OUTDIR=/_ext/tmp/spike1
mkdir -p "$OUTDIR"

# --- 1. 幂等把 OB_LIBS 追加到 awesome 目标的 LINK_LIBRARIES ---
# 注意：必须定位 awesome 的边（build.ninja 中第一个 LINK_LIBRARIES 属于 lgi-check）。
# 顺序敏感：追加在原 LINK_LIBRARIES 末尾，让 transitive 静态库在主库之后解析符号。
python3 - "$BUILDDIR/build.ninja" <<'PYEOF'
import re, sys
p = sys.argv[1]
s = open(p).read()
mark = "# SPIKE1_OB_LIBS"
# --start-group/--end-group 让 ld 反复扫描直到符号收敛，避免静态库顺序迭代
OPEN = "-Wl,--start-group"
END = "-Wl,--end-group"
extra = (
    " -lxcb-image -lxcb-shm -lxcb-render-util -lX11-xcb"
    " -lXdmcp -lXau -lXext -lXft -lfreetype -lpng -lXrender"
    " -lexpat -lxml2 -lz -lbz2 -llzma -lbrotlidec -lbrotlicommon"
    " -lfribidi -lharfbuzz -lgraphite2"
    " -lgio-2.0 -lgobject-2.0 -lglib-2.0 -lgmodule-2.0"
    " -lpcre -lffi -lpixman-1 -ljpeg -lfontconfig"
    " -lmount -luuid -lblkid"
    " -lXinerama -lXfixes -lXrandr -lXcomposite"
    " -lm -lc -ldl -lpthread -lrt -lutil -lcrypt"
)
# 先剥掉上一轮插入的整行（从 --start-group 到标记），保证与当前库表一致
s = re.sub(r"-Wl,--start-group[^\n]*" + re.escape(mark), " ", s)
idx = s.find("build awesome:")
if idx < 0:
    print("ERROR: 'build awesome:' edge not found"); sys.exit(1)
nxt = s.find("\nbuild ", idx)
end = nxt if nxt >= 0 else len(s)
edge = s[idx:end]
k = edge.find("  LINK_LIBRARIES = ")
if k < 0:
    print("ERROR: LINK_LIBRARIES not found in awesome edge"); sys.exit(1)
# 在 cmake 原始库列表两侧加 group 边界，使其与 transitive 库一起收敛
eol = edge.find("\n", k)
val = edge[k + len("  LINK_LIBARIES = "):eol]
s = (s[:idx] + edge[:k] + "  LINK_LIBRARIES = " + OPEN + " " + val
     + extra + " " + END + " " + mark + edge[eol:] + s[end:])
open(p, "w").write(s)
print("ninja patched: True", mark in s)
PYEOF

# --- 2. 链接 ---
cd "$BUILDDIR" || exit 1
AWESOME_IGNORE_LGI=1 ninja awesome > "$OUTDIR/round$ROUND.log" 2>&1
RC=$?

# --- 3. 抽取未定义符号 + 可解析性分析 ---
EXTRACT="$REPO/docs/sam/scripts/spike1_extract.py"
python3 "$EXTRACT" "$OUTDIR/round$ROUND.log" "$OUTDIR/round$ROUND.undef" --analyze \
  > "$OUTDIR/analysis$ROUND.txt" 2>&1

N=$(wc -l < "$OUTDIR/round$ROUND.undef")
echo "ninja_exit=$RC unique_undefined_symbols=$N"
if [ -f "$BUILDDIR/awesome" ]; then
  echo "binary: $(file -b "$BUILDDIR/awesome")"
  echo "size: $(stat -c %s "$BUILDDIR/awesome") bytes"
fi
