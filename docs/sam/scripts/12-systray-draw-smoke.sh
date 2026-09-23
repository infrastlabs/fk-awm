#!/bin/bash
# 12-systray-draw-smoke.sh — prove that wibox.widget.systray:draw really runs
# under the smoke environment, instead of being silently short-circuited.
#
# ────────────────────────────────────────────────────────────────────────────
# WHY THIS EXISTS
#
# The stock smoke (step 6 of 11-package-static-release.sh) boots the default
# rc.lua and asserts "no lua errors". That is a false green for the systray:
# systray.lua's draw() has never been executed on this box, and the smoke
# cannot tell.
#
# The original hypothesis was that should_display_on() (systray.lua:50)
# short-circuits because `screen.primary` is nil under Xvfb/Xvnc. That is
# FALSE. objects/screen.c:1494 screen_get_primary() falls back to
# globalconf.screens.tab[0] when no RandR 1.3 primary output exists, so
# `screen.primary` is screen[1] and that gate passes.
#
# The real chain has two gates, both environment-dependent:
#
#   GATE 1  lib/wibox/widget/systray.lua:118     (inside fit)
#           if num_entries == 0 then return 0, 0 end
#           With no tray clients registered, awesome.systray() returns 0, so
#           the widget fits to 0x0.
#
#   GATE 2  lib/wibox/hierarchy.lua:338          (inside hierarchy:draw)
#           if not empty_clip(cr) then ... call(widget.draw) ...
#           A 0x0 widget yields an empty cairo clip, so the widget's draw is
#           skipped before it is even invoked.
#
# Real desktops always have tray clients, so GATE 1 does not fire there. But
# it hides everything behind it -- including a genuine bug this smoke found:
#
#   lib/vendor/lgishim_cairo.c:MATRIX_M has no "transform_point" and no
#   "transform_distance". lib/gears/matrix.lua:185 (transform_rectangle) and
#   lib/wibox/drawable.lua:173 both call self:transform_point(...). Trace:
#   systray.lua:62 -> base.rect_to_device_geometry -> cr.matrix
#   (ctx_get_matrix, a C_MATRIX userdata) -> gears/matrix.lua:185 ->
#   self:transform_point -> nil. systray:draw cannot reach line 75 on ANY
#   display until that is fixed upstream. The probe carries a test-only
#   fallback and reports matrix_shim_used=true in the verdict whenever it was
#   needed, so a green run cannot be mistaken for a clean desktop.
#
# ────────────────────────────────────────────────────────────────────────────
# THE DISCRIMINATOR
#
# "No error" is not a pass condition. The verdict asserts on execution counts
# instead:
#
#   draw_entered >= 1   draw() was actually invoked (not just registered)
#   utd_calls   >= 1    cr:user_to_device_distance ran (commit a51ed56b)
#   draw_errors == 0    it ran to completion
#   utd_bad     == 0    the API returned two real numbers
#
# utd_calls is the load-bearing one. systray.lua:75 is the ONLY caller of
# cr:user_to_device_distance in all of lib/ (verified by grep), and it sits
# strictly AFTER every guard in draw(). So utd_calls >= 1 proves the entire
# body executed. If draw_entered > 0 but utd_calls == 0, a guard was skipped --
# the smoke fails on exactly that distinction.
#
# ────────────────────────────────────────────────────────────────────────────
# USAGE
#
#   bash 12-systray-draw-smoke.sh
#
# Override anything via env:
#   BIN=        awesome binary            (default /usr/local/static/awm/bin/awesome)
#   PREFIX=     install prefix; derived from BIN if unset
#   LIB=        Lua lib dir               (default <prefix>/share/awesome/lib)
#   RC=         stock rc.lua              (default <prefix>/etc/xdg/awesome/rc.lua)
#   THEMES=     themes dir
#   ICONS=      icons dir
#   BUILD_DIR=  fallback source for BIN/LIB/RC/THEMES  (default /_ext/tmp/awe-build-release)
#   DISPLAY_PORT=  X display number       (default 99)
#   SECS=       time to run               (default 5)
#   STRICT=     1 = also fail on matrix_shim_used  (default 0)
#   FAIL_INJECT=  red-test hook: error | no-force | bad-utd  (see INJECT below)
#
# The script occupies one X display and must be run serially.
set -u

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

BIN=${BIN:-/usr/local/static/awm/bin/awesome}
BUILD_DIR=${BUILD_DIR:-/_ext/tmp/awe-build-release}
DISPLAY_PORT=${DISPLAY_PORT:-99}
SECS=${SECS:-5}
STRICT=${STRICT:-0}
FAIL_INJECT=${FAIL_INJECT:-}

# ---------------------------------------------------------------- fallbacks
# The install prefix does not exist in every sandbox, and /_ext is mounted
# noexec so a build-dir binary cannot be executed in place. Resolve both.
# PREFIX may come from the caller (step_smoke passes it); otherwise derive it
# from BIN. Keep the two in step or LIB/RC/THEMES point at the wrong tree.
if [ -z "${PREFIX:-}" ]; then
    PREFIX=${BIN%/*}
    PREFIX=${PREFIX%/*}        # .../awesome/bin/awesome -> .../awesome
fi
LIB=${LIB:-$PREFIX/share/awesome/lib}
RC=${RC:-$PREFIX/etc/xdg/awesome/rc.lua}
THEMES=${THEMES:-$PREFIX/share/awesome/themes}
ICONS=${ICONS:-$PREFIX/share/awesome/icons}

T=$(mktemp -d /_ext/tmp/systraysmoke.XXXXXX 2>/dev/null) \
    || T=$(mktemp -d /tmp/systraysmoke.XXXXXX)
XPID=""
DISP_OWNED=""
trap 'rm -rf "$T"; pkill -9 -x awesome 2>/dev/null; [ -n "${XPID:-}" ] && kill -9 "$XPID" 2>/dev/null; [ -z "${DISP_OWNED:-}" ] || rm -f "/tmp/.X$DISPLAY_PORT-lock" "/tmp/.X11-unix/X$DISPLAY_PORT"' EXIT

fail() { echo "FAIL: $*"; exit 1; }
say()  { echo "== $*"; }

[ -d "$LIB" ] || LIB=$BUILD_DIR/lib
[ -f "$RC" ]  || RC=$BUILD_DIR/awesomerc.lua
[ -d "$THEMES" ] || THEMES=$BUILD_DIR/themes
[ -d "$ICONS" ]  || ICONS=$BUILD_DIR/icons

# Binary: prefer the installed one; copy the build-dir one (noexec mount).
if [ ! -x "$BIN" ] || [ ! -f "$BIN" ]; then
    BIN=$BUILD_DIR/awesome
    [ -f "$BIN" ] || fail "no awesome binary (BIN or $BUILD_DIR/awesome)"
    cp "$BIN" "$T/awesome" && chmod +x "$T/awesome"
    BIN=$T/awesome
fi

[ -d "$LIB" ]    || fail "lib dir missing: $LIB"
[ -f "$RC" ]     || fail "rc.lua missing: $RC"
[ -d "$THEMES" ] || fail "themes dir missing: $THEMES"

# ────────────────────────────────────────────────────────────────────────────
# X server. Xvnc (tigervnc) is the only headless server used here; it is not
# installed everywhere, so it may well live outside PATH.
# ────────────────────────────────────────────────────────────────────────────
find_xvnc() {
    command -v Xvnc 2>/dev/null && return 0
    for c in /usr/local/static/tigervnc/bin/Xvnc /usr/bin/Xvnc; do
        [ -x "$c" ] && { echo "$c"; return 0; }
    done
}
xserver_up() {
    # pgrep -x is not portable: busybox' pgrep has no -x and returns an empty
    # list silently instead of erroring. An empty list here would make
    # start_xserver spin up a second server on a display that is already in
    # use. Scan /proc and match on argv[0], so a shell that merely quotes
    # "Xvnc" later in its argv is not mistaken for a server.
    local f cmd arg0
    for f in /proc/[0-9]*/cmdline; do
        cmd=$(tr '\0' ' ' < "$f" 2>/dev/null) || continue
        arg0=${cmd%% *}
        case "$arg0" in
            */Xvnc|Xvnc) ;;
            *) continue ;;
        esac
        case " $cmd " in
            *" :$DISPLAY_PORT "*) return 0 ;;
        esac
    done
    return 1
}
start_xserver() {
    xserver_up && { say "X server already on :$DISPLAY_PORT"; return 0; }
    XB=$(find_xvnc) || fail "no Xvnc available (pkg add tigervnc, or static /usr/local/static/tigervnc)"
    # Xvnc specifics, each one load-bearing:
    #   -ac           without it the X server rejects every other client of
    #                 :$DISPLAY_PORT with "authorization required".
    #   -localhost yes  bind 127.0.0.1 only; the VNC listener on 5900+PORT is
    #                 then not reachable outside the sandbox. Do NOT use
    #                 -nolisten tcp instead: it makes a second Xvnc believe the
    #                 display is already active and refuse to start.
    #   -geometry/-depth  Xvnc has no Xvfb-style `-screen 0 WxHxD`.
    # Also clear a stale lock/socket: a socket left behind by a killed server
    # would satisfy the readiness check below instantly, so the loop would
    # "succeed" with no server at all and awesome would die with
    # "cannot open display (error 1)". A lock left the same way makes the next
    # Xvnc abort with "Server is already active for display".
    rm -f "/tmp/.X$DISPLAY_PORT-lock" "/tmp/.X11-unix/X$DISPLAY_PORT"
    nohup "$XB" -ac -localhost yes ":$DISPLAY_PORT" \
        -geometry 1280x800 -depth 24 >/dev/null 2>&1 & XPID=$!
    for i in $(seq 1 30); do
        # Bail rather than wait forever if the server already exited.
        kill -0 "$XPID" 2>/dev/null || fail "X server exited during startup"
        [ -e "/tmp/.X11-unix/X$DISPLAY_PORT" ] \
            && { say "X server up (:$DISPLAY_PORT, Xvnc)"; DISP_OWNED=1; return 0; }
        sleep 1
    done
    fail "X server did not come up on :$DISPLAY_PORT"
}

# ────────────────────────────────────────────────────────────────────────────
# The probe. Must run BEFORE the stock rc.lua: systray.lua:228
#   gtable.crush(ret, systray, true)
# copies the module's fit/draw into the widget instance at construction time,
# so patching the module afterwards is a no-op.
# ────────────────────────────────────────────────────────────────────────────
cat > "$T/rc.lua" <<'LUA'
-- rc.lua for 12-systray-draw-smoke.sh
-- probe prelude first, then the stock rc.lua unchanged, then verdict + exit.

local gears = require("gears")

dofile(__PRELUDE__)
dofile(__RC__)

-- Let the render loop run, then report. draw() is invoked by the normal
-- damage loop; nothing here forces a repaint.
gears.timer.start_new(__SECS__, function()
    probe.dump("systray-draw")
    awesome.quit()
    return false
end)
LUA

cat > "$T/probe.lua" <<'LUA'
-- systray.draw probe. See 12-systray-draw-smoke.sh header for the theory.
local cairo = require("vendor.lgishim").cairo

probe = {
    forced_entries = 2,
    count_queries  = 0,
    placement_calls = 0,
    fit_calls      = 0,
    fit_zeros      = 0,
    draw_hooked    = false,
    draw_entered   = 0,
    draw_errors    = 0,
    last_error     = nil,
    utd_hooked     = false,
    utd_calls      = 0,
    utd_bad        = 0,
    matrix_shim_used = false,
}

-- ---- clear GATE 1: give systray a non-zero entry count -------------------
local real_systray = awesome.systray
awesome.systray = function(...)
    local n = select("#", ...)
    if n == 0 then
        probe.count_queries = probe.count_queries + 1
        return probe.forced_entries
    end
    probe.placement_calls = probe.placement_calls + 1   -- systray.lua:96
    return real_systray(...)
end

-- ---- instrument fit/draw on the module before the widget is built -------
local smod = require("wibox.widget.systray")
local orig_fit, orig_draw = smod.fit, smod.draw
if type(orig_fit) == "function" then
    smod.fit = function(self, context, width, height)
        probe.fit_calls = probe.fit_calls + 1
        local w, h = orig_fit(self, context, width, height)
        if w == 0 and h == 0 then probe.fit_zeros = probe.fit_zeros + 1 end
        return w, h
    end
end
if type(orig_draw) == "function" then
    -- draw_body is the indirection the red-test injects through, so the
    -- instrumentation below stays outermost either way and both counters
    -- (draw_entered / draw_errors) remain meaningful under FAIL_INJECT=error.
    local draw_body = orig_draw
    smod.draw = function(self, context, cr, width, height)
        probe.draw_entered = probe.draw_entered + 1
        local ok, err = pcall(draw_body, self, context, cr, width, height)
        if not ok then
            probe.draw_errors = probe.draw_errors + 1
            probe.last_error = tostring(err)
        end
        return ok and err or nil
    end
    probe.draw_hooked = true
    probe.set_draw_body = function(fn) draw_body = fn end
end

-- ---- intercept cairo methods via the __index metamethod -----------------
-- lgishim_cairo.c:class_setup installs __index as the C function m_index, so
-- the method table cannot be edited directly. Wrap __index and delegate
-- everything else. Never call m_index eagerly: it truncates the stack with
-- lua_settop(L, 3) and corrupts a plain call.
local function wrap_index(mt, on_lookup)
    local ri = mt and mt.__index
    if type(ri) ~= "function" then return false, type(ri) end
    mt.__index = function(self, k)
        local v = ri(self, k)
        local w = on_lookup(self, k, v)
        return w ~= nil and w or v
    end
    return true
end

-- cr:user_to_device_distance -- commit a51ed56b, and the load-bearing
-- assertion: systray.lua:75 is its only caller in lib/ and it sits after
-- every guard in draw().
local real_utd
local utd_ok = wrap_index(
    getmetatable(cairo.Context.create(
        cairo.ImageSurface(cairo.Format.ARGB32, 8, 8))) or {},
    function(self, k, v)
        if k ~= "user_to_device_distance" or type(v) ~= "function" then
            return nil
        end
        if real_utd == nil then real_utd = v end
        return function(s2, dx, dy)
            probe.utd_calls = probe.utd_calls + 1
            local x, y = real_utd(s2, dx, dy)
            if type(x) ~= "number" or type(y) ~= "number"
                    or x ~= x or y ~= y then           -- NaN guard
                probe.utd_bad = probe.utd_bad + 1
            end
            return x, y
        end
    end)
probe.utd_hooked = utd_ok

-- ---- TEST-ONLY fallback: lgishim cairo.Matrix is missing methods --------
-- lgishim_cairo.c:MATRIX_M lacks transform_point/transform_distance, which
-- gears/matrix.lua:185 and wibox/drawable.lua:173 both call. systray:draw
-- dies at systray.lua:62 before reaching line 75 until this is fixed upstream.
local real_tp, real_td
local mat_ok = wrap_index(getmetatable(cairo.Matrix.identity()) or {},
    function(self, k, v)
        if k == "transform_distance" then
            if type(v) == "function" then return v end
            probe.matrix_shim_used = true
            if real_td == nil then
                real_td = function(s2, x, y)  -- gears/matrix.lua:163-164
                    return s2.xx * x + s2.xy * y, s2.yx * x + s2.yy * y
                end
            end
            return real_td
        end
        if k == "transform_point" then
            if type(v) == "function" then return v end
            probe.matrix_shim_used = true
            if real_tp == nil then
                real_tp = function(s2, x, y)  -- gears/matrix.lua:169-172
                    return s2.x0 + s2.xx * x + s2.xy * y,
                           s2.y0 + s2.yx * x + s2.yy * y
                end
            end
            return real_tp
        end
        return nil
    end)
if not mat_ok then
    print("PROBE WARN: could not wrap cairo.Matrix __index")
end

-- ---- red-test injection (FAIL_INJECT) -----------------------------------
-- Each mode must turn the smoke RED. The assertion block also double-checks
-- that it did, so a broken injector cannot silently pass as "green".
if __INJECT__ == "no-force" then
    -- Restore the environment as it really is: no tray clients. GATE 1 fires,
    -- draw is never entered, and draw_entered/utd_calls must be 0.
    probe.forced_entries = 0
elseif __INJECT__ == "error" then
    -- draw() must raise. draw_errors must go > 0 while draw_entered stays >= 1,
    -- proving the instrumented wrapper is still outermost.
    if probe.set_draw_body then
        probe.set_draw_body(function()
            error("SYSTRAY_SMOKE_INJECTED_FAILURE")
        end)
    else
        smod.draw = function() error("SYSTRAY_SMOKE_INJECTED_FAILURE") end
    end
elseif __INJECT__ == "bad-utd" then
    -- user_to_device_distance must return garbage. utd_bad must go > 0 and the
    -- draw must die at systray.lua:76 (abs() on a non-number).
    real_utd = function() return "not-a-number", 0 end
end

function probe.dump(tag)
    local L = {
        "SMOKE-VERDICT",
        ("  tag              %s"):format(tag or "?"),
        ("  count_queries    %d"):format(probe.count_queries),
        ("  placement_calls  %d"):format(probe.placement_calls),
        ("  fit_calls        %d"):format(probe.fit_calls),
        ("  fit_zeros        %d"):format(probe.fit_zeros),
        ("  draw_hooked      %s"):format(tostring(probe.draw_hooked)),
        ("  draw_entered     %d"):format(probe.draw_entered),
        ("  draw_errors      %d"):format(probe.draw_errors),
        ("  utd_hooked       %s"):format(tostring(probe.utd_hooked)),
        ("  utd_calls        %d"):format(probe.utd_calls),
        ("  utd_bad          %d"):format(probe.utd_bad),
        ("  matrix_shim_used %s"):format(tostring(probe.matrix_shim_used)),
    }
    if probe.last_error then
        L[#L + 1] = ("  last_error       %s"):format(probe.last_error)
    end
    print(table.concat(L, "\n"))
end

print(("PROBE prelude: draw_hooked=%s utd_hooked=%s inject=%s"):format(
    tostring(probe.draw_hooked), tostring(probe.utd_hooked),
    __INJECT__ or "none"))
LUA

# Placeholders live in two files: __PRELUDE__/__RC__/__SECS__ in rc.lua,
# __INJECT__ in probe.lua. Substitute each token into the file that holds it.
sed -i "s#__PRELUDE__#'$T/probe.lua'#; s#__RC__#'$RC'#; s#__SECS__#$SECS#" "$T/rc.lua"
sed -i "s#__INJECT__#'$FAIL_INJECT'#" "$T/probe.lua"

# --------------------------------------------------------------------------
say "X server on :$DISPLAY_PORT"
start_xserver

say "running $BIN  ($SECS s)"
LOG="$T/run.log"
DISPLAY=":$DISPLAY_PORT" \
  LUA_PATH="$LIB/?.lua;$LIB/?/init.lua;;" \
  LUA_CPATH="$LIB/?.so;;" \
  AWESOME_THEMES_PATH="$THEMES" \
  AWESOME_ICON_PATH="$ICONS" \
  timeout -s KILL $((SECS + 20)) "$BIN" -c "$T/rc.lua" >"$LOG" 2>&1
RC_AWE=$?

VERDICT=$(awk '/^SMOKE-VERDICT$/{f=1} f' "$LOG")
[ -n "$VERDICT" ] || { cat "$LOG"; fail "no SMOKE-VERDICT in output"; }
echo "$VERDICT"

# rc 0 = clean quit(); 124/137 = SIGKILLed by timeout = still running, which
# is also fine (it means nothing crashed). Anything else is a boot failure.
case "$RC_AWE" in 0|124|137|143) ;; *) cat "$LOG"; fail "awesome rc=$RC_AWE";; esac

# --------------------------------------------------------------- assertions
# get FIELD value : number, from the verdict block
g() { printf '%s' "$VERDICT" | awk -v k="  $1" '$0 ~ k {print $2; exit}'; }
draw_entered=$(g draw_entered);  draw_errors=$(g draw_errors)
utd_calls=$(g utd_calls);        utd_bad=$(g utd_bad)
draw_hooked=$(g draw_hooked);    utd_hooked=$(g utd_hooked)      matrix_shim=$(g matrix_shim_used)

BAD=0
bad() { echo "  ASSERT FAIL: $*"; BAD=1; }

[ "$draw_hooked" = "true" ] || bad "draw_hooked is not true (instrumentation missing)"
[ "$utd_hooked"  = "true" ] || bad "utd_hooked is not true (instrumentation missing)"
[ "${draw_entered:-0}" -ge 1 ] || bad "draw_entered=$draw_entered (draw never entered)"
[ "${draw_errors:-0}"  -eq 0 ] || bad "draw_errors=$draw_errors"
[ "${utd_calls:-0}"    -ge 1 ] || bad "utd_calls=$utd_calls (guard skipped -- short-circuit, not execution)"
[ "${utd_bad:-0}"      -eq 0 ] || bad "utd_bad=$utd_bad (user_to_device_distance returned non-numeric)"

MATRIX_WARN=""
if [ "$matrix_shim" = "true" ]; then
    MATRIX_WARN="lgishim cairo.Matrix lacks transform_point/transform_distance"
    if [ "$STRICT" = "1" ]; then
        bad "$MATRIX_WARN: probe used a test-only fallback. Fix "\
            "lib/vendor/lgishim_cairo.c:MATRIX_M or set STRICT=0 to accept."
    fi
fi

if [ "${FAIL_INJECT:-}" = "no-force" ]; then
    [ "$BAD" -eq 1 ] \
      && { echo "INJECT(no-force): correctly RED as intended"; exit 0; } \
      || { echo "INJECT(no-force): stayed GREEN -- the check is vacuous"; exit 1; }
fi
if [ "${FAIL_INJECT:-}" = "error" ]; then
    [ "$BAD" -eq 1 ] \
      && { echo "INJECT(error): correctly RED as intended"; exit 0; } \
      || { echo "INJECT(error): stayed GREEN -- the check is vacuous"; exit 1; }
fi
if [ "${FAIL_INJECT:-}" = "bad-utd" ]; then
    [ "$BAD" -eq 1 ] \
      && { echo "INJECT(bad-utd): correctly RED as intended"; exit 0; } \
      || { echo "INJECT(bad-utd): stayed GREEN -- the check is vacuous"; exit 1; }
fi

[ "$BAD" -eq 0 ] || { cat "$LOG"; fail "systray:draw did not really run"; }

if [ -n "$MATRIX_WARN" ]; then
    echo "WARNING: $MATRIX_WARN (upstream bug, reported -- not gating here)."
    echo "         lgishim_cairo.c:MATRIX_M needs transform_point + transform_distance,"
    echo "         or systray:draw dies at systray.lua:62 on every real display."
    echo "         Run with STRICT=1 to make this a hard failure."
fi
echo "PASS: systray:draw executed and cr:user_to_device_distance ran"
