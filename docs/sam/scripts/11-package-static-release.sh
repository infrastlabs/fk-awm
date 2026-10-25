#!/bin/bash
# 11-package-static-release.sh —— Phase 2 / Part 12 出可发布包（分步可跑）
#
# 分 7 步，每步可单独跑、可重跑、可断点续：
#   bash 11-package-static-release.sh 1     # 只跑第 1 步
#   bash 11-package-static-release.sh all   # 顺序跑完 7 步（默认）
#
#   1 configure   cmake 配置（-G Ninja + 全静态 linker flags + prefix）
#   2 build       ninja 编译 + 静态性强制校验
#   3 checkpaths  校验 config.h 编译期路径 == prefix（必须在 UPX 之前）
#   4 upx         UPX 7 轮阶梯压缩，取最小
#   5 install     复用 CMake install 规则装到 prefix + 校验安装树
#   6 smoke       UPX 后实跑安装产物（Xvnc + 默认 rc.lua）
#   7 package     写 README + tar.gz + sha256 + 解包校验
#
# 产物：
#   /_ext/tmp/awe-package/awm-static-musl-<yymmdd-HHmmss>-v<ver>-<sha7>.tar.gz
#   时间戳为出包当时的东八区时间，每次运行现取
#   ver 默认取 git describe（如 4.3-1849，即 v4.3 tag 后的提交数）
#   包内即全路径 usr/local/static/awm/...，所以 tar -C / 一步装好。
#
# 包内布局（标准 make install 布局，对标官方 awesome / Debian / Arch 包）：
#   usr/local/static/awm/
#     bin/awesome                  全静态 + UPX 压缩
#     bin/awesome-client
#     etc/xdg/awesome/rc.lua       <- AWESOME_DEFAULT_CONF
#     share/awesome/lib            <- AWESOME_LUA_LIB_PATH
#     share/awesome/icons          <- AWESOME_ICON_PATH
#     share/awesome/themes         <- AWESOME_THEMES_PATH
#     share/xsessions/awesome.desktop
#     share/doc/awesome/README.txt
#
# 关键：**不需要 run.sh，不需要 LUA_PATH/LUA_CPATH**。路径在 config.h 里是编译期
#      常量，prefix 落在 /usr/local/static/awm 就自动对得上。
#
# 安装：tar -xzf <pkg>.tar.gz -C /        # prefix 已写死，别改位置
# 启动：/usr/local/static/awm/bin/awesome
#
# 在哪跑：容器内（宿主机直接跑也行，会自动 docker exec 进 builder-awe）
#   docker exec builder-awe bash <repo>/docs/sam/scripts/11-package-static-release.sh 1
#
# ------------------------------------------------------------
# 运行环境要求
# ------------------------------------------------------------
# 必需：容器内 /etc/alpine-release 存在（busybox ash + musl + gcc）。宿主机直接跑
#      会自动转发进 builder-awe。
# 必需：bash git cmake ninja gcc g++ make file tar awk + lua5.3 / lua-libs
# 必需：静态库（.a，不是 .so）。Alpine 装 -static-dev 包，装 -dev 不够：
#        apk add --virtual awe-static-deps \
#            lua-static-dev libtool-static-dev libX11-static-dev \
#            libXcursor-static-dev libXrandr-static-dev libXi-static-dev \
#            libXinerama-static-dev libxcb-static-dev libXext-static-dev \
#            xcb-util-static-dev xcb-util-image-static-dev \
#            xcb-util-keysyms-static-dev xcb-util-wm-static-dev \
#            xcb-util-renderutil-static-dev xcb-util-xrm-static-dev \
#            wayland-static-dev wayland-protocols-static-dev \
#            libudev-static-dev xdg-basedir-static-dev \
#            harfbuzz-static-dev freetype-static-dev zlib-static-dev \
#            png-static-dev pixman-static-dev cairo-static-dev \
#            pangocairo-static-dev harfbuzz-icu-dev icu-static-dev \
#            dbus-static-dev glib-static-dev
#      缺哪个脚本会列出来。静态性真正的校验在 step 2（file 必须是
#      statically linked），但那是失败太晚。
# 可选：upx     —— apk add upx。缺则 step 4 自动不压缩，出包照常
# 可选：Xvnc    —— apk add tigervnc（或静态版放 /usr/local/static/tigervnc/bin）。缺则
 #      step 6 直接报错退出（SMOKE=0 可跳过冒烟）。注意 Xvnc 还额外要求
 #      /usr/share/X11/xkb + xkbcomp，否则起不来（XKB rules file 找不到 →
 #      Failed to activate virtual core keyboard）；Xvfb 没有这个要求
# 换环境到 glibc / musl-gcc 以外的工具链要改 3 处：CC/CXX、-DCMAKE_EXE_LINKER_FLAGS、
#       工具可用性判断（busybox timeout 不支持 -k，本脚本按 busybox 写法）
#
# 脚本不装任何依赖。缺什么直接退出并打印怎么装 / 改哪个变量。
#
# 可调参数（环境变量）：
#   REPO             源码目录，默认 /_ext/home/headless/xm-zs01/fk-awesomewm
#   PREFIX           安装 prefix，默认 /usr/local/static/awm（带不带尾斜杠都行）
#   BUILD_DIR        构建目录，默认 /_ext/tmp/awe-build-release
#   OUT_DIR          出包目录，默认 /_ext/tmp/awe-package
#   VERSION          包名版本号，默认取 git describe（如 4.3-1849），可用 VERSION 覆盖
#   PKG              包名前缀，默认 awm-static-musl
#   UPX              1/0，默认 1。UPX=0 跳过 step 4（不装 upx 时用）
#   UPX_ROUNDS       UPX 轮数，默认 7，最大 7（对应下面 7 档参数）
#   UPX_ROUND_TIMEOUT 单轮超时秒数，默认 180（--ultra-rep 很吃时间，别卡死）
#   SMOKE            1/0，默认 0（出包默认跳过冒烟；要冒烟显式 SMOKE=1）
#   DISPLAY_PORT     Xvnc 显示号，默认 99（VNC 监听口 = 5900 + 该值）
#
# UPX 7 轮阶梯（UPX 拒绝二次压缩自己的产物，所以是 7 种策略各从原样副本试一次，
# 取最小的那档，不是嵌套压缩 7 次）：
#   round 1  -1            2  -3            3  -5            4  -7
#   round 5  -9            6  -9 --lzma     7  --best --lzma --ultra-rep
#   实测 9,597,256 bytes：round1 48.4%↓ / round5 54.8%↓ / round6 64.6%↓。
#   -9 --lzma 是性价比最高的一档，--ultra-rep 只多压一点点但慢得多。
#
# 坑（实测）：
#   1. **全静态的开关是 -DCMAKE_EXE_LINKER_FLAGS，不是 AWESOME_STATIC_DEPS。**
#      AWESOME_STATIC_DEPS=ON 只是把额外传递依赖的 .a 名拼进链接行
#      （CMakeLists.txt:104-122）。不传 linker flags 就编出
#      "ELF 64-bit LSB pie executable, dynamically linked"（9.6MB 静态 → 13MB 动态），
#      肉眼看不出来，只有 file -b / ldd 才报。它当初是手动加在 impl-plan.md:115 的，
#      从没进过任何脚本，所以首次干净构建就翻车。step 2 因此强制校验静态性。
#   2. 必须 -G Ninja。**全新 build dir 不指定生成器就是 Unix Makefiles**，
#      后面 ninja -C 报 "open build.ninja: No such file or directory"。
#   3. **不能 cp -a 已有 build dir 再 cmake**：CMake 拒绝
#      "CMakeCache.txt directory ... is different than the directory where it was created"
#   4. AWESOME_DATA_PATH / DOC_PATH / MAN_PATH / XSESSION_PATH / SYSCONFDIR /
#      XDG_CONFIG_DIR 全是 CACHE PATH 变量（awesomeConfig.cmake:244-280）。
#      **只改 -DCMAKE_INSTALL_PREFIX 不会带它们走**，必须逐个显式传。
#   5. 必须 AWESOME_IGNORE_LGI=1（build-utils/lgi-check.c:57 读它；容器无 lgi
#      runtime，否则 lgi-check 目标直接失败）
#   6. UPX 后 strings/grep 抓不到路径字符串，所以 step 3 路径校验在 step 4 之前
#   7. step 5 前 rm -rf $PREFIX 清场，保证包内只有本次 install 的产物
# ============================================================
set -u

# 宿主机跑就自动转发进容器
if [ ! -f /etc/alpine-release ]; then
    exec docker exec builder-awe bash "$(readlink -f "${BASH_SOURCE[0]}")" "$@"
fi

export AWESOME_IGNORE_LGI=1

REPO=${REPO:-/_ext/home/headless/xm-zs01/fk-awesomewm}
PREFIX=${PREFIX:-/usr/local/static/awm}
PREFIX=${PREFIX%/}          # 容忍 /usr/local/static/awm/ 这种带尾斜杠的写法
PREFIX_REL=${PREFIX#/}      # 去开头斜杠 = 包内相对路径（解包到 / 时还原成绝对路径）
BUILD_DIR=${BUILD_DIR:-/_ext/tmp/awe-build-release}
OUT_DIR=${OUT_DIR:-/_ext/tmp/awe-package}
VERSION=${VERSION:-}
# 版本号默认 = git describe（v4.3-1849-g3fdacff2 → 4.3-1849）：带子版本号（tag 后提交数），
# 去掉 v 前缀和 -g<sha> 尾巴（包名后面已有单独 SHA 段）。可被 VERSION 环境变量覆盖。
if [ -z "$VERSION" ]; then
    # --match 'v*' 必须显式给。仓里还有 t01.1 / t02.2 这类临时 tag，git describe 的
    # 默认 default 模式按版本序取「最近的祖先 tag」，t02.2 排在 v4.3 之后，不加
    # --match 就算出 02.2-8 而不是 4.3-1843。对标 awm-package.yml 的
    # push: tags: ['v*'] —— v 系列才是发布线，临时 tag 不参与版本号。
    DESCRIBE=$(git -C "$REPO" describe --tags --match 'v*' --long 2>/dev/null || echo "v4.3-0")
    VERSION=${DESCRIBE#v}
    VERSION=${VERSION%-g*}
    [ -n "$VERSION" ] || VERSION="4.3-0"
fi
PKG=${PKG:-awm-static-musl}
UPX=${UPX:-1}
UPX_ROUNDS=${UPX_ROUNDS:-7}
UPX_ROUND_TIMEOUT=${UPX_ROUND_TIMEOUT:-180}
SMOKE=${SMOKE:-0}
DISPLAY_PORT=${DISPLAY_PORT:-99}
# busybox 的 timeout 不支持 -k，只有 -s SIG；GNU coreutils 两种都收
KILLT="timeout -s KILL"

SHA=$(git -C "$REPO" rev-parse --short=7 HEAD 2>/dev/null || echo nogit)
mkdir -p "$BUILD_DIR" "$OUT_DIR"
# 日期时间每次现取，东八区（容器默认 UTC；POSIX TZ 符号与直觉相反，UTC-8 = UTC+8）
TS=$(TZ=UTC-8 date +%y%m%d-%H%M%S)
PKGNAME="${PKG}-${TS}-v${VERSION}-${SHA}"
LOGF="$OUT_DIR/$PKGNAME.log"
SMOKE_LOG="$OUT_DIR/$PKGNAME.smoke.log"
: > "$LOGF"

# 进度同时进 stdout 和 LOGF：长构建被 kill 时日志也不丢行
say() { echo "==> $*"; echo "==> $*" >>"$LOGF"; }
sayf() { echo "$*"; echo "$*" >>"$LOGF"; }
fail() {
    echo "FATAL: $*"
    echo "FATAL: $*" >>"$LOGF"
    tail -n 25 "$LOGF" | sed 's/^/    /'
    exit 1
}
# 跑一步命令：输出进 LOGF，失败就带尾部退出
run() {
    local label="$1"; shift
    "$@" >>"$LOGF" 2>&1
    local rc=$?
    [ "$rc" -eq 0 ] || { tail -n 25 "$LOGF" | sed 's/^/    /'; fail "$label 退出码 $rc"; }
}

# 兜底清理：脚本无论正常退出还是 fail，都别留下 awesome 进程
# （awesome 忽略 SIGTERM，残留会一直刷日志；实测把容器刷到 OOM）
trap 'pkill -9 -x awesome 2>/dev/null' EXIT

# ---------- 依赖预检 ----------
# 不装任何东西。缺什么就打出怎么装 / 改哪个变量，然后退出。
preflight() {
    local missing=""
    local cmd
    for cmd in bash git cmake ninja gcc g++ make file tar awk lua5.3; do
        command -v "$cmd" >/dev/null 2>&1 || missing="$missing $cmd"
    done
    if [ -n "$missing" ]; then
        sayf "缺工具:$missing"
        sayf "Alpine: apk add git cmake ninja build-base file tar lua-libs"
        sayf "（必须能跑起来 lua5.3：/usr/bin/lua5.3 与 lua-libs）"
        fail "依赖不全，退出"
    fi
    if pkg-config --exists pangocairo 2>/dev/null; then
        pkg-config --modversion pangocairo >/dev/null
    else
        sayf "pkg-config 找不到 pangocairo：apk add pangocairo-static-dev pkgconf"
        fail "pangocairo 没装，退出"
    fi
    # 静态库必须是 .a。装成 -dev（只有 .so）时 ninja 报 "cannot find -lpango"，
    # 那已经失败太晚，在这里先查一遍
    local libs="pangocairo cairo glib-2.0 gio-2.0 dbus-1 lua"
    local want missing_static=""
    for want in $libs; do
        for lib in $(pkg-config --libs "$want" 2>/dev/null); do
            case "$lib" in -l*)
                lib=${lib#-l}
                [ -n "$(find /usr/lib /usr/local/lib -maxdepth 2 -name "lib${lib}.a" 2>/dev/null | head -1)" ] \
                    || missing_static="$missing_static ${lib}.a"
                ;;
            esac
        done
    done
    if [ -n "$missing_static" ]; then
        sayf "缺静态库:$missing_static"
        sayf "Alpine 装 -static-dev 包，装 -dev 不够（只有 .so）。清单见脚本头「运行环境要求」"
        fail "静态依赖不全，退出"
    fi
    if [ "$SMOKE" = "1" ] && ! command -v Xvnc >/dev/null 2>&1 \
        && [ ! -x /usr/local/static/tigervnc/bin/Xvnc ] \
        && [ ! -x /usr/bin/Xvnc ]; then
        sayf "缺 Xvnc（step 6 冒烟要用）：apk add tigervnc，或静态版放 /usr/local/static/tigervnc/bin/Xvnc"
        sayf "另需 /usr/share/X11/xkb + xkbcomp，缺则 Xvnc 起来就挂（XKB rules file）"
        sayf "或  SMOKE=0 跳过冒烟"
        fail "Xvnc 没装，退出"
    fi
    if [ "$UPX" = "1" ] && ! command -v upx >/dev/null 2>&1; then
        sayf "缺 upx：apk add upx    或  UPX=0 出未压缩包"
        sayf "（step 4 会自动跳过压缩，不影响出包）"
    fi
    sayf "依赖齐全"
}

preflight

# ---------- step 1: configure ----------
step_configure() {
    say "1/7 cmake configure"
    # lua 解释器：03 编的 /usr/local/bin/lua 是静态版不能 dlopen lgi，
    # 全新 build 目录会触发 docs 生成（require lgi）→ 必须用 apk 动态版
    run "cmake configure" cmake \
        -S "$REPO" -B "$BUILD_DIR" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_EXE_LINKER_FLAGS="-static -Wl,--as-needed -Wl,--strip-all" \
        -DAWESOME_STATIC_DEPS=ON -DAWESOME_LGISHIM=ON \
        -DLUA_EXECUTABLE=/usr/bin/lua5.3 \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DSYSCONFDIR="$PREFIX/etc" \
        -DXDG_CONFIG_DIR="$PREFIX/etc/xdg" \
        -DAWESOME_DATA_PATH="$PREFIX/share/awesome" \
        -DAWESOME_DOC_PATH="$PREFIX/share/doc/awesome" \
        -DAWESOME_MAN_PATH="$PREFIX/share/man" \
        -DAWESOME_XSESSION_PATH="$PREFIX/share/xsessions"
    [ -f "$BUILD_DIR/build.ninja" ] || fail "build.ninja 没生成（生成器不是 Ninja？）"
}

# ---------- step 2: build ----------
step_build() {
    say "2/7 ninja awesome"
    run "ninja awesome" ninja -C "$BUILD_DIR" awesome
    [ -f "$BUILD_DIR/awesome" ] || fail "没产出 $BUILD_DIR/awesome"
    # 全静态不变式：这是静态发布包，不是静态就不出包
    FILELINE=$(file -b "$BUILD_DIR/awesome")
    case "$FILELINE" in
        *"statically linked"*) say "    静态校验 ok：$FILELINE" ;;
        *) fail '静态校验 FAIL（少了 -DCMAKE_EXE_LINKER_FLAGS="-static ..."？）：'"$FILELINE" ;;
    esac
}

# ---------- step 3: checkpaths ----------
step_checkpaths() {
    say "3/7 校验 config.h 编译期路径"
    CFG="$BUILD_DIR/config.h"
    [ -f "$CFG" ] || fail "没有 $CFG（先跑 step 2）"
    grep -E 'define AWESOME_(LUA_LIB|THEMES|ICON|DEFAULT_CONF)' "$CFG" | sed 's/^/    /'
    for want in \
        "$PREFIX/share/awesome/lib" \
        "$PREFIX/share/awesome/themes" \
        "$PREFIX/share/awesome/icons" \
        "$PREFIX/etc/xdg/awesome/rc.lua"
    do
        grep -q "$want" "$CFG" || fail "config.h 里缺 $want —— prefix 没写对，别往下跑"
        sayf "    ok   $want"
    done
}

# ---------- step 4: upx ----------
step_upx() {
    BINARY="$BUILD_DIR/awesome"
    [ -f "$BINARY" ] || fail "没有 $BINARY（先跑 step 2）"
    PRISTINE="$BUILD_DIR/awesome.pristine"
    UPTAG="$BUILD_DIR/upx.done"

    if [ -f "$PRISTINE" ] && [ -f "$UPTAG" ] && [ ! "$BINARY" -nt "$PRISTINE" ]; then
        # 状态 1：UPX 已完成且二进制没重编过 → 跳过（避免每次 all 重跑 7 轮）
        say "4/7 跳过（UPX 已完成：$(awk '/^args=/{print}' "$UPTAG")）"
        return 0
    fi
    if [ ! -f "$PRISTINE" ] && [ -f "$UPTAG" ]; then
        # 状态 2：原样副本被删但 UPX 已完成 → 别再拿压缩产物二次压缩
        say "4/7 跳过（PRISTINE 缺失但 UPX 已完成，不重复压缩）"
        return 0
    fi
    # 状态 3：全新 / 二进制重建过（BINARY 比 PRISTINE 新）→ 刷新原样副本，重新做 UPX
    cp -a "$BINARY" "$PRISTINE"
    : > "$UPTAG"
    echo "pristine=$(stat -c %s "$PRISTINE")" >>"$UPTAG"
    say "4/7 原样副本已刷新（$(stat -c %s "$PRISTINE") bytes，重建后重压）"

    BEFORE=$(stat -c %s "$PRISTINE")
    UPX_STATE=""
    if [ "$UPX" != "1" ]; then
        UPX_STATE="跳过（UPX=0）"
    elif ! command -v upx >/dev/null 2>&1; then
        UPX_STATE="跳过（容器没装 upx）"
    fi

    if [ -z "$UPX_STATE" ]; then
        UPX_LADDER=(-1 -3 -5 -7 -9 "-9 --lzma" "--best --lzma --ultra-rep")
        N=${#UPX_LADDER[@]}
        [ "$UPX_ROUNDS" -lt "$N" ] && N=$UPX_ROUNDS

        BEST=$BEFORE; BEST_CAND=""
        say "4/7 UPX $N 轮阶梯压缩（原样 $BEFORE bytes）"
        i=0
        while [ "$i" -lt "$N" ]; do
            i=$((i+1))
            args=${UPX_LADDER[$((i-1))]}" -q"
            CAND="$BUILD_DIR/awesome.upx-round$i"
            cp -a "$PRISTINE" "$CAND"
            if $KILLT "$UPX_ROUND_TIMEOUT" upx $args "$CAND" >>"$LOGF" 2>&1; then
                SZ=$(stat -c %s "$CAND")
                PCT=$(awk -v s="$SZ" -v b="$BEFORE" 'BEGIN{printf "%.1f", 100*(1-s/b)}')
                if [ "$SZ" -lt "$BEST" ]; then
                    BEST=$SZ; BEST_ARGS="$args"; BEST_CAND="$CAND"
                    say "    round $i/$N [${args// -q/}] -> $SZ bytes ($PCT%↓) ★最小"
                else
                    say "    round $i/$N [${args// -q/}] -> $SZ bytes ($PCT%↓) 未变小"
                fi
            else
                say "    round $i/$N [${args// -q/}] -> 失败/超时(${UPX_ROUND_TIMEOUT}s)，跳过"
            fi
            [ "$CAND" = "$BEST_CAND" ] || rm -f "$CAND"
        done

        if [ -n "$BEST_CAND" ]; then
            cp -a "$BEST_CAND" "$BINARY"
            rm -f "$BEST_CAND"
            UPX_STATE="${BEST_ARGS// -q/}"
        else
            UPX_STATE="未压缩（所有轮次都失败/没变小）"
        fi
        echo "args=$UPX_STATE size=$(stat -c %s "$BINARY")" >>"$UPTAG"
    else
        say "4/7 跳过 UPX：$UPX_STATE"
    fi

    AFTER=$(stat -c %s "$BINARY")
    PCT=$(awk -v s="$AFTER" -v b="$BEFORE" 'BEGIN{printf "%.1f", 100*(1-s/b)}')
    say "    $BEFORE -> $AFTER bytes ($PCT%↓)  upx=[$UPX_STATE]"
    # 压缩产物的 mtime 对齐原样副本：否则下次跑 step 4 会误判"二进制重建过"，
    # 拿压缩产物当原样副本二次压缩（UPX 拒绝二次压缩，反而把 BINARY 弄丢）
    touch -r "$PRISTINE" "$BINARY"
    # 压完还要确认还是静态 + 还能跑
    FILELINE=$(file -b "$BINARY")
    case "$FILELINE" in
        *"statically linked"*) say "    UPX 后静态校验 ok" ;;
        *) fail "UPX 后二进制不再是静态：$FILELINE" ;;
    esac
    if ! "$BINARY" --version >>"$LOGF" 2>&1; then
        fail "UPX 后 binary 跑不起来（--version 失败）"
    fi
    say "    UPX 后 --version 正常"
}

# ---------- step 5: install ----------
step_install() {
    case "$PREFIX" in
        /|/usr|/usr/local|/etc|/var|/bin|/lib|/home|/root)
            fail "PREFIX=$PREFIX 不安全，step 5 会 rm -rf 它"
            ;;
        *) [ -n "$PREFIX" ] && [ "${PREFIX:0:1}" = "/" ] || fail "PREFIX 必须是绝对路径" ;;
    esac
    say "5/7 cmake --install（复用 CMakeLists 已有 install 规则）"
    rm -rf "$PREFIX"; mkdir -p "$PREFIX"
    run "cmake --install" cmake --install "$BUILD_DIR"

    MISSING=0
    for p in \
        "$PREFIX/bin/awesome" \
        "$PREFIX/bin/awesome-client" \
        "$PREFIX/etc/xdg/awesome/rc.lua" \
        "$PREFIX/share/awesome/lib" \
        "$PREFIX/share/awesome/themes" \
        "$PREFIX/share/awesome/icons" \
        "$PREFIX/share/xsessions/awesome.desktop"
    do
        if [ -e "$p" ]; then sayf "    ok   $p"
        else sayf "    缺失 $p"; MISSING=1; fi
    done
    [ "$MISSING" -eq 0 ] || fail "安装树不完整"
    say "    安装树 $(du -sh "$PREFIX" | awk '{print $1}')"
    say "    ldd : $(ldd "$PREFIX/bin/awesome" 2>&1 | head -1)"
    say "    file: $(file -b "$PREFIX/bin/awesome" | cut -c1-100)"
}

# ---------- step 6: smoke ----------
step_smoke() {
    BIN="$PREFIX/bin/awesome"
    [ -x "$BIN" ] || fail "没有 $BIN（先跑 step 5）"
    : > "$SMOKE_LOG"
    say "6/7 冒烟（实跑安装产物）"

    pkill -9 -f "static/awm/bin/awesome" 2>/dev/null || true
    pkill -9 -f "Xvnc :$DISPLAY_PORT" 2>/dev/null || true

    if "$BIN" --version >>"$SMOKE_LOG" 2>&1; then
        say "    T1 ok --version"
    else
        tail -n 20 "$SMOKE_LOG" | sed 's/^/    /'
        fail "T1 失败"
    fi

    XB=$(command -v Xvnc 2>/dev/null) || XB=/usr/local/static/tigervnc/bin/Xvnc
    [ -x "$XB" ] || XB=/usr/bin/Xvnc
    [ -x "$XB" ] || fail "Xvnc 没装：apk add tigervnc   或  SMOKE=0 跳过冒烟"
    # Xvnc 参数注意项，每条都不能省：
    #   -ac             否则 X server 拒绝其他客户端连 :$DISPLAY_PORT
    #                   （报 authorization required），awesome 起不来
    #   -localhost yes  只绑 127.0.0.1，VNC 监听口 5900+N 不对外暴露。
    #                   别图省事改 -nolisten tcp：它会令第二个 Xvnc 误判
    #                   该 display 已 active 而拒绝启动
    #   -geometry/-depth  Xvnc 没有 Xvfb 的 -screen 0 WxHxD 语法
    # 残留的 .X$N-lock 会让 Xvnc 报 Server is already active for display，
    # 残留的 /tmp/.X11-unix/X$N 会让下面的 $BIN 以为连上了而报
    # cannot open display (error 1)，所以两边都先删。
    rm -f "/tmp/.X$DISPLAY_PORT-lock" "/tmp/.X11-unix/X$DISPLAY_PORT"
    "$XB" -ac -localhost yes ":$DISPLAY_PORT" \
        -geometry 1280x800 -depth 24 >/dev/null 2>&1 &
    XPID=$!
    sleep 2

    # busybox 的 timeout 不支持 -k，用 -s KILL；SIGKILL 无法被 awesome 忽略
    DISPLAY=:$DISPLAY_PORT $KILLT 8 "$BIN" >>"$SMOKE_LOG" 2>&1
    T2_RC=$?
    kill -9 "$XPID" 2>/dev/null || true
    rm -f "/tmp/.X$DISPLAY_PORT-lock" "/tmp/.X11-unix/X$DISPLAY_PORT"
    pkill -9 -f "static/awm/bin/awesome" 2>/dev/null || true

    case "$T2_RC" in
        0|124|137|143) T2_OK=1 ;;
        *) T2_OK=0 ;;
    esac
    LSIZE=$(stat -c %s "$SMOKE_LOG" 2>/dev/null || echo 0)
    if [ "$T2_OK" -eq 1 ]; then
        say "    T2 ok 默认 rc.lua 启动并稳定挂满 8s（rc=$T2_RC）— 无需 LUA_PATH/run.sh"
    else
        tail -n 30 "$SMOKE_LOG" | sed 's/^/    /'
        fail "T2 失败 rc=$T2_RC"
    fi
    # T2b：光看退出码是假通过。rc.lua 报错时 awesome 立刻 exit(1)；但要是进程
    # 卡死在别处（比如 pango 的 use-after-free 死循环），8s 到点照样被 SIGKILL、
    # rc=137，冒烟会报"稳定挂满 8s"。所以必须另外确认 rc.lua 真的加载干净。
    ERRN=$(grep -acE 'traceback|attempt to call|bad argument|interval must be|E: awesome' \
        "$SMOKE_LOG" || true)
    say "    冒烟日志 $(numfmt --to=iec "$LSIZE" 2>/dev/null || echo "$LSIZE")，lua/运行时错误行=$ERRN"
    if [ "$ERRN" -gt 0 ]; then
        grep -nE 'traceback|attempt to call|bad argument|interval must be|E: awesome' \
            "$SMOKE_LOG" | head -12 | sed 's/^/      /'
        fail "T2b 失败：rc.lua 没加载干净——T2 的 rc=$T2_RC 只是进程没死，不等于跑起来了"
    fi

    # T2c：T2/T2b 只能证明 rc.lua 加载干净、进程没崩。它们证明不了"某个 draw
    # 分支真的跑过"。wibox.widget.systray:draw 在整个冒烟链路上从未执行过
    # （headless X 无 tray client -> fit 返回 0x0 -> hierarchy.lua:338 空 clip
    # 跳过），而 T2/T2b 照样全绿。必须用执行计数来判，不能用"没报错"来判。
    # 判据见 12-systray-draw-smoke.sh 头部：utd_calls>=1 才说明 systray.lua:75
    # （commit a51ed56b）真跑过。
    #
    # 串行使：T2 的 X server 上面已经 kill 掉了，这里复用同一个 :$DISPLAY_PORT。
    SAYSCRIPT="$(dirname "$0")/12-systray-draw-smoke.sh"
    if [ -x "$SAYSCRIPT" ] || [ -f "$SAYSCRIPT" ]; then
        say "    T2c systray:draw 执行判据（12-systray-draw-smoke.sh）"
        if BIN="$BIN" PREFIX="$PREFIX" DISPLAY_PORT="$DISPLAY_PORT" \
             SECS=5 bash "$SAYSCRIPT" >>"$SMOKE_LOG" 2>&1; then
            say "        ok systray:draw 真跑完，cr:user_to_device_distance 已执行"
        else
            tail -n 20 "$SMOKE_LOG" | sed 's/^/        /'
            fail "T2c 失败：systray:draw 没有真跑（短路跳过，不是执行）"
        fi
    else
        sayf "    T2c 跳过：找不到 12-systray-draw-smoke.sh"
    fi
}

# ---------- step 7: package ----------
step_package() {
    say "7/7 打包"
    BIN_SRC="$BUILD_DIR/awesome"
    [ -f "$BUILD_DIR/upx.done" ] || UPX_STATE="未压缩"
    UPX_STATE=$(awk '/^args=/{sub(/^args=/,""); sub(/ size=.*$/,""); print; f=1} END{if(!f) print "未压缩"}' "$BUILD_DIR/upx.done" 2>/dev/null || echo "未压缩")
    BEFORE=$(awk -F'[ =]' '/pristine=/{print $2}' "$BUILD_DIR/upx.done" 2>/dev/null)
    BEFORE=${BEFORE:-0}
    AFTER=$(stat -c %s "$BIN_SRC")

    DOCDIR="$PREFIX/share/doc/awesome"
    mkdir -p "$DOCDIR"
    cat > "$DOCDIR/README.txt" <<EOF
$PKGNAME  —— awesome 全静态发布包
==================================================================
 版本      $VERSION（git $SHA）
 出包时间  $TS
 构建环境  alpine 3.15 / musl / Release / -G Ninja / AWESOME_STATIC_DEPS=ON
 二进制    $BEFORE -> $AFTER bytes（upx $UPX_STATE，7 轮阶梯取最优）
 动态依赖  无（statically linked）

安装（一步，包内即全路径）
    tar -xzf $PKGNAME.tar.gz -C /

启动
    $PREFIX/bin/awesome

  不需要 run.sh，也不需要 LUA_PATH / LUA_CPATH。下面这些在 config.h 里
  是编译期常量，prefix 对了就自动生效：
      AWESOME_LUA_LIB_PATH  $PREFIX/share/awesome/lib
      AWESOME_THEMES_PATH   $PREFIX/share/awesome/themes
      AWESOME_ICON_PATH     $PREFIX/share/awesome/icons
      AWESOME_DEFAULT_CONF  $PREFIX/etc/xdg/awesome/rc.lua
  把这些路径改到别处会静默失效 —— 要改位置请重编。

前置要求（运行时）
    X server（Xorg / Xvnc） + fontconfig 字体
    可选：dbus-daemon + session bus（否则通知静默降级，不影响窗口管理）

包内布局
    usr/local/static/awm/
      bin/awesome                 UPX 压缩的静态二进制
      bin/awesome-client
      etc/xdg/awesome/rc.lua
      share/awesome/lib           Lua 库（含 vendor/lgishim）
      share/awesome/icons
      share/awesome/themes
      share/xsessions/awesome.desktop

自检
    ldd   $PREFIX/bin/awesome        # Not a valid dynamic program
    $PREFIX/bin/awesome --version
    rm -f /tmp/.X99-lock /tmp/.X11-unix/X99   # 否则 Xvnc 报 already active
    Xvnc -ac -localhost yes :99 -geometry 1280x800 -depth 24 &
    #   -ac 必需，否则拒绝其他客户端（authorization required）
    #   -localhost yes 只绑本机（VNC 口 5900+99=5999）；别用 -nolisten tcp
    DISPLAY=:99 timeout -s KILL 8 $PREFIX/bin/awesome   # busybox 的 timeout 不支持 -k
      # 退出码 124/137 = 被 timeout 杀掉 = 一直正常跑着
      # 退出码 1 = 崩了，看 /tmp 或上面的 stderr

构建环境要求（重编时才需要）
    alpine + musl + gcc，静态库装 -static-dev 包。清单见
    docs/sam/scripts/11-package-static-release.sh 脚本头的「运行环境要求」。
    本脚本不会自己装依赖，缺什么会打印怎么装然后退出。

已知限制
    · dbus 不在运行时环境里时，通知静默降级，窗口管理不受影响
    · 改 prefix 位置必须重编（路径是 config.h 编译期常量），改完包名不变但路径会静默失效

EOF

    TAR_ROOT="$OUT_DIR/$PKGNAME-root"
    rm -rf "$TAR_ROOT"; mkdir -p "$TAR_ROOT/$PREFIX_REL"
    cp -a "$PREFIX/." "$TAR_ROOT/$PREFIX_REL/"
    TARBALL="$OUT_DIR/$PKGNAME.tar.gz"
    run "tar czf" tar -czf "$TARBALL" -C "$TAR_ROOT" usr
    sha256sum "$TARBALL" > "$TARBALL.sha256"

    # 解包校验：包能不能装回来
    XDIR=$(mktemp -d /_ext/tmp/tarball-check.XXXXXX)
    run "tar xzf (校验)" tar -xzf "$TARBALL" -C "$XDIR"
    [ -x "$XDIR/$PREFIX_REL/bin/awesome" ] || fail "包内缺 bin/awesome"
    CNT=$(tar -tzf "$TARBALL" | wc -l)
    rm -rf "$XDIR"

    TB=$(stat -c %s "$TARBALL")
    say "    解包校验通过，包内 $CNT 个条目"
    say "    包内路径（前 12 条）"
    tar -tzf "$TARBALL" | head -12 | sed 's/^/    /'
    PCT=$(awk -v s="$TB" -v b="$BEFORE" 'BEGIN{if(b>0) printf "%.1f", 100*(1-s/b); else print "n/a"}')
    cat <<EOF
==================================================================
 出包完成
   包    : $TARBALL  ($TB bytes)
   sha256: $(cut -d' ' -f1 "$TARBALL.sha256")
   大小  : 二进制 $BEFORE -> $AFTER bytes，包 $TB bytes（相对原二进制 $PCT%↓）
   安装  : tar -xzf $PKGNAME.tar.gz -C /
   启动  : $PREFIX/bin/awesome
   说明  : $DOCDIR/README.txt
==================================================================
EOF
    say "出包完成：$TARBALL"
    chown -R 1000:1000 "$OUT_DIR" 2>/dev/null || true
}

# ---------- 分派 ----------
WANT=${1:-all}
echo "=================================================================="
say " $PKGNAME"
say " prefix=$PREFIX  build=$BUILD_DIR  out=$OUT_DIR"
say " sha=$SHA  ts=$TS  step=$WANT  upx=$UPX($UPX_ROUNDS轮)  smoke=$SMOKE"
echo "=================================================================="
say "==================================================================" >>"$LOGF"

case "$WANT" in
    all)
        step_configure; step_build; step_checkpaths
        step_upx; step_install
        [ "$SMOKE" = "1" ] && step_smoke
        step_package
        ;;
    1) step_configure ;;
    2) step_build ;;
    3) step_checkpaths ;;
    4) step_upx ;;
    5) step_install ;;
    6) step_smoke ;;
    7) step_package ;;
    *) fail "用法：$0 [all|1|2|3|4|5|6|7]";;
esac
say "step $WANT 完成"
