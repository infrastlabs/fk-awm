#!/bin/bash
# 12-spec-shim-loader.sh — make the busted spec suite run on the LGI shim.
#
# WHY THIS EXISTS
# ---------------
# spec/preload.lua used to do `require "lgi"`, which resolved to the
# distribution's lua-lgi (0.9.2, /usr/share/lua/5.3/lgi.lua). The shim's C
# part is compiled into the awesome binary and registered in package.preload by
# luaa.c:1141-1160, so it is unreachable from the interpreter busted runs.
# Part 13's 577/2/10 therefore proved "lib/ migration did not change Lua
# semantics" and said nothing about the shim itself.
#
# THIS SCRIPT
# -----------
# Compiles lib/vendor/lgishim_*.c into ONE Lua-loadable module, then hardlinks
# that single .so under the seven module names lgishim.lua requires
# (lgishim_pango / lgishim_pangocairo / lgishim_cairo / lgishim_glib /
# lgishim_pixbuf / lgishim_gobject / lgishim_gio). Hardlinks, not copies:
# dlopen dedupes by inode, so the seven requires all land in one mapping and
# the cross-module helpers (lgishim_cairo.c -> lgishim_pango_check_layout,
# lgishim_gio.c -> lgishim_gobject_check_closure) resolve. One .so also means
# no duplicate copies of the static cairo/pango symbol tables.
#
# It links the same dynamic glib/gio/cairo/pango/pangocairo/gdk-pixbuf the
# container ships, NOT the static archives the awesome binary is built from.
# The shim is designed to compile against exactly those headers (see the NOTE
# ON THE CONTAINER'S CAIRO BUILD block at the top of lgishim_cairo.c), so a
# dynamic link is faithful and needs no ninja and no touch of
# /_ext/tmp/awe-build-release.
#
# spec/preload.lua is switched over through LGI_SHIM_CDIR. Unset it (or omit
# this step) and the suite falls back to lua-lgi, which is the historic
# behaviour -- the loader is behaviour-preserving when the shim is absent.
#
# Usage (inside builder-awe):
#   bash /_ext/home/headless/xm-zs01/fk-awesomewm/docs/sam/scripts/12-spec-shim-loader.sh
#   bash .../12-spec-shim-loader.sh --no-build     # reuse the existing .so
#   OUT_FMT=tap bash .../12-spec-shim-loader.sh    # per-test TAP lines for diffing
#
# Note: the default output handler only prints one summary line, so use
# OUT_FMT=tap when you want to diff individual test names against a run of
# `busted-5.3 --output=TAP` (the historic lua-lgi baseline).
#
# Outputs:
#   /_ext/tmp/lgishim-spec/lgishim_{pango,pangocairo,cairo,glib,pixbuf,gobject,gio}.so
#   /_ext/tmp/spec-run-shim.txt
set -u

REPO=${REPO:-/_ext/home/headless/xm-zs01/fk-awesomewm}
SHIM_DIR=${SHIM_DIR:-/_ext/tmp/lgishim-spec}
OUT=${OUT:-/_ext/tmp/spec-run-shim.txt}
MODULES=(pango pangocairo cairo glib pixbuf gobject gio)
SRC=(pango cairo glib pixbuf gobject gio)

if [ "${1:-}" = "--no-build" ]; then
    echo "--- skipping build, reusing $SHIM_DIR ---"
else
    mkdir -p "$SHIM_DIR"
    cd "$REPO" || exit 1

    CF=$(pkg-config --cflags glib-2.0 gio-2.0 gio-unix-2.0 cairo pangocairo gdk-pixbuf-2.0)
    LF=$(pkg-config --libs glib-2.0 gio-2.0 gio-unix-2.0 cairo pangocairo gdk-pixbuf-2.0)
    # -I/usr/include/lua5.3 : the interpreter busted runs (/usr/bin/lua5.3,
    # linked to /usr/lib/liblua-5.3.so.0), not /usr/local/include/lua.h, which
    # is the LUA_ROOT=/usr/local/ build linked into the static awesome binary.
    gcc -shared -fPIC -O2 -std=gnu99 -fno-strict-aliasing \
        -Wall -Wno-unused-parameter -Wmissing-prototypes \
        -I/usr/include/lua5.3 $CF \
        $(for s in "${SRC[@]}"; do echo "lib/vendor/lgishim_$s.c"; done) \
        $LF \
        -o "$SHIM_DIR/lgishim_pango.so" || { echo "gcc failed" >&2; exit 1; }

    for m in "${MODULES[@]}"; do
        [ "$m" = pango ] && continue
        ln -f "$SHIM_DIR/lgishim_pango.so" "$SHIM_DIR/lgishim_$m.so"
    done
    echo "--- built $(ls -1 "$SHIM_DIR" | wc -l) module names in $SHIM_DIR ---"
fi

# lgi must stay resolvable so that the shim's own fallback branch
# (lib/vendor/lgishim.lua:46-48) can still reach lua-lgi when the .so is gone.
export LGI_SHIM_CDIR="$SHIM_DIR"
cd "$REPO" || exit 1

# Gate: fail loudly if the shim did not actually take effect. Without this the
# run would silently fall back to lua-lgi, look green, and prove nothing.
BACKEND=$(LUA_CPATH="$LGI_SHIM_CDIR/?.so;;" LUA_PATH="lib/?.lua;lib/?/init.lua;;" \
    lua5.3 -e '
        package.cpath = os.getenv("LGI_SHIM_CDIR") .. "/?.so;" .. package.cpath
        local lgi = require("vendor.lgishim")
        print(lgi.shim and "lgishim" or "lua-lgi")' 2>/dev/null)
echo "--- LGI backend: $BACKEND ---"
[ "$BACKEND" = "lgishim" ] || { echo "shim not loaded -- aborting" >&2; exit 2; }

OUT_ARGS=
[ "${OUT_FMT:-}" = "tap" ] && OUT_ARGS="--output=TAP"

# Busted's filesystem_spec.lua chmods the fixtures it tests and restores them
# in the same spec block. The two known root-run failures throw before the
# restore line, so the permissions leak past the run and poison the next one:
# `git status` reports spec/gears/filesystem_tests/x/NoRead as modified, and a
# `git checkout` cannot even read it. Snapshot what we found and give it back
# no matter how busted exits.
FIXTURE_PERMS=
for f in spec/gears/filesystem_tests/y spec/gears/filesystem_tests/x/NoRead; do
    perm=$(stat -c %a "$f" 2>/dev/null) || perm=$([ -d "$f" ] && echo 777 || echo 666)
    FIXTURE_PERMS="$FIXTURE_PERMS $perm:$f"
done
restore_fixtures() {
    for entry in $FIXTURE_PERMS; do
        chmod "${entry%%:*}" "${entry#*:}" 2>/dev/null || true
    done
}
# Pango font operations (beautiful.init -> set_font -> load_font) block
# indefinitely when DISPLAY is unset: X11 connection times out silently.
# Default to :97 unless the caller overrides. If the caller already has a live
# server where DISPLAY points, leave it alone and just reuse it; only when
# nothing is listening do we start one of our own.
DISPLAY=${DISPLAY:-:97}
DISP_NO=${DISPLAY#:}
DISP_XPID=
DISP_OWNED=
xserver_live() {
    # pgrep -x is not portable: busybox' pgrep has no -x and returns an empty
    # list silently instead of erroring, so a second X server would be started
    # on a display that is already in use. Scan /proc directly and match on
    # argv[0] rather than the whole cmdline, so a shell that merely quotes
    # "Xvnc" later in its argv is not mistaken for a server.
    local f cmd arg0
    for f in /proc/[0-9]*/cmdline; do
        cmd=$(tr '\0' ' ' < "$f" 2>/dev/null) || continue
        arg0=${cmd%% *}
        case "$arg0" in
            */Xvnc|Xvnc|*/Xorg|Xorg) ;;
            *) continue ;;
        esac
        case " $cmd " in
            *" $DISPLAY "*) return 0 ;;
        esac
    done
    return 1
}
cleanup_display() {
    [ -z "$DISP_XPID" ] || kill -9 "$DISP_XPID" 2>/dev/null
    # Only remove the lock/socket we created. If we reused a server the caller
    # already had on $DISPLAY, deleting its lock/socket under it is worse than
    # leaking them: Xvnc treats a vanished lock as a stale/foreign state and
    # the server keeps running with no lock protecting its display.
    [ -z "$DISP_OWNED" ] && return 0
    rm -f "/tmp/.X$DISP_NO-lock" "/tmp/.X11-unix/X$DISP_NO"
}
if ! xserver_live; then
    XBN=$(command -v Xvnc 2>/dev/null) || XBN=/usr/local/static/tigervnc/bin/Xvnc
    [ -x "$XBN" ] || XBN=/usr/bin/Xvnc
    if [ -x "$XBN" ]; then
        # Xvnc parameter notes, each load-bearing:
        #   -ac             without it the server refuses every client of
        #                   $DISPLAY with "authorization required", so Pango
        #                   would hang for the full 300s timeout anyway
        #   -localhost yes  bind 127.0.0.1 only, so the VNC listener on
        #                   5900+DISP_NO is not reachable outside the sandbox.
        #                   -nolisten tcp is NOT a substitute: it makes a
        #                   second Xvnc believe the display is already active
        #                   and refuse to start.
        #   -geometry/-depth  Xvnc has no Xvfb-style `-screen 0 WxHxD`
        # A stale /tmp/.X$N-lock makes Xvnc abort with "Server is already
        # active for display" - it must go, even from a much earlier run.
        rm -f "/tmp/.X$DISP_NO-lock" "/tmp/.X11-unix/X$DISP_NO"
        nohup "$XBN" -ac -localhost yes "$DISPLAY" \
            -geometry 1280x800 -depth 24 >/dev/null 2>&1 &
        DISP_XPID=$!
        for _ in $(seq 1 30); do
            kill -0 "$DISP_XPID" 2>/dev/null || break
            [ -e "/tmp/.X11-unix/X$DISP_NO" ] && break
            sleep 1
        done
        if [ -e "/tmp/.X11-unix/X$DISP_NO" ]; then
            DISP_OWNED=1
            echo "--- started Xvnc on $DISPLAY (pid $DISP_XPID) ---"
        else
            echo "WARN: Xvnc did not come up on $DISPLAY; Pango font loading may hang until the 300s timeout" >&2
        fi
    else
        echo "WARN: no X server on $DISPLAY and no Xvnc available; Pango font loading may hang until the 300s timeout" >&2
    fi
fi
export DISPLAY
trap 'restore_fixtures; cleanup_display' EXIT

echo "--- running busted-5.3 with LGI_SHIM_CDIR=$LGI_SHIM_CDIR $OUT_ARGS ---"
START=$(date +%s)
timeout -s KILL 300 busted-5.3 $OUT_ARGS >"$OUT" 2>&1
RC=$?
END=$(date +%s)

{
    echo "=== busted-5.3 exit code: $RC ==="
    echo "=== elapsed: $((END - START))s ==="
    if [ "${OUT_FMT:-}" = "tap" ]; then
        echo "=== plan:  $(grep '^1\.\.' "$OUT") ==="
        echo "=== ok:    $(grep -c '^ok ' "$OUT") ==="
        echo "=== not ok:$(grep -c '^not ok ' "$OUT") ==="
    else
        echo "=== summary ==="
        grep -E "successes / .* failures" "$OUT" | tail -1
    fi
}
echo "--- full output: $OUT ---"
