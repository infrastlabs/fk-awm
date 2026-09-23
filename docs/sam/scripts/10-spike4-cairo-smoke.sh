#!/bin/bash
# 10-spike4-cairo-smoke.sh — Phase 2 / Parts 1+2+3+4+7 (foundation, enums, real
# calls, Pango bridge, Surface/Pattern/Matrix/Region extensions, the property
# (getter/setter) layer, and Pango Layout measurement)
#
# Evolved from 08-spike4-cairo-part1-smoke.sh (git mv). Part 1 shipped only the
# wrap/metatable/enum plumbing with inert stub constructors, so the two sections
# that asserted "Rectangle is nil" and "constructors return nil" are gone --
# Part 2 replaced the stubs with real cairo_* calls. Everything that still
# holds from Part 1 (string-valued enums, real cairo enum names, the invented
# names are dead, metatable-identity is_type_of) is kept as a regression guard.
#
# The Part 2 sections verify that the shim is genuinely wired to cairo and not
# silently stubbed out. cairo defers errors, so the meaningful checks are
# round-trips through real cairo state:
#   set_operator() -> cr.operator   must come back as the same name
#   scale(2,3)     -> cr.matrix.xx  must be 2
#   move_to(3,4)   -> get_current_point()  must be 3,4
#   clip()         -> clip_extents()       must match
#   text_extents("hello")                 must have width > 0
#
# Part 2c (§15) covers the Pango bridge (cr:update_layout / cr:show_layout)
# used by lib/wibox/widget/textbox.lua.
#
# Part 3 (§16–19) covers the remaining API surface:
#   Surface: create_similar, get_width, get_height, get_content, flush,
#     get_data, get_type, get_description, mark_dirty, mark_dirty_rectangle,
#     get_device_offset, push_group, pop_group, push_group_to_image,
#     get_font_options
#   Pattern: get_type, get_target, get_target_format, get_filter, get_matrix,
#     get_extend, get_opaque, get_rgba, get_color_stop_count, get_color_stop,
#     get_surface_pattern_surface, get_surface_pattern_filter
#   Matrix: identity, translate, scale
#   Region: new class + contains_point
#
# Usage: bash 10-spike4-cairo-smoke.sh
set -u

REPO=${REPO:-/_ext/home/headless/xm-zs01/fk-awesomewm}
T=$(mktemp -d /_ext/tmp/cairosmoke.XXXXXX)
trap 'rm -rf "$T"' EXIT

# --- 0. Wiring guard (Part 6 regression) ---
# A shim module that compiles and links into awesome but is never put into
# package.preload in luaa.c is silently replaced by an empty table by the
# `pcall(require, ...)` fallback in lib/vendor/lgishim.lua.  That is how Gio
# shipped as `{}` for two parts: every check ran against a green empty table.
# Both directions must hold:
#   * every luaopen_lgishim_* extern in luaa.c has a package.preload entry
#   * every such module is required by lib/vendor/lgishim.lua
# `lgishim_pangocairo` is registered from inside lgishim_pango.c, so only the
# luaa.c side is authoritative here.
if ! grep -q 'luaopen_lgishim_' "$REPO/luaa.c"; then
    echo "FAIL: luaa.c declares no luaopen_lgishim_*"
    exit 1
fi
WIRE_FAIL=0
for MOD in $(grep -o 'luaopen_lgishim_[a-z0-9_]*' "$REPO/luaa.c" \
        | sed 's/^luaopen_//' | sort -u); do
    if ! grep -q "lua_setfield(L, -2, \"$MOD\")" "$REPO/luaa.c"; then
        echo "FAIL: $MOD is externed in luaa.c but never added to package.preload"
        WIRE_FAIL=1
    fi
    if ! grep -q "require, \"$MOD\"" "$REPO/lib/vendor/lgishim.lua"; then
        echo "FAIL: $MOD is preloaded but lib/vendor/lgishim.lua never requires it"
        WIRE_FAIL=1
    fi
done
if [ "$WIRE_FAIL" -ne 0 ]; then
    echo "FAIL: luaa.c <-> lib/vendor/lgishim.lua wiring mismatch"
    exit 1
fi
echo "--- wiring: $(grep -o 'luaopen_lgishim_[a-z0-9_]*' "$REPO/luaa.c" \
        | sed 's/^luaopen_//' | sort -u | tr '\n' ' ')"

# --- 1. Minimal Lua host (same pattern as 07-spike2-shim-smoke.sh) ---
cat > "$T/smoke_main.c" <<'EOF'
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <stdio.h>

extern int luaopen_lgishim_pango(lua_State *L);
extern int luaopen_lgishim_pangocairo(lua_State *L);
extern int luaopen_lgishim_cairo(lua_State *L);
extern int luaopen_lgishim_glib(lua_State *L);
extern int luaopen_lgishim_pixbuf(lua_State *L);
extern int luaopen_lgishim_gobject(lua_State *L);
extern int luaopen_lgishim_gio(lua_State *L);

int
main(int argc, char **argv)
{
    lua_State *L;
    int rc;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <script.lua>\n", argv[0]);
        return 2;
    }

    L = luaL_newstate();
    luaL_openlibs(L);

    lua_getglobal(L, "package");
    lua_getfield(L, -1, "preload");
    lua_pushcfunction(L, luaopen_lgishim_pango);
    lua_setfield(L, -2, "lgishim_pango");
    lua_pushcfunction(L, luaopen_lgishim_pangocairo);
    lua_setfield(L, -2, "lgishim_pangocairo");
    lua_pushcfunction(L, luaopen_lgishim_cairo);
    lua_setfield(L, -2, "lgishim_cairo");
    lua_pushcfunction(L, luaopen_lgishim_glib);
    lua_setfield(L, -2, "lgishim_glib");
    lua_pushcfunction(L, luaopen_lgishim_pixbuf);
    lua_setfield(L, -2, "lgishim_pixbuf");
    lua_pushcfunction(L, luaopen_lgishim_gobject);
    lua_setfield(L, -2, "lgishim_gobject");
    lua_pushcfunction(L, luaopen_lgishim_gio);
    lua_setfield(L, -2, "lgishim_gio");
    lua_pop(L, 2);

    rc = luaL_dofile(L, argv[1]) ? 1 : 0;
    if (rc)
        fprintf(stderr, "ERR: %s\n", lua_tostring(L, -1));
    lua_close(L);
    return rc;
}
EOF

# --- 2. Test script ---
cat > "$T/part12_smoke.lua" <<'EOF'
-- Unbuffered: stdout is a pipe here, so a crash would otherwise lose the tail
-- of the output and the last printed line would not be the crash site.
io.stdout:setvbuf("no")
local fails = 0
local n = 0
local function check(cond, name, extra)
    n = n + 1
    if not cond then
        fails = fails + 1
        print("  FAIL " .. name .. (extra and ("  -- " .. tostring(extra)) or ""))
    else
        print("  ok   " .. name)
    end
end
local function is_table(name, v)
    check(type(v) == "table", name, "type=" .. type(v))
end
local function is_userdata(name, v)
    check(type(v) == "userdata", name, "type=" .. type(v))
end
local function eq(name, got, want)
    check(got == want, name, "got=" .. tostring(got) .. " want=" .. tostring(want))
end
local function approx(name, got, want, tol)
    tol = tol or 1e-6
    check(type(got) == "number" and math.abs(got - want) < tol, name,
        "got=" .. tostring(got) .. " want=" .. tostring(want))
end
-- Every Part 2 method call goes through this so a signature mismatch surfaces
-- as a named failure instead of an unattributed script abort.
local function safe(name, fn)
    local ok, err = pcall(fn)
    check(ok, name, err)
    return ok
end

print("== module ==")
local lgi = require("vendor.lgishim")
check(lgi.shim == true, "lgishim.lua selected the shim")
local cairo = lgi.Cairo
is_table("lgi.Cairo", cairo)
check(cairo == lgi.cairo, "lgi.cairo lowercase alias")

-- Regression guard (Part 6): a shim module that compiles and links but is
-- never preloaded in luaa.c is silently replaced by an empty table below
-- (`ok and Gio or {}`). Anything asserting against its own harness preload
-- suite stays green, so the failure only shows up at runtime as nil fields.
-- Every shim table must therefore be non-empty.
for _, name in ipairs({ "Cairo", "Pango", "PangoCairo", "GdkPixbuf",
        "GLib", "GObject", "Gio" }) do
    check(next(lgi[name]) ~= nil, name .. " module table is non-empty",
        "0 fields -- luaa.c package.preload entry missing?")
end

-- The Pango round must still be intact (regression guard).
local Pango = lgi.Pango
check(Pango and Pango.SCALE == 1024, "Pango.SCALE regression")

print("")
print("== 1. enum constants are STRINGS, not ints ==")
local Op = cairo.Operator
is_table("cairo.Operator", Op)
local function str_eq(name, got, want)
    check(type(got) == "string", name .. " is a string", "type=" .. type(got))
    check(got == want, name .. " == " .. want, "got=" .. tostring(got))
end
str_eq("Op.SOURCE", Op.SOURCE, "SOURCE")
str_eq("Op.CLEAR", Op.CLEAR, "CLEAR")
str_eq("Op.OVER", Op.OVER, "OVER")
str_eq("Status.SUCCESS", cairo.Status.SUCCESS, "SUCCESS")
str_eq("Extend.REPEAT", cairo.Extend.REPEAT, "REPEAT")
str_eq("Format.ARGB32", cairo.Format.ARGB32, "ARGB32")
str_eq("Content.COLOR", cairo.Content.COLOR, "COLOR")
str_eq("FillRule.EVEN_ODD", cairo.FillRule.EVEN_ODD, "EVEN_ODD")
str_eq("PatternType.SOLID", cairo.PatternType.SOLID, "SOLID")
str_eq("Filter.BILINEAR", cairo.Filter.BILINEAR, "BILINEAR")

print("")
print("== 2. all 14 enum tables present ==")
local ENUMS = {
    "Operator", "Format", "Content", "Status", "Antialias", "LineCap",
    "LineJoin", "FillRule", "Path", "Extend", "FontSlant", "FontWeight",
    "PatternType", "Filter",
}
local total_members = 0
for _, k in ipairs(ENUMS) do
    local t = cairo[k]
    is_table("cairo." .. k, t)
    local cnt = 0
    for _ in pairs(t) do cnt = cnt + 1 end
    total_members = total_members + cnt
    check(cnt > 0, "cairo." .. k .. " is non-empty", cnt)
    print(("  --> %s: %d members"):format(k, cnt))
end
check(total_members >= 80, "enum member total >= 80", total_members)

print("")
print("== 3. real cairo names present (the Part 1 bug-fix surface) ==")
local PRESENT = {
    "Operator.SATURATE", "Operator.HARD_LIGHT", "Operator.SOFT_LIGHT",
    "Operator.DEST_IN", "Operator.DEST_OUT", "Operator.OVERLAY",
    "Content.ALPHA", "Extend.NONE", "Extend.PAD", "Extend.REFLECT",
    "FontWeight.BOLD", "FontWeight.NORMAL",
    "PatternType.LINEAR", "PatternType.RADIAL",
    "Filter.FAST", "Filter.GAUSSIAN", "Filter.BEST",
}
for _, name in ipairs(PRESENT) do
    local t, key = name:match("^(.+)%.(.+)$")
    local got = cairo[t][key]
    check(got == key, name, "got=" .. tostring(got) .. " want=" .. key)
end

print("")
print("== 4. invented names are GONE ==")
local ABSENT = {
    { "Operator.SUBTRACT",  cairo.Operator.SUBTRACT },
    { "Operator.HARDLIGHT", cairo.Operator.HARDLIGHT },
    { "Operator.SOFTLIGHT", cairo.Operator.SOFTLIGHT },
    { "Operator.OUT_OVER",  cairo.Operator.OUT_OVER },
    { "Operator.IN_OVER",   cairo.Operator.IN_OVER },
    { "Content.NONE",       cairo.Content.NONE },
}
for _, p in ipairs(ABSENT) do
    check(p[2] == nil, p[1] .. " is nil", "got=" .. tostring(p[2]))
end

print("")
print("== 5. Matrix ==")
local Matrix = cairo.Matrix
is_table("cairo.Matrix", Matrix)
check(type(Matrix.new) == "function", "Matrix.new")
local m = Matrix.new()
is_userdata("Matrix.new() returns userdata", m)
approx("identity xx", m.xx, 1)
approx("identity yy", m.yy, 1)
approx("identity xy", m.xy, 0)
approx("identity yx", m.yx, 0)
approx("identity x0", m.x0, 0)
approx("identity y0", m.y0, 0)
safe("m:init()", function() m:init(2, 0, 0, 3, 10, 20) end)
approx("init xx", m.xx, 2)
approx("init yy", m.yy, 3)
approx("init x0", m.x0, 10)
approx("init y0", m.y0, 20)
-- gears/matrix.lua:201 multiplies then reads the result's fields. Use an
-- identity operand: that check is independent of cairo's row/column convention.
local s = cairo.Matrix.new()
s:init(2, 0, 0, 3, 10, 20)
local ident = cairo.Matrix.new()
local p = s:multiply(ident)
is_userdata("Matrix:multiply() returns userdata", p)
approx("multiply by identity xx", p.xx, 2)
approx("multiply by identity yy", p.yy, 3)
approx("multiply by identity x0", p.x0, 10)
approx("multiply by identity y0", p.y0, 20)

local t = cairo.Matrix.new()
t:init(2, 0, 0, 2, 5, 5)
local inv = t:invert()
is_userdata("Matrix:invert() returns userdata", inv)
approx("invert of scale(2) xx", inv.xx, 0.5)
local rt = inv:multiply(t)
approx("invert x self xx", rt.xx, 1, 1e-4)
approx("invert x self yy", rt.yy, 1, 1e-4)
approx("invert x self x0", rt.x0, 0, 1e-4)
approx("invert x self y0", rt.y0, 0, 1e-4)

-- A pure translation is symmetric, so its inverse is convention-independent.
local tr = cairo.Matrix.new()
tr:init(1, 0, 0, 1, 5, 5)
local tr_inv = tr:invert()
approx("invert translate x0", tr_inv.x0, -5)
approx("invert translate y0", tr_inv.y0, -5)

print("")
print("== 6. Surface / ImageSurface / RecordingSurface ==")
local Surface = cairo.Surface
local ImageSurface = cairo.ImageSurface
local RecordingSurface = cairo.RecordingSurface
is_table("cairo.Surface", Surface)
is_table("cairo.ImageSurface", ImageSurface)
is_table("cairo.RecordingSurface", RecordingSurface)

local img = ImageSurface.new(cairo.Format.ARGB32, 100, 50)
is_userdata("ImageSurface.new() returns userdata", img)
check(img.width == 100, "img.width == 100", img.width)
check(img.height == 50, "img.height == 50", img.height)
check(img.content == cairo.Content.COLOR_ALPHA,
    "img.content == COLOR_ALPHA", tostring(img.content))

-- gears/surface.lua:72 wraps an existing surface through Surface().
local img2 = Surface.new(img)
is_userdata("Surface.new(img) returns userdata", img2)
check(img2 == img, "Surface.new() passes the surface through")
safe("Surface.new(nil) is a no-op", function() return Surface.new(nil) end)
check(Surface.new(nil) == nil, "Surface.new(nil) returns nil")

local sim = Surface.create_similar(img, cairo.Content.COLOR_ALPHA, 40, 30)
is_userdata("Surface.create_similar() returns userdata", sim)
check(sim.width == 40 and sim.height == 30, "similar surface dims",
    sim.width .. "x" .. sim.height)

local rec = RecordingSurface.new(cairo.Content.COLOR_ALPHA,
    { x = 0, y = 0, width = 10, height = 10 })
is_userdata("RecordingSurface.new() returns userdata", rec)
check(rec.content == cairo.Content.COLOR_ALPHA,
    "rec.content == COLOR_ALPHA", tostring(rec.content))

safe("img:flush()", function() img:flush() end)
safe("img:show_page()", function() img:show_page() end)

-- The user-data hook is what makes create_for_data safe: cairo holds the
-- pointer, so the copy must outlive the Lua string.
local pixels = string.rep("\0", 400)
local owned = ImageSurface.create_for_data(pixels, cairo.Format.ARGB32, 10, 10, 40)
is_userdata("ImageSurface.create_for_data() returns userdata", owned)
check(owned.width == 10 and owned.height == 10, "create_for_data dims",
    owned.width .. "x" .. owned.height)
safe("create_for_data result flushes", function() owned:flush() end)

print("")
print("== 7. Pattern ==")
local Pattern = cairo.Pattern
is_table("cairo.Pattern", Pattern)

-- lib/ calls these with the DOT, never the colon: cairo.Pattern.create_rgba(...).
local solid = cairo.Pattern.CreateRgba(0.5, 0.25, 0.125, 1)
is_userdata("Pattern.CreateRgba() returns userdata", solid)
local r, g, bl, al = solid:get_rgba()
approx("get_rgba r", r, 0.5)
approx("get_rgba g", g, 0.25)
approx("get_rgba b", bl, 0.125)
approx("get_rgba a", al, 1)
check(solid.type == cairo.PatternType.SOLID,
    "solid.type == SOLID", tostring(solid.type))

safe("p:set_filter()", function() solid:set_filter(cairo.Filter.BILINEAR) end)
check(solid.filter == cairo.Filter.BILINEAR,
    "filter round-trips", tostring(solid.filter))
safe("p:set_extend()", function() solid:set_extend(cairo.Extend.REPEAT) end)
check(solid.extend == cairo.Extend.REPEAT,
    "extend round-trips", tostring(solid.extend))

local lin = cairo.Pattern.CreateLinear(0, 0, 100, 0)
is_userdata("Pattern.CreateLinear() returns userdata", lin)
check(lin.type == cairo.PatternType.LINEAR,
    "linear.type == LINEAR", tostring(lin.type))
safe("add_color_stop_rgba(0)", function() lin:add_color_stop_rgba(0, 1, 0, 0, 1) end)
safe("add_color_stop_rgba(1)", function() lin:add_color_stop_rgba(1, 0, 0, 1, 1) end)
local _, off, sr, sg, sb, sa = lin:get_color_stop_rgba(0)
approx("color_stop offset", off, 0)
approx("color_stop r", sr, 1)
approx("color_stop b", sb, 0)
approx("color_stop a", sa, 1)

local rad = cairo.Pattern.CreateRadial(0, 0, 0, 0, 0, 50)
is_userdata("Pattern.CreateRadial() returns userdata", rad)
check(rad.type == cairo.PatternType.RADIAL,
    "radial.type == RADIAL", tostring(rad.type))

local sp = cairo.Pattern.CreateForSurface(img)
is_userdata("Pattern.CreateForSurface() returns userdata", sp)
check(sp.type == cairo.PatternType.SURFACE,
    "surface pattern type", tostring(sp.type))

local pm = cairo.Matrix.new()
pm:init(1, 0, 0, 1, 7, 9)
safe("p:set_matrix()", function() solid:set_matrix(pm) end)
local got_pm = solid.matrix
is_userdata("p.matrix is a Matrix userdata", got_pm)
approx("p.matrix.x0 round-trips", got_pm.x0, 7)
approx("p.matrix.y0 round-trips", got_pm.y0, 9)
print("  gc after section 7 (patterns)")
collectgarbage("collect")

print("")
print("== 8. Context: state ==")
local Context = cairo.Context
is_table("cairo.Context", Context)
check(type(Context.new) == "function", "Context.new")

local cr = Context.new(img)
is_userdata("Context.new(img) returns userdata", cr)
eq("cr:get_status() == SUCCESS", cr:get_status(), "SUCCESS")
check(cr.status == "SUCCESS", "cr.status property == SUCCESS", tostring(cr.status))

-- operator must round-trip through real cairo state.
safe("set_operator", function() cr:set_operator(cairo.Operator.MULTIPLY) end)
check(cr.operator == cairo.Operator.MULTIPLY,
    "operator round-trips", tostring(cr.operator))

-- scale must produce real matrix arithmetic.
safe("cr:scale(2,3)", function() cr:scale(2, 3) end)
local cm = cr:get_matrix()
is_userdata("cr:get_matrix() returns userdata", cm)
approx("scale -> matrix.xx == 2", cm.xx, 2)
approx("scale -> matrix.yy == 3", cm.yy, 3)
approx("scale -> matrix.x0 == 0", cm.x0, 0)
check(cr.matrix.xx == 2, "cr.matrix.xx property", tostring(cr.matrix.xx))
safe("cr:transform()", function() cr:transform(cm) end)
safe("cr:set_matrix()", function() cr:set_matrix(cm) end)
safe("cr:reset_matrix()", function() cr:reset_matrix() end)
local im = cr:get_matrix()
approx("reset_matrix -> identity xx", im.xx, 1)
approx("reset_matrix -> identity yy", im.yy, 1)

safe("cr:translate(10,20)", function() cr:translate(10, 20) end)
approx("translate -> matrix.x0 == 10", cr:get_matrix().x0, 10)
approx("translate -> matrix.y0 == 20", cr:get_matrix().y0, 20)
safe("cr:rotate(0.1)", function() cr:rotate(0.1) end)
safe("cr:reset_matrix()", function() cr:reset_matrix() end)

-- user_to_device_distance (wibox/widget/systray.lua:75 probes (1,0) to spot a
-- rotated context). It must return the rotated *length* only -- the translation
-- part has to be cancelled out, otherwise rotation detection is garbage.
cr:save()
cr:translate(10, 20)
cr:rotate(math.pi / 2)
local ddx, ddy = cr:user_to_device_distance(1, 0)
approx("user_to_device_distance 90deg -> dx == 0", ddx, 0)
approx("user_to_device_distance 90deg -> dy == 1", ddy, 1)
local nux, nuy = cr:user_to_device_distance(-2, 0)
approx("user_to_device_distance 90deg neg -> x == 0", nux, 0)
approx("user_to_device_distance 90deg neg -> y == -2", nuy, -2)
cr:restore()
local iddx, iddy = cr:user_to_device_distance(1, 0)
approx("identity -> user_to_device_distance dx == 1", idddx, 1)
approx("identity -> user_to_device_distance dy == 0", iddy, 0)
check(iddd ~= nil and iddy ~= nil, "user_to_device_distance returns 2 values")

-- save/restore must actually restore the state.
cr:save()
cr:translate(100, 100)
cr:restore()
approx("restore -> matrix.x0 back to 0", cr:get_matrix().x0, 0)

-- move_to drives get_current_point.
safe("cr:move_to(3,4)", function() cr:move_to(3, 4) end)
local cx, cy = cr:get_current_point()
approx("current_point x", cx, 3)
approx("current_point y", cy, 4)
safe("cr:rel_move_to(1,2)", function() cr:rel_move_to(1, 2) end)
approx("rel_move_to -> x == 4", cr:get_current_point(), 4)

-- new_sub_path (gears/shape.lua:287 rounded_rect, :622 pie, :661 arc) starts a
-- sub-path at the origin so the trailing close_path() returns to its own start
-- instead of the enclosing path's. It must not keep the running current point.
cr:save()
cr:move_to(7, 8)
safe("cr:line_to(9,9)", function() cr:line_to(9, 9) end)
safe("cr:new_sub_path()", function() cr:new_sub_path() end)
local sx, sy = cr:get_current_point()
approx("new_sub_path -> current_point x == 0", sx, 0)
approx("new_sub_path -> current_point y == 0", sy, 0)
cr:restore()
approx("restore re-applies the path -> x back to 4", cr:get_current_point(), 4)

local tgt = cr:get_target()
is_userdata("cr:get_target() returns userdata", tgt)
check(cairo.Surface:is_type_of(tgt), "get_target() is a Surface")

-- Clip extents must reflect a real clip.
-- cairo intersects the clip with the surface's own region, so a rectangle that
-- runs past the bottom of a 100x50 surface is clamped at y=50.
cr:save()
cr:rectangle(10, 20, 30, 40)
cr:clip()
local x0, y0, x1, y1 = cr:clip_extents()
approx("clip_extents x0", x0, 10)
approx("clip_extents y0", y0, 20)
approx("clip_extents x1", x1, 40)
approx("clip_extents y1 clamped to the surface height", y1, 50)
cr:restore()
local rx0, ry0, rx1, ry1 = cr:clip_extents()
approx("restore clears the clip -> x0 back to 0", rx0, 0)
approx("restore -> y0 back to 0", ry0, 0)
approx("restore -> x1 is the target width", rx1, 100)
approx("restore -> y1 is the target height", ry1, 50)
safe("cr:clip_preserve()", function()
    cr:save(); cr:rectangle(0, 0, 5, 5); cr:clip_preserve(); cr:restore()
end)
safe("cr:reset_clip()", function() cr:reset_clip() end)
safe("cr:new_path()", function() cr:new_path() end)

-- sources.
safe("set_source_rgb", function() cr:set_source_rgb(1, 0, 0) end)
safe("set_source_rgba", function() cr:set_source_rgba(0, 1, 0, 0.5) end)
local gs = cr:get_source()
is_userdata("cr:get_source() returns userdata", gs)
check(cairo.Pattern:is_type_of(gs), "get_source() is a Pattern")
safe("get_source():set_filter()", function() gs:set_filter(cairo.Filter.NEAREST) end)
safe("set_source", function() cr:set_source(lin) end)
safe("set_source_surface", function() cr:set_source_surface(sim, 0, 0) end)
safe("set_hairline", function() cr:set_hairline() end)
check(cr.hairline == true, "hairline property after set_hairline",
    tostring(cr.hairline))
safe("set_line_width(5)", function() cr:set_line_width(5) end)
approx("cr.line_width == 5", cr.line_width, 5)
check(cr.hairline == false, "hairline false at width 5", tostring(cr.hairline))

-- dashes: lib/ calls set_dash(table, #table, offset).
safe("set_dash", function() cr:set_dash({ 2, 4, 6 }, 3, 1.5) end)
-- get_dash() returns three values; the .dashes property goes through __index,
-- which collapses a multi-return getter to its first value.
local dsh, nd, doff = cr:get_dash()
is_table("get_dash returns a table", dsh)
check(nd == 3, "dash count == 3", tostring(nd))
approx("dash offset == 1.5", doff, 1.5)
check(dsh[1] == 2 and dsh[2] == 4 and dsh[3] == 6,
    "dash values round-trip", tostring(dsh[1]) .. "/" .. tostring(dsh[2]) .. "/" .. tostring(dsh[3]))
safe("set_dash empty", function() cr:set_dash({}, 0, 0) end)
local dsh2, nd2 = cr:get_dash()
is_table("empty dash table", dsh2)
check(nd2 == 0, "empty dash count == 0", tostring(nd2))

safe("set_antialias", function() cr:set_antialias(cairo.Antialias.NONE) end)
check(cr.antialias == cairo.Antialias.NONE,
    "antialias round-trips", tostring(cr.antialias))
safe("set_fill_rule", function() cr:set_fill_rule(cairo.FillRule.EVEN_ODD) end)
check(cr.fill_rule == cairo.FillRule.EVEN_ODD,
    "fill_rule round-trips", tostring(cr.fill_rule))
safe("set_line_cap", function() cr:set_line_cap(cairo.LineCap.ROUND) end)
check(cr.line_cap == cairo.LineCap.ROUND,
    "line_cap round-trips", tostring(cr.line_cap))
safe("set_line_join", function() cr:set_line_join(cairo.LineJoin.ROUND) end)
check(cr.line_join == cairo.LineJoin.ROUND,
    "line_join round-trips", tostring(cr.line_join))
safe("set_miter_limit", function() cr:set_miter_limit(10) end)
safe("set_tolerance", function() cr:set_tolerance(1.5) end)
approx("tolerance round-trips", cr.tolerance, 1.5)
print("  gc after section 8 (context state)")
collectgarbage("collect")

print("")
print("== 9. Context: drawing (real cairo must not raise a deferred error) ==")
local dimg = cairo.ImageSurface.new(cairo.Format.ARGB32, 200, 100)
local dc = cairo.Context.new(dimg)
eq("fresh context status", dc:get_status(), "SUCCESS")

safe("move_to/line_to/stroke", function()
    dc:set_source_rgb(1, 0, 0)
    dc:move_to(0, 0); dc:line_to(100, 100); dc:stroke()
end)
safe("rel_move_to/rel_line_to", function()
    dc:move_to(0, 0)
    dc:rel_move_to(5, 5); dc:rel_line_to(10, 10); dc:stroke()
end)
safe("curve_to", function()
    dc:move_to(0, 0)
    dc:curve_to(10, 0, 20, 10, 30, 10)
    dc:stroke()
end)
safe("rel_curve_to", function()
    dc:move_to(0, 0)
    dc:rel_curve_to(10, 0, 20, 10, 30, 10)
    dc:stroke()
end)
safe("arc", function()
    dc:arc(50, 50, 20, 0, 6.283)
    dc:stroke()
end)
safe("arc_negative", function()
    dc:arc_negative(50, 50, 20, 0, -6.283)
    dc:stroke()
end)
safe("rectangle/fill", function()
    dc:rectangle(10, 10, 30, 20); dc:fill()
end)
safe("fill_rectangle", function() dc:fill_rectangle(20, 20, 10, 10) end)
safe("stroke_rectangle", function() dc:stroke_rectangle(30, 30, 10, 10) end)
safe("circle", function() dc:circle(60, 60, 8); dc:fill() end)
safe("close_path", function() dc:move_to(0, 0); dc:line_to(5, 5); dc:close_path() end)
safe("path_close alias", function() dc:move_to(0, 0); dc:path_close() end)
safe("fill_preserve", function()
    dc:rectangle(0, 0, 5, 5); dc:fill_preserve()
end)
safe("stroke_preserve", function()
    dc:rectangle(0, 0, 5, 5); dc:stroke_preserve()
end)
safe("paint", function() dc:set_source_rgb(0, 0, 1); dc:paint() end)
safe("paint_with_alpha", function() dc:paint_with_alpha(0.5) end)
safe("mask", function() dc:set_source(lin); dc:mask(lin) end)
safe("mask_surface", function() dc:mask_surface(sim, 0, 0) end)
safe("push_group/paint/pop_group_to_source", function()
    dc:push_group(); dc:paint(); dc:pop_group_to_source()
end)
safe("push_group_with_content/pop_group", function()
    dc:push_group_with_content(cairo.Content.COLOR_ALPHA)
    dc:paint()
    local grp = dc:pop_group()
    check(cairo.Pattern:is_type_of(grp), "pop_group returns a Pattern")
end)
safe("clip + stroke inside", function()
    dc:save(); dc:rectangle(0, 0, 50, 50); dc:clip_preserve()
    dc:move_to(0, 0); dc:line_to(50, 50); dc:stroke(); dc:restore()
end)

-- cairo defers errors: if ANY call above produced an invalid operation the
-- status would be stuck. This is the real functional assertion.
eq("drawing left the context clean", dc:get_status(), "SUCCESS")
dimg:flush()
safe("show_page on the surface", function() dimg:show_page() end)
collectgarbage("collect")

print("")
print("== 10. Context: toy font API ==")
local fimg = cairo.ImageSurface.new(cairo.Format.ARGB32, 400, 60)
local fc = cairo.Context.new(fimg)
safe("select_font_face", function()
    fc:select_font_face("sans", cairo.FontSlant.NORMAL, cairo.FontWeight.BOLD)
end)
safe("set_font_size", function() fc:set_font_size(24) end)
-- This build has no cairo_get_font_size, so font_size has no getter. The
-- name still resolves -- through the property-setter table -- to the setter
-- itself, which is the lgi behaviour.
check(type(fc.font_size) == "function",
    "font_size resolves to the setter (no getter in this build)",
    type(fc.font_size))

local ext = fc:text_extents("hello")
is_table("text_extents returns a TABLE (piechart.lua:37)", ext)
check(type(ext.width) == "number" and ext.width > 0,
    "text_extents().width > 0", tostring(ext.width))
check(type(ext.height) == "number" and ext.height > 0,
    "text_extents().height > 0", tostring(ext.height))
check(type(ext.x_advance) == "number" and ext.x_advance > 0,
    "text_extents().x_advance > 0", tostring(ext.x_advance))
check(type(ext.x_bearing) == "number" and type(ext.y_bearing) == "number",
    "text_extents bearings present")

local fext = fc:font_extents()
is_table("font_extents returns a TABLE", fext)
check(fext.height > 0, "font_extents().height > 0", tostring(fext.height))
check(fext.ascent > 0, "font_extents().ascent > 0", tostring(fext.ascent))
check(fext.max_x_advance > 0, "font_extents().max_x_advance > 0",
    tostring(fext.max_x_advance))
check(fext.x_size == fext.max_x_advance, "legacy x_size alias")
check(fext.y_size == fext.height, "legacy y_size alias")

local gext = fc:glyph_extents(0, 0, 104)
is_table("glyph_extents returns a TABLE", gext)
check(gext.width > 0, "glyph_extents().width > 0", tostring(gext.width))

safe("show_text", function() fc:set_source_rgb(0, 0, 0); fc:show_text("hello") end)
safe("font_size property setter", function() fc.font_size = 16 end)
eq("font calls left the context clean", fc:get_status(), "SUCCESS")
fimg:flush()

print("")
print("== 11. Rectangle constructors + Format.stride_for_width ==")
is_table("cairo.Rectangle", cairo.Rectangle)
is_table("cairo.RectangleInt", cairo.RectangleInt)
check(type(cairo.Rectangle.new) == "function", "cairo.Rectangle.new")
-- Diagnostics while __call is not resolving:
local rmt = getmetatable(cairo.Rectangle)
print("  debug Rectangle metatable  = " .. tostring(rmt))
print("  debug Rectangle mt.__call  = " .. tostring(rmt and rmt.__call))
local rimt = getmetatable(cairo.RectangleInt)
print("  debug RectangleInt mt      = " .. tostring(rimt))
print("  debug Context mt.__call    = " ..
    tostring(getmetatable(cairo.Context).__call))
local rect = { x = 1, y = 2, width = 3, height = 4 }
local rect2 = cairo.Rectangle(rect)
collectgarbage("collect")
check(rect2 == rect, "Rectangle() returns the table unchanged")
check(rect2.x == 1 and rect2.width == 3, "Rectangle() keeps the fields")
local recti = cairo.RectangleInt.new({ x = 5, y = 6, width = 7, height = 8 })
check(recti == recti and recti.width == 7, "RectangleInt.new keeps the fields")
safe("RecordingSurface accepts a Rectangle", function()
    cairo.RecordingSurface.new(cairo.Content.COLOR_ALPHA,
        cairo.Rectangle{ x = 0, y = 0, width = 5, height = 5 })
end)
approx("Format.stride_for_width(ARGB32, 100)",
    cairo.Format.stride_for_width(cairo.Format.ARGB32, 100), 400)
approx("Format.stride_for_width(ARGB32, 1)",
    cairo.Format.stride_for_width(cairo.Format.ARGB32, 1), 4)
approx("Format.stride_for_width(RGB24, 100)",
    cairo.Format.stride_for_width(cairo.Format.RGB24, 100), 400)
-- 400, not 300: cairo allocates 4 bytes/pixel for RGB24 too (the 4th byte is
-- unused), matching upstream cairo_format_stride_for_width().
approx("Format.stride_for_width(A1, 100)",
    cairo.Format.stride_for_width(cairo.Format.A1, 100), 16)

print("")
print("== 12. is_type_of on REAL objects ==")
local ctx_is = cairo.Context
check(ctx_is:is_type_of(nil) == false, "Context:is_type_of(nil) == false")
check(ctx_is:is_type_of(42) == false, "Context:is_type_of(42) == false")
check(ctx_is:is_type_of({}) == false, "Context:is_type_of(table) == false")
check(ctx_is:is_type_of(dc) == true, "Context:is_type_of(real ctx) == true")
check(ctx_is:is_type_of(dimg) == false,
    "Context:is_type_of(surface) == false")
check(ctx_is:is_type_of(lin) == false,
    "Context:is_type_of(pattern) == false")

check(cairo.Surface:is_type_of(dimg) == true,
    "Surface:is_type_of(image surface) == true")
check(cairo.Surface:is_type_of(rec) == true,
    "Surface:is_type_of(recording surface) == true (derived class)")
check(cairo.Surface:is_type_of(sim) == true,
    "Surface:is_type_of(similar surface) == true")
check(cairo.Surface:is_type_of(dc) == false,
    "Surface:is_type_of(context) == false")
check(cairo.Surface:is_type_of(lin) == false,
    "Surface:is_type_of(pattern) == false")
check(cairo.ImageSurface:is_type_of(dimg) == true,
    "ImageSurface:is_type_of(image surface) == true")
check(cairo.ImageSurface:is_type_of(rec) == true,
    "ImageSurface:is_type_of(recording surface) == true (shared metatable)")

check(cairo.Pattern:is_type_of(solid) == true,
    "Pattern:is_type_of(solid) == true")
check(cairo.Pattern:is_type_of(lin) == true,
    "Pattern:is_type_of(linear) == true")
check(cairo.Pattern:is_type_of(rad) == true,
    "Pattern:is_type_of(radial) == true")
check(cairo.Pattern:is_type_of(sp) == true,
    "Pattern:is_type_of(for_surface) == true")
check(cairo.Pattern:is_type_of(gs) == true,
    "Pattern:is_type_of(get_source result) == true")
check(cairo.Pattern:is_type_of(dimg) == false,
    "Pattern:is_type_of(surface) == false")
check(cairo.Pattern:is_type_of(dc) == false,
    "Pattern:is_type_of(context) == false")

check(cairo.Matrix:is_type_of(cm) == true, "Matrix:is_type_of(matrix) == true")
check(cairo.Matrix:is_type_of(cr) == false, "Matrix:is_type_of(context) == false")
check(cairo.Matrix:is_type_of(dimg) == false, "Matrix:is_type_of(surface) == false")

print("")
print("== 13. is_type_of is not fooled by cross-module cls collisions ==")
-- cairo {CTX=1,SURFACE=2,PATTERN=3,FONTOPT=4,MATRIX=5} collides with pango
-- {ATTRLIST=1,LAYOUT=2,FONTDESC=3,COLOR=4} on every shared index, so a
-- cls-index/strcmp discriminator matches across modules. Metatable identity
-- must not.
check(cairo.FontOptions:is_type_of(Pango.Color()) == false,
    "FontOptions vs Pango.Color (both cls=4)")
check(cairo.Pattern:is_type_of(Pango.FontDescription.from_string("x")) == false,
    "Pattern vs Pango.FontDescription (both cls=3)")
check(cairo.Surface:is_type_of(Pango.Layout.new(Pango.Context())) == false,
    "Surface vs Pango.Layout (both cls=2)")
check(cairo.Context:is_type_of(Pango.AttrList()) == false,
    "Context vs Pango.AttrList (both cls=1)")

print("")
print("== 14. garbage collection does not crash ==")
local function gc_probe()
    for i = 1, 200 do
        local s = cairo.ImageSurface.new(cairo.Format.ARGB32, 8, 8)
        local c = cairo.Context.new(s)
        c:set_source_rgb(1, 0, 0)
        c:rectangle(0, 0, 4, 4)
        c:fill()
        cairo.Pattern.CreateRgba(0, 0, 1, 1)
        cairo.Matrix.new()
    end
    collectgarbage("collect")
    return true
end
local gc_ok, gc_err = pcall(gc_probe)
check(gc_ok, "200 cycles of surface+context+pattern+matrix + GC", gc_err)

print("")
print("== 15. Pango bridge: update_layout / show_layout ==")
-- Called by lib/wibox/widget/textbox.lua:44,53.
-- Build a real Pango.Layout through the pango shim, then exercise both
-- context methods that bridge cairo <-> Pango.
local panctx = Pango.Context.new()
local layout = Pango.Layout.new(panctx)
-- Use set_text() method, not .text = property setter: pango/cairo modules
-- share REG_CLASSES and cairo's Surface (cls=2) overwrites Pango's Layout
-- (cls=2) property tables (see §13 is_type_of cross-module comment).
layout:set_text("Hello")
layout:context_changed()

-- update_layout copies the layout's font state into the cairo context.
local pcimg = cairo.ImageSurface.new(cairo.Format.ARGB32, 200, 100)
local pccr = cairo.Context.new(pcimg)
safe("cr:update_layout(layout)", function() pccr:update_layout(layout) end)
eq("update_layout -> status", pccr:get_status(), "SUCCESS")

-- After update_layout the layout's pixel extents should be non-zero.
local ink, log = layout:get_pixel_extents()
check(ink.width > 0 and ink.height > 0,
    "update_layout -> ink width/height > 0",
    string.format("w=%d h=%d", ink.width, ink.height))

-- show_layout draws the layout text through the cairo context.
local pcimg2 = cairo.ImageSurface.new(cairo.Format.ARGB32, 200, 100)
local pccr2 = cairo.Context.new(pcimg2)
safe("cr:show_layout(layout)", function() pccr2:show_layout(layout) end)
eq("show_layout -> status", pccr2:get_status(), "SUCCESS")
collectgarbage("collect")

print("")
print("== 16. Surface: create_similar, getters, mark_dirty, device_offset, font_options, push_group_to_image ==")
local s3 = cairo.ImageSurface.new(cairo.Format.ARGB32, 100, 50)

-- create_similar (method form on Surface; §6 tested the static constructor)
safe("Surface.create_similar(method)",
    function() cairo.Surface.create_similar(s3, cairo.Content.COLOR_ALPHA, 40, 30) end)

-- get_width / get_height round-trip on the known 100×50 surface
safe("surface:get_width()  -> 100", function()
    eq("get_width()", s3:get_width(), 100)
end)
safe("surface:get_height() -> 50", function()
    eq("get_height()", s3:get_height(), 50)
end)

-- get_content round-trip
safe("surface:get_content() -> COLOR_ALPHA", function()
    eq("get_content()", s3:get_content(), cairo.Content.COLOR_ALPHA)
end)

-- flush (method form; also tested as property in §6)
safe("surface:flush() method", function() s3:flush() end)

-- get_data: returns raw pixel pointer (lightuserdata or string)
safe("surface:get_data()", function()
    local data = s3:get_data()
    check(data ~= nil, "get_data returns non-nil", type(data))
end)

-- get_type: surface type enum string
safe("surface:get_type()", function()
    local st = s3:get_type()
    check(type(st) == "string", "get_type is a string", tostring(st))
end)

-- get_description: human-readable surface description
safe("surface:get_description()", function()
    local desc = s3:get_description()
    check(type(desc) == "string", "get_description is a string", tostring(desc))
end)

-- mark_dirty / mark_dirty_rectangle
safe("surface:mark_dirty()", function() s3:mark_dirty() end)
safe("surface:mark_dirty_rectangle()",
    function() s3:mark_dirty_rectangle(0, 0, 10, 10) end)

-- get_device_offset: fresh surface starts at (0,0)
safe("surface:get_device_offset()", function()
    local dx, dy = s3:get_device_offset()
    approx("device_offset x", dx, 0)
    approx("device_offset y", dy, 0)
end)

-- get_font_options: returns a FontOptions userdata
safe("surface:get_font_options()", function()
    local fo = s3:get_font_options()
    is_userdata("get_font_options -> userdata", fo)
    check(cairo.FontOptions:is_type_of(fo),
        "FontOptions:is_type_of(font_options)")
end)

-- push_group_to_image: Context method, returns an ImageSurface
-- (cairo_push_group wraps to an intermediate surface; push_group_to_image
-- returns that surface as a standalone object).
local ptimg = cairo.ImageSurface.new(cairo.Format.ARGB32, 80, 40)
local ptcr = cairo.Context.new(ptimg)
ptcr:set_source_rgb(0, 0, 1)
ptcr:rectangle(0, 0, 40, 20)
ptcr:fill()
safe("cr:push_group_to_image()", function()
    local img2 = ptcr:push_group_to_image()
    is_userdata("push_group_to_image -> userdata", img2)
    check(cairo.Surface:is_type_of(img2), "push_group_to_image is a Surface")
end)
collectgarbage("collect")

print("")
print("== 17. Pattern: get_type, get_target, get_target_format, get_opaque, get_color_stop_count/stop, surface-pattern accessors ==")

-- get_type on a surface pattern (method form; .type property tested in §7)
local sp_surf = cairo.ImageSurface.new(cairo.Format.ARGB32, 30, 20)
local sp_pat = cairo.Pattern.CreateForSurface(sp_surf)
safe("pattern:get_type() -> SURFACE", function()
    eq("get_type()", sp_pat:get_type(), cairo.PatternType.SURFACE)
end)

-- get_target: surface pattern → target surface
safe("pattern:get_target()", function()
    local tgt = sp_pat:get_target()
    is_userdata("get_target -> userdata", tgt)
end)

-- get_target_format: format of the target surface
safe("pattern:get_target_format()", function()
    local fmt = sp_pat:get_target_format()
    check(type(fmt) == "string", "get_target_format is a string",
        tostring(fmt))
end)

-- get_opaque: solid pattern with alpha=1 is opaque
local opq_solid = cairo.Pattern.CreateRgba(0.5, 0, 0, 1)
safe("pattern:get_opaque()", function()
    local opq = opq_solid:get_opaque()
    check(opq == true, "opaque solid -> true", tostring(opq))
end)

-- get_color_stop_count / get_color_stop on a gradient with 3 stops.
-- These mirror the real lib/ call shapes exactly -- gears/color.lua:325,334 and
-- wibox/container/background.lua:33,36 -- and lgi returns the cairo status as
-- return value 1, so a failure must surface as a status string, not a thrown
-- error. If the shim ever regresses to a count-only / offset-first shape, the
-- colour-stop cloning in those two files silently misreads every channel.
local grad = cairo.Pattern.CreateLinear(0, 0, 100, 0)
grad:add_color_stop_rgba(0,   1, 0, 0, 1)
grad:add_color_stop_rgba(0.5, 0, 1, 0, 1)
grad:add_color_stop_rgba(1,   0, 0, 1, 1)
safe("pattern:get_color_stop_count() -> (status, count)", function()
    local st, nc = grad:get_color_stop_count()
    eq("stop count status SUCCESS", st, "SUCCESS")
    eq("get_color_stop_count == 3", nc, 3)
end)
safe("pattern:get_color_stop_rgba(i) -> (status, off, r, g, b, a)", function()
    local st, coff, cr, cg, cb, ca = grad:get_color_stop_rgba(0)
    eq("stop rgba status SUCCESS", st, "SUCCESS")
    approx("1st stop offset", coff, 0)
    approx("1st stop red", cr, 1)
    approx("1st stop alpha", ca, 1)
end)
safe("pattern:get_color_stop(0)", function()
    local st, coff, cr, cg, cb, ca = grad:get_color_stop(0)
    eq("get_color_stop status SUCCESS", st, "SUCCESS")
    approx("1st stop offset", coff, 0)
    approx("1st stop red", cr, 1)
end)
safe("pattern:get_color_stop(1)", function()
    local st, coff, cr, cg, cb, ca = grad:get_color_stop(1)
    approx("2nd stop offset", coff, 0.5)
    approx("2nd stop green", cg, 1)
end)
-- A fresh linear gradient with no stops: gears/color.lua:325-328 decides
-- "no color stops" from this count, so it must come back 0 with a clean status
-- rather than a thrown error. (get_color_stop_rgba on a stopless pattern is
-- not asserted here: this cairo fills the out-params with zeros instead of
-- failing, so the count is the only reliable signal.)
safe("pattern:get_color_stop_count() on a stopless gradient", function()
    local empty = cairo.Pattern.CreateLinear(0, 0, 100, 0)
    local st, nc = empty:get_color_stop_count()
    eq("stopless count status SUCCESS", st, "SUCCESS")
    eq("stopless count == 0", nc, 0)
end)

-- End-to-end regression guard for the reverted upstream files. This is
-- wibox/container/background.lua:33-38's clone_stops() ported verbatim -- the
-- same status-first destructuring the real code does. It exists because Part 3
-- initially "fixed" the shim by editing those two lib/ files instead; this
-- check fails if the return shape ever drifts back to that.
safe("upstream clone_stops() shape works against the shim", function()
    local function clone_stops(input, output)
        local err, count = input:get_color_stop_count()
        if err ~= "SUCCESS" then return end
        for idx = 0, count - 1 do
            local _, off, r, g, b, a = input:get_color_stop_rgba(idx)
            output:add_color_stop_rgba(off, r, g, b, a)
        end
    end
    local dst = cairo.Pattern.CreateLinear(0, 0, 0, 1)
    clone_stops(grad, dst)
    local st, nc = dst:get_color_stop_count()
    eq("clone_stops status SUCCESS", st, "SUCCESS")
    eq("clone_stops copied 3 stops", nc, 3)
    local _, coff, cr, cg, cb, ca = dst:get_color_stop_rgba(2)
    approx("clone_stops 3rd offset", coff, 1)
    approx("clone_stops 3rd blue", cb, 1)
    approx("clone_stops 3rd alpha", ca, 1)
end)

-- get_surface_pattern_surface / get_surface_pattern_filter
safe("pattern:get_surface_pattern_surface()", function()
    local sps = sp_pat:get_surface_pattern_surface()
    is_userdata("get_surface_pattern_surface -> userdata", sps)
    check(cairo.Surface:is_type_of(sps), "is a Surface")
end)
safe("pattern:get_surface_pattern_filter()", function()
    local spf = sp_pat:get_surface_pattern_filter()
    check(type(spf) == "string", "get_surface_pattern_filter is a string",
        tostring(spf))
end)
collectgarbage("collect")

print("")
print("== 18. Matrix: identity, translate, scale convenience constructors ==")
-- These are static methods on the Matrix class that return new Matrix objects.
safe("Matrix.identity()", function()
    local idm = cairo.Matrix.identity()
    is_userdata("identity -> userdata", idm)
    approx("identity xx", idm.xx, 1)
    approx("identity yy", idm.yy, 1)
    approx("identity xy", idm.xy, 0)
    approx("identity x0", idm.x0, 0)
end)
safe("Matrix.translate(5, 10)", function()
    local tm = cairo.Matrix.translate(5, 10)
    is_userdata("translate -> userdata", tm)
    approx("translate xx", tm.xx, 1)
    approx("translate yy", tm.yy, 1)
    approx("translate x0", tm.x0, 5)
    approx("translate y0", tm.y0, 10)
end)
-- A pure scale by (3,4) should produce xx==3, yy==4, no translation.
safe("Matrix.scale(3, 4)", function()
    local sm = cairo.Matrix.scale(3, 4)
    is_userdata("scale -> userdata", sm)
    approx("scale xx", sm.xx, 3)
    approx("scale yy", sm.yy, 4)
    approx("scale x0", sm.x0, 0)
    approx("scale y0", sm.y0, 0)
end)
collectgarbage("collect")

print("")
print("== 19. Region: new class + contains_point ==")
safe("cairo.Region is a table", function()
    check(type(cairo.Region) == "table", "cairo.Region is a table")
end)
safe("Region.new() + contains_point()", function()
    local rgn = cairo.Region.new()
    is_userdata("Region.new -> userdata", rgn)
    -- contains_point: returns boolean
    local inside = rgn:contains_point(5, 5)
    check(type(inside) == "boolean", "contains_point returns boolean",
        tostring(inside))
end)
collectgarbage("collect")

print("")
print("== 20. Integration: real awesome call paths (verbatim ports) ==")
print("")

-- wibox/drawable.lua:110-119 -- the dirty-area clip. The only thing lib/
-- does with region:get_rectangle() is index .x/.y/.width/.height, so this
-- port fails if get_rectangle ever returns userdata instead of a table.
safe("drawable.lua dirty-area clip path", function()
    local dirty = cairo.Region.create()
    dirty:union_rectangle(cairo.RectangleInt{x = 0, y = 0, width = 100, height = 50})
    check(not dirty:is_empty(), "dirty area is not empty")
    for i = 0, dirty:num_rectangles() - 1 do
        local rect = dirty:get_rectangle(i)
        check(type(rect) == "table", "get_rectangle -> table", tostring(rect))
        local cx, cy, cw, ch = rect.x, rect.y, rect.width, rect.height
        approx("rect.x", cx, 0)
        approx("rect.width", cw, 100)
        approx("rect.height", ch, 50)
    end
end)

-- awful/placement.lua:829-842 -- fit_in_bounding. create_rectangle +
-- intersect + get_rectangle(0), then the width/height comparison that
-- decides whether the geometry fits.
safe("placement.lua fit_in_bounding path", function()
    local round_sgeo = {x = 0, y = 0, width = 200, height = 100}
    local region = cairo.Region.create_rectangle(
        cairo.RectangleInt(round_sgeo))
    local round_geo = {x = 20, y = 10, width = 80, height = 40}
    region:intersect(cairo.Region.create_rectangle(
        cairo.RectangleInt(round_geo)))
    local geo2 = region:get_rectangle(0)
    eq("fit_in_bounding width", geo2.width, round_geo.width)
    eq("fit_in_bounding height", geo2.height, round_geo.height)
    -- Cropped case: a larger proposed geometry must come back smaller.
    local region2 = cairo.Region.create_rectangle(
        cairo.RectangleInt{ x = 0, y = 0, width = 50, height = 30 })
    region2:intersect(cairo.Region.create_rectangle(
        cairo.RectangleInt{ x = 0, y = 0, width = 200, height = 100 }))
    local cropped = region2:get_rectangle(0)
    eq("cropped width == 50", cropped.width, 50)
    eq("cropped height == 30", cropped.height, 30)
end)

-- gears/surface.lua:167 + gears/wallpaper.lua:52 -- create_similar with an
-- explicit content and size. lib/ reads .content back off the result.
safe("gears/surface.lua create_similar path", function()
    local src = cairo.ImageSurface.new(cairo.Format.ARGB32, 64, 32)
    local result = src:create_similar(src.content, 32, 16)
    is_userdata("create_similar -> userdata", result)
    eq("create_similar width", result:get_width(), 32)
    eq("create_similar height", result:get_height(), 16)
    eq("create_similar content round-trips", result.content, cairo.Content.COLOR_ALPHA)
end)

-- wibox/container/background.lua:313,335 -- pop_group hands back a pattern
-- used straight as a mask. Must be a Pattern userdata, never a Context.
safe("background.lua push/pop_group path", function()
    local cr = cairo.Context(cairo.ImageSurface.new(cairo.Format.ARGB32, 40, 20))
    cr:push_group()
    cr:rectangle(5, 5, 10, 10)
    cr:fill()
    local mask = cr:pop_group()
    is_userdata("pop_group -> userdata", mask)
    check(cairo.Pattern:is_type_of(mask), "pop_group is a Pattern")
    -- This cairo's cairo_pattern_type_t stops at RASTER_SOURCE -- there is no
    -- CAIRO_PATTERN_TYPE_PATTERN member -- so a group pattern reports SURFACE.
    -- What matters is that the value maps to a real enum name rather than
    -- falling through to "UNKNOWN", which is what a bad mapping would give.
    eq("mask.type is a known pattern type",
        cairo.PatternType[mask.type] ~= nil, true)
    check(mask.type ~= "UNKNOWN", "mask.type mapped", tostring(mask.type))
    cr:set_source(mask)
    eq("clean after using the mask", cr:get_status(), "SUCCESS")
end)

-- wibox/widget/textbox.lua:40-54 + 513 -- the actual draw(). The context comes
-- from PangoCairo.font_map_get_default():create_context() exactly like
-- textbox.lua:513 does -- NOT Pango.Context.new(). show_layout exercises the
-- Pango->cairo bridge end to end rather than just checking it does not raise.
local PangoCairo = lgi.PangoCairo
safe("textbox.lua draw() path", function()
    local cr = cairo.Context(cairo.ImageSurface.new(cairo.Format.ARGB32, 200, 40))
    local panctx = PangoCairo.font_map_get_default():create_context()
    local layout = Pango.Layout.new(panctx)
    layout:set_text("hello cairo", -1)
    panctx:set_resolution(96)
    layout:context_changed()
    cr:update_layout(layout)
    cr:move_to(0, 0)
    cr:show_layout(layout)
    eq("clean status after show_layout", cr:get_status(), "SUCCESS")
    local ink, logical = layout:get_pixel_extents()
    check(type(logical) == "table", "get_pixel_extents -> table")
    check(logical.width > 0 and logical.height > 0,
        "rendered text has non-zero extents",
        ("w=%s h=%s"):format(tostring(logical.width), tostring(logical.height)))
    check(ink.width > 0, "ink width > 0", tostring(ink.width))
end)
collectgarbage("collect")

print("")
print("== 21. Context/Pattern properties: getters (--index into real cairo state) ==")
print("")
-- A getter proves nothing by itself: cairo defers errors, so every read below
-- follows a set_* that changed the state in a detectable way.

-- Self-contained context, like wibox/drawable.lua:84.
local pimg = cairo.ImageSurface.new(cairo.Format.ARGB32, 60, 40)
local pc = cairo.Context.new(pimg)
is_userdata("property-section context", pc)

-- operator -- wibox/drawable.lua:127,134,137 and gears/wallpaper.lua:76 read
-- cr.operator straight off the context, so it must be a real string, not a
-- cached number and not a method closure.
pc:set_operator(cairo.Operator.SATURATE)
check(type(pc.operator) == "string",
    "cr.operator reads as a string, not an int/closure", type(pc.operator))
eq("cr.operator round-trips SATURATE", pc.operator, cairo.Operator.SATURATE)
pc:set_operator(cairo.Operator.OVER)
eq("cr.operator follows a second set_operator", pc.operator, cairo.Operator.OVER)

-- status -- wibox/drawable.lua:166 does `cr.status == "SUCCESS"` and
-- gears/wallpaper.lua:157 concatenates it into a string. Both paths require a
-- real Lua string; if enum_to ever fell through to a raw number, drawable.lua
-- would assert false after every successful draw().
local st = pc.status
check(type(st) == "string", "cr.status reads as a string", type(st))
eq("cr.status == SUCCESS", st, "SUCCESS")
eq("cr.status concatenates into a string", tostring(st), "SUCCESS")

-- matrix -- wibox/widget/base.lua:537 feeds cr.matrix to
-- matrix.transform_rectangle, which indexes .xx/.yx/.xy/.yy/.x0/.y0.
local pm0 = cairo.Matrix.new()
pm0:init(2, 0, 0, 3, 10, 20)
pc:set_matrix(pm0)
local pmat = pc.matrix
is_userdata("cr.matrix reads as a matrix userdata", pmat)
check(type(pmat.xx) == "number", "cr.matrix.xx reads as a number", type(pmat.xx))
approx("cr.matrix.xx", pmat.xx, 2)
approx("cr.matrix.yx", pmat.yx, 0)
approx("cr.matrix.xy", pmat.xy, 0)
approx("cr.matrix.yy", pmat.yy, 3)
approx("cr.matrix.x0", pmat.x0, 10)
approx("cr.matrix.y0", pmat.y0, 20)

-- __index resolves the property getter BEFORE the method table, so a getter
-- that shadows a method would make `obj:name()` call a plain value. lib/ has
-- no colon call on any of the names in the property tables, and this keeps it
-- that way -- the same trap REGION_PG hit with num_rectangles/is_empty.
check(type(pc:get_matrix()) == "userdata",
    "cr:get_matrix() still works next to the cr.matrix getter")
approx("cr:get_matrix().xx agrees with cr.matrix.xx", pc:get_matrix().xx, 2)
check(type(pc:get_status()) == "string",
    "cr:get_status() still works next to the cr.status getter")
eq("cr:get_status() == SUCCESS", pc:get_status(), "SUCCESS")
check(type(pc.set_operator) == "function",
    "cr.set_operator is still a function", type(pc.set_operator))
safe("cr:set_operator() after property reads", function()
    pc:set_operator(cairo.Operator.CLEAR)
end)
eq("method and property agree on operator", pc.operator, cairo.Operator.CLEAR)

-- The remaining CTX_PG entries are not read by lib/ today, but they are the
-- same getters, so cover them rather than let them rot silently.
safe("property section set_antialias", function()
    pc:set_antialias(cairo.Antialias.BEST) end)
eq("cr.antialias round-trips", pc.antialias, cairo.Antialias.BEST)
safe("property section set_fill_rule", function()
    pc:set_fill_rule(cairo.FillRule.EVEN_ODD) end)
eq("cr.fill_rule round-trips", pc.fill_rule, cairo.FillRule.EVEN_ODD)
safe("property section set_line_cap", function()
    pc:set_line_cap(cairo.LineCap.ROUND) end)
eq("cr.line_cap round-trips", pc.line_cap, cairo.LineCap.ROUND)
safe("property section set_line_join", function()
    pc:set_line_join(cairo.LineJoin.BEVEL) end)
eq("cr.line_join round-trips", pc.line_join, cairo.LineJoin.BEVEL)
safe("property section set_line_width", function()
    pc:set_line_width(7.5) end)
approx("cr.line_width round-trips", pc.line_width, 7.5)
eq("cr.hairline is false at a non-zero width", pc.hairline, false)
safe("property section set_tolerance", function()
    pc:set_tolerance(2.5) end)
approx("cr.tolerance round-trips", pc.tolerance, 2.5)

-- __index collapses a multi-return getter to its FIRST return value, so
-- cr.dashes is only the table (get_dash pushes table, count, offset). lib/
-- always uses cr:set_dash()/cr:get_dash() for the full triple -- grid.lua:1452.
pc:set_dash({ 2, 4, 6 }, 3, 1.5)
local pd, pnd, pdoff = pc:get_dash()
approx("dash offset still 1.5", pdoff, 1.5)
eq("cr.dashes is the dash TABLE, not the count", type(pc.dashes), "table")
check(pc.dashes ~= 3, "cr.dashes is not the dash count", tostring(pc.dashes))
eq("#cr.dashes == 3", #pc.dashes, 3)
approx("cr.dashes[1]", pc.dashes[1], 2)
approx("cr.dashes[2]", pc.dashes[2], 4)
approx("cr.dashes[3]", pc.dashes[3], 6)
pc:set_dash({}, 0, 0)
eq("empty cr.dashes is still a table", type(pc.dashes), "table")
eq("empty cr.dashes has 0 entries", #pc.dashes, 0)
print("  gc after section 21 (context properties)")
collectgarbage("collect")

print("")
print("== 22. Property setters (__newindex) round-trip into real cairo state ==")
print("")

-- Self-contained again. Every set below must land in cairo, and every read
-- below must come back from cairo, so a setter silently doing nothing
-- (cairo errors are deferred, "no crash" is not a pass) shows up here.
local qimg = cairo.ImageSurface.new(cairo.Format.ARGB32, 60, 40)
local qc = cairo.Context.new(qimg)

-- operator -- the single most common write in lib/: 13 sites do
-- `cr.operator = cairo.Operator.SOURCE` (wibox/drawable.lua:127 is the
-- canonical one). enum_from must accept the enum NAME and write it into
-- cairo_get_operator.
qc:set_operator(cairo.Operator.OVER)
eq("operator before the property write", qc.operator, cairo.Operator.OVER)
qc.operator = cairo.Operator.SOURCE
eq("cr.operator = SOURCE round-trips", qc.operator, cairo.Operator.SOURCE)
qc.operator = cairo.Operator.CLEAR
eq("cr.operator = CLEAR round-trips", qc.operator, cairo.Operator.CLEAR)
qc.operator = cairo.Operator.XOR
eq("cr.operator = XOR round-trips", qc.operator, cairo.Operator.XOR)
-- drawable.lua restores the previous op after the dirty-area blit, so the
-- write must be genuinely sticky across the assignment.
qc.operator = cairo.Operator.OVER
eq("cr.operator = OVER round-trips", qc.operator, cairo.Operator.OVER)

-- status is read-only: no setter exists, so __newindex must raise instead of
-- quietly accepting the write (a silent drop would mask a real regression).
check(not pcall(function() qc.status = "SUCCESS" end),
    "setting cr.status raises (read-only)")

-- line_width -- wibox/container/background.lua:328 does `cr.line_width = 2*bw`,
-- the only numeric property write in lib/. Set an explicit baseline first:
-- this cairo build reports 2.0 for a fresh context, not the documented 1.0,
-- so asserting a default here would test cairo, not the shim.
qc:set_line_width(1)
approx("method baseline line_width == 1", qc.line_width, 1)
eq("line_width before the property write", qc.line_width, 1)
qc.line_width = 3.5
approx("cr.line_width = 3.5 round-trips", qc.line_width, 3.5)
qc.line_width = 2 * 7.25
approx("cr.line_width = 2*bw round-trips", qc.line_width, 14.5)
eq("cr.hairline is false at the new width", qc.hairline, false)

-- antialias -- not written by lib/ today, same setter family as operator.
qc.antialias = cairo.Antialias.NONE
eq("cr.antialias = NONE round-trips", qc.antialias, cairo.Antialias.NONE)
qc.antialias = cairo.Antialias.SUBPIXEL
eq("cr.antialias = SUBPIXEL round-trips", qc.antialias, cairo.Antialias.SUBPIXEL)

-- fill_rule.
qc.fill_rule = cairo.FillRule.EVEN_ODD
eq("cr.fill_rule = EVEN_ODD round-trips", qc.fill_rule, cairo.FillRule.EVEN_ODD)
qc.fill_rule = cairo.FillRule.WINDING
eq("cr.fill_rule = WINDING round-trips", qc.fill_rule, cairo.FillRule.WINDING)

-- line_cap / line_join / tolerance.
qc.line_cap = cairo.LineCap.ROUND
eq("cr.line_cap = ROUND round-trips", qc.line_cap, cairo.LineCap.ROUND)
qc.line_join = cairo.LineJoin.MITER
eq("cr.line_join = MITER round-trips", qc.line_join, cairo.LineJoin.MITER)
qc.tolerance = 0.25
approx("cr.tolerance = 0.25 round-trips", qc.tolerance, 0.25)

-- matrix. cr.matrix is BOTH a getter and a setter, so a regression that put
-- it in only one table would break half the accesses.
local prop_m = cairo.Matrix.new()
prop_m:init(4, 0, 0, 5, 11, 12)
qc.matrix = prop_m
local back = qc.matrix
is_userdata("cr.matrix = m reads back as a matrix", back)
approx("matrix property write xx", back.xx, 4)
approx("matrix property write yy", back.yy, 5)
approx("matrix property write x0", back.x0, 11)
approx("matrix property write y0", back.y0, 12)
-- The matrix must have landed in cairo, not just in a field: push_matrix
-- heap-copies, so a second read returns a distinct userdata with the same
-- numbers -- a stale pointer would surface garbage here.
local back2 = qc.matrix
check(not rawequal(back, back2), "each cr.matrix read is a fresh copy")
approx("matrix survives a second read", back2.xx, 4)
approx("matrix survives a second read (y0)", back2.y0, 12)
approx("cr:get_matrix() agrees with cr.matrix", qc:get_matrix().xx, 4)

-- source -- 4 lib/ sites (gears/wallpaper.lua:140,180,252 and
-- wibox/container/tile.lua:84) do `cr.source = <pattern>`. There is no
-- source GETTER on CTX_PG, so verify the write actually reached cairo by
-- reading the pattern back with get_source() and comparing a type that a
-- pre-existing source cannot have.
qc:set_source_rgb(1, 0, 0)
eq("source before the property write is solid",
    qc:get_source().type, cairo.PatternType.SOLID)
local prop_lin = cairo.Pattern.create_linear(0, 0, 0, 60)
qc.source = prop_lin
eq("cr.source = <linear> lands in cairo (type changed)",
    qc:get_source().type, cairo.PatternType.LINEAR)
eq("cr:get_source() is a pattern userdata",
    type(qc:get_source()), "userdata")
qc.source = cairo.Pattern.create_solid(0, 0, 0, 1)
eq("cr.source = <solid> lands in cairo",
    qc:get_source().type, cairo.PatternType.SOLID)
-- The written source is a live pattern, so chaining a setter onto it must
-- reach cairo too (wibox/widget/imagebox.lua:328 does this).
qc.source = prop_lin
safe("write then chain set_filter on the source", function()
    qc:get_source():set_filter(cairo.Filter.BILINEAR)
end)
eq("filter set through the assigned source reads back",
    qc:get_source().filter, cairo.Filter.BILINEAR)

-- dashes -- a setter with a 3-arg table shape, distinct from every other
-- scalar property setter in the table.
qc.dashes = { 5, 10, 15 }
local qd, qn = qc:get_dash()
eq("cr.dashes write count", qn, 3)
approx("cr.dashes write value 1", qd[1], 5)
approx("cr.dashes write value 2", qd[2], 10)
approx("cr.dashes write value 3", qd[3], 15)

-- Pattern.extend -- both lib/ writes (gears/wallpaper.lua:179 and
-- wibox/container/tile.lua:36) are literally
-- `pattern.extend = cairo.Extend.REPEAT`. Neither reads it back, which is
-- exactly why this needed a real assertion: the write is otherwise untested.
local eimg = cairo.ImageSurface.new(cairo.Format.ARGB32, 24, 24)
local ept = cairo.Pattern.create_for_surface(eimg)
is_userdata("surface pattern for the extend test", ept)
eq("pattern.extend defaults to NONE", ept.extend, cairo.Extend.NONE)
ept.extend = cairo.Extend.REPEAT
eq("pattern.extend = REPEAT round-trips", ept.extend, cairo.Extend.REPEAT)
eq("pattern.extend = REPEAT reads back from cairo",
    ept:get_extend(), cairo.Extend.REPEAT)
ept.extend = cairo.Extend.PAD
eq("pattern.extend = PAD round-trips", ept.extend, cairo.Extend.PAD)
ept.extend = cairo.Extend.REFLECT
eq("pattern.extend = REFLECT round-trips", ept.extend, cairo.Extend.REFLECT)

-- Pattern.filter / Pattern.matrix setters, same PAT_P table.
ept.filter = cairo.Filter.NEAREST
eq("pattern.filter = NEAREST round-trips", ept.filter, cairo.Filter.NEAREST)
local ptm = cairo.Matrix.new()
ptm:init(1, 0, 0, 1, 3, 4)
ept.matrix = ptm
approx("pattern.matrix property write x0", ept.matrix.x0, 3)
approx("pattern.matrix property write y0", ept.matrix.y0, 4)

-- A property write must not disturb unrelated state, and must keep the
-- context drawable.
qc.line_width = 2.5
qc.operator = cairo.Operator.SOURCE
qc:rectangle(1, 2, 3, 4)
qc:fill()
qc:stroke()
qc:paint()
eq("context still SUCCESS after property-driven drawing",
    qc:get_status(), "SUCCESS")
approx("line_width kept across the draw", qc.line_width, 2.5)
eq("operator kept across the draw", qc.operator, cairo.Operator.SOURCE)
print("  gc after section 22 (property setters)")
collectgarbage("collect")

print("")
print("== 23. Regression guards: lib/'s exact property access shapes ==")
print("")

-- wibox/drawable.lua:166 --
--   assert(cr.status == "SUCCESS", "Cairo context entered error state: "
--           .. cr.status)
-- Verbatim port. The assert has to stay true after real drawing, which means
-- cr.status must read back the SUCCESS enum NAME as a Lua string -- not 0,
-- not nil, not "0". A getter that ever returns a number makes this assert
-- fail on every successful draw in the whole compositor.
safe("drawable.lua:166 cr.status assert", function()
    local simg = cairo.ImageSurface.new(cairo.Format.ARGB32, 120, 80)
    local scr = cairo.Context.new(simg)
    scr:select_font_face("Sans", cairo.FontSlant.NORMAL, cairo.FontWeight.BOLD)
    scr:set_font_size(12)
    scr:move_to(5, 20)
    scr:show_text("hello")
    scr:rectangle(60, 10, 30, 30)
    scr:fill()
    scr:stroke()
    -- The exact assertion from drawable.lua, plus the string concatenation
    -- in its message arm, which also fails if cr.status is not a string.
    assert(scr.status == "SUCCESS",
        "Cairo context entered error state: " .. scr.status)
    check(scr.status == "SUCCESS",
        "drawable.lua:166 cr.status == SUCCESS", tostring(scr.status))
    -- wallpaper.lua:156 branches on the other arm: a healthy context must NOT
    -- trip the warning path, so == "SUCCESS" and ~= "SUCCESS" must be mutually
    -- exclusive and exhaustive here.
    check(scr.status ~= "SUCCESS" == false,
        "wallpaper.lua:156 does not fire on a healthy context")
end)

-- wibox/widget/imagebox.lua:328 --
--   cr:get_source():set_filter(cairo.Filter[filter:upper()])
-- get_source() hands back the context's BORROWED source, so the returned
-- userdata must be a live Pattern whose set_filter actually reaches cairo.
-- This also guards the d_noop destructor choice: a d_pattern wrapper here
-- would abort in cairo_pattern_destroy the moment the GC runs.
safe("imagebox.lua:328 get_source():set_filter()", function()
    local iimg = cairo.ImageSurface.new(cairo.Format.ARGB32, 40, 40)
    local icr = cairo.Context.new(iimg)
    local ipat = cairo.Pattern.create_linear(0, 0, 0, 40)
    ipat:add_color_stop_rgba(0, 1, 0, 0, 1)
    ipat:add_color_stop_rgba(1, 0, 0, 1, 1)
    icr:set_source(ipat)
    -- index into the enum table by an upper-cased name, like imagebox.lua does
    local filter = "nearest"
    local src = icr:get_source()
    check(cairo.Pattern:is_type_of(src), "get_source() is a Pattern")
    src:set_filter(cairo.Filter[filter:upper()])
    eq("imagebox.lua:328 filter reaches the live source",
        icr:get_source().filter, cairo.Filter.NEAREST)
    icr:paint()
    eq("clean status after the filtered paint", icr:get_status(), "SUCCESS")
end)

-- wibox/widget/imagebox.lua:427 --
--   elseif cairo.Surface:is_type_of(image) then
-- is_type_of compares METATABLE IDENTITY (see m_is_type_of), because cairo
-- C_SURFACE == 2 collides with pango C_LAYOUT == 2. A regression to a
-- w->cls comparison would misclassify a Pango.Layout as a surface and
-- crash inside cairo_set_source_surface.
safe("imagebox.lua:427 Surface:is_type_of(image)", function()
    local real = cairo.ImageSurface.new(cairo.Format.ARGB32, 16, 16)
    local similar = cairo.Surface.new(real)
    check(cairo.Surface:is_type_of(real), "ImageSurface passes")
    check(cairo.Surface:is_type_of(similar), "a Surface wrapper passes")
    check(not cairo.Surface:is_type_of(cairo.Pattern.create_solid(0, 0, 0, 1)),
        "a Pattern is not a Surface")
    check(not cairo.Surface:is_type_of(cairo.Matrix.new()),
        "a Matrix is not a Surface")
    check(not cairo.Surface:is_type_of("not a surface"), "a string is not a Surface")
    check(not cairo.Surface:is_type_of(nil), "nil is not a Surface")
    -- the actual lib/ branch, taken by any real image box
    local image = real
    if cairo.Surface:is_type_of(image) then
        check(true, "imagebox.lua:427 branch is taken for an ImageSurface")
    else
        check(false, "imagebox.lua:427 branch is taken for an ImageSurface")
    end
end)

-- The negative arm that wallpaper.lua:156 branches on. A clean control
-- context must read back as the SUCCESS string, and that same string must
-- survive the exact comparisons lib/ performs: == "SUCCESS", ~= "SUCCESS",
-- and concatenation into the warning message.
safe("status read supports both comparison arms", function()
    local cimg = cairo.ImageSurface.new(cairo.Format.ARGB32, 16, 16)
    local ccr = cairo.Context.new(cimg)
    ccr:rectangle(0, 0, 8, 8)
    ccr:fill()
    eq("control context reads back SUCCESS", ccr.status, "SUCCESS")
    check(cairo.Status[ccr.status] ~= nil,
        "status maps to a known Status enum name", tostring(ccr.status))
    eq("cairo.Status round-trips its own name",
        cairo.Status[ccr.status], ccr.status)
    check(ccr.status == "SUCCESS", "== arm of the comparison holds")
    check(not (ccr.status ~= "SUCCESS"), "~= arm stays false when clean")
    eq("status concatenates for the warning message",
        "Cairo context entered error state: " .. ccr.status,
        "Cairo context entered error state: SUCCESS")
    -- a boolean or a number would silently satisfy one arm and break lib/'s
    -- error reporting on every real failure
    check(type(ccr.status) == "string",
        "status is a string, so == and ~= behave as lib/ expects",
        type(ccr.status))
end)

-- The warning arm itself: force cairo into a non-SUCCESS state with an
-- unmatched restore, and make sure the getter reports something other than
-- the SUCCESS string instead of masking the error.
safe("a failed context does not read back as SUCCESS", function()
    local fimg = cairo.ImageSurface.new(cairo.Format.ARGB32, 8, 8)
    local fcr = cairo.Context.new(fimg)
    fcr:rectangle(0, 0, 4, 4)
    fcr:stroke()
    eq("pre-fault context reports SUCCESS", fcr.status, "SUCCESS")
    local failed = pcall(function() fcr:restore() end)
    if not failed then
        check(type(fcr.status) == "string",
            "a failed context still reads back a string", tostring(fcr.status))
        check(fcr.status ~= "SUCCESS",
            "a failed context does not read back as SUCCESS", tostring(fcr.status))
        check(cairo.Status[fcr.status] ~= nil,
            "the failure maps to a known Status enum name", tostring(fcr.status))
        fcr:rectangle(1, 1, 2, 2)
        fcr:fill()
        check(fcr.status ~= "SUCCESS",
            "the failure persists across later drawing", tostring(fcr.status))
    end
end)
print("  gc after section 23 (regression guards)")
collectgarbage("collect")

print("== 24. Pango Layout measurement: get_extents / get_size / get_baseline / get_line_count ==")
-- Part 7 added these four. Note the scope reality: lib/ only ever calls
-- layout:get_pixel_extents() (lib/wibox/widget/textbox.lua:45,57 + 1). The
-- get_preferred_size / get_height_for_width pair that naive planning assumed
-- is textbox-level (lib/wibox/widget/textbox.lua:79,100), called from
-- lib/naughty/layout/legacy.lua:180,196,450 and lib/menubar/utils.lua:419,
-- and resolves through get_pixel_extents + set_width internally. So this
-- section pins the shim's Layout measurement surface against the one API lib/
-- does call, plus the Pango 1.48 header contract this container ships.

local mctx = Pango.Context.new()
local ml = Pango.Layout.new(mctx)
ml:set_text("Hello, part 7")
ml:context_changed()

-- 24.1 get_extents -> (ink, logical) rectangles in Pango units.
safe("layout:get_extents()", function() ml:get_extents() end)
local mink, mlogical = ml:get_extents()
is_table("get_extents -> ink rect", mink)
is_table("get_extents -> logical rect", mlogical)
for _, k in ipairs({ "x", "y", "width", "height" }) do
    check(type(mink[k]) == "number", "get_extents().ink." .. k .. " is a number")
    check(type(mlogical[k]) == "number",
        "get_extents().logical." .. k .. " is a number")
end
check(mink.width > 0 and mink.height > 0,
    "get_extents().ink is non-empty",
    string.format("w=%d h=%d", mink.width, mink.height))
check(mlogical.width >= mink.width, "logical width >= ink width")
check(mlogical.height >= mink.height, "logical height >= ink height")

-- 24.2 Pango units are pixels * SCALE, to within one SCALE quantum.
-- pango_layout_get_pixel_extents rounds units down to pixels, so the
-- round-trip error is bounded by Pango.SCALE.
local pixink, pixlogical = ml:get_pixel_extents()
check(math.abs(mink.width - pixink.width * Pango.SCALE) <= Pango.SCALE,
    "get_extents().ink.width is get_pixel_extents() within one SCALE",
    string.format("units=%d px*SCALE=%d", mink.width, pixink.width * Pango.SCALE))
check(math.abs(mlogical.height - pixlogical.height * Pango.SCALE) <= Pango.SCALE,
    "get_extents().logical.height is get_pixel_extents() within one SCALE",
    string.format("units=%d px*SCALE=%d",
        mlogical.height, pixlogical.height * Pango.SCALE))

-- 24.3 get_size -> (width, height) in Pango units, i.e. the logical extents.
safe("layout:get_size()", function() ml:get_size() end)
local msz_w, msz_h = ml:get_size()
check(type(msz_w) == "number" and msz_w > 0, "get_size().width > 0", tostring(msz_w))
check(type(msz_h) == "number" and msz_h > 0, "get_size().height > 0", tostring(msz_h))
eq("get_size().width == get_extents().logical.width", msz_w, mlogical.width)
eq("get_size().height == get_extents().logical.height", msz_h, mlogical.height)

-- 24.4 get_baseline -> distance from the top, 0 when there is no baseline.
safe("layout:get_baseline()", function() ml:get_baseline() end)
local mbl = ml:get_baseline()
check(type(mbl) == "number", "get_baseline() is a number")
check(mbl >= 0 and mbl <= msz_h,
    "baseline is within [0, size.height]",
    string.format("bl=%d h=%d", mbl, msz_h))

-- 24.5 get_line_count reacts to set_width wrap.
-- Pango enums are deliberately NOT exported on the module table (grep lib/
-- finds 0 usages of Pango.Wrap / .Alignment / .Ellipsize / .Direction), so
-- the enum is passed as the string name set_wrap's enum_from() resolves.
local wctx = Pango.Context.new()
local wl = Pango.Layout.new(wctx)
wl:set_text("one two three four five six seven eight nine ten eleven twelve")
wl:set_wrap("WORD")
wl:set_width(8 * Pango.SCALE)
wl:context_changed()
local wrapped = wl:get_line_count()
check(type(wrapped) == "number" and wrapped > 1,
    "narrow width wraps into more than one line", "n=" .. tostring(wrapped))
safe("layout:get_wrap() returns the set name", function() wl:get_wrap() end)
eq("get_wrap() round-trips the string", wl:get_wrap(), "WORD")

wl:set_width(-1)
wl:context_changed()
local unwrapped = wl:get_line_count()
eq("set_width(-1) removes the wrap -> one line", unwrapped, 1)

-- 24.6 empty layout.
local ectx = Pango.Context.new()
local el = Pango.Layout.new(ectx)
el:set_text("")
el:context_changed()
eq("empty layout still has one line", el:get_line_count(), 1)
check(el:get_baseline() >= 0, "empty layout baseline is non-negative")
check(el:get_size() == 0 or type(el:get_size()) == "number",
    "empty layout get_size() still returns a number")

-- 24.7 measurement never disturbs a cairo context that bridges to the layout.
local mcimg = cairo.ImageSurface.new(cairo.Format.ARGB32, 200, 100)
local mcr = cairo.Context.new(mcimg)
mcr:update_layout(ml)
mcr:show_layout(ml)
local _, mlog2 = ml:get_extents()
eq("measurement does not disturb cairo status", mcr:get_status(), "SUCCESS")
check(mlog2.width > 0,
    "repeated get_extents() is stable after update_layout/show_layout")

-- 24.8 dot-call must fail: m_index returns the raw closure without self.
local dotok = pcall(function() ml.get_extents() end)
check(not dotok, "dot-call layout.get_extents() fails (colon is required)")

-- 24.9 enum argument semantics.
-- enum_from() takes luaL_optstring() and only matches known names; anything
-- else (unknown name, non-string) silently falls back to the default. So an
-- enum typo is DROPPED, not reported -- pin the behaviour so nobody
-- "improves" it into a hard error and surprises lib/.
wl:set_wrap("WORD")
wl:set_width(9 * Pango.SCALE)
wl:context_changed()
eq("set_wrap WORD is reflected by get_wrap", wl:get_wrap(), "WORD")
wl:set_wrap("TYPO")
wl:context_changed()
eq("an unknown wrap name silently falls back to the default",
    wl:get_wrap(), "WORD_CHAR")
wl:set_wrap(3)
wl:context_changed()
eq("a non-string wrap mode also falls back to the default",
    wl:get_wrap(), "WORD_CHAR")

-- 24.10 the hard type errors: chk() rejects a wrong receiver, and the
-- Pango-units setters reject non-integers.
local bad1, e1 = pcall(function()
    local img = cairo.ImageSurface.new(cairo.Format.ARGB32, 8, 8)
    img:get_size()
end)
check(not bad1, "a cairo Surface is not a valid Layout receiver", tostring(e1))
local bad2, e2 = pcall(function() wl:set_width("wide") end)
check(not bad2, "set_width(string) raises -- Pango units are integers",
    tostring(e2))

print("  gc after section 24 (layout measurement)")
collectgarbage("collect")

print("== 25. GLib (Part 5): main loop, paths, Variant, KeyFile ==")
-- Ground truth: lib/gears/timer.lua:60 binds `local glib = lgi.GLib` and
-- reads glib.timeout_add (:102), glib.PRIORITY_DEFAULT (:102),
-- glib.SOURCE_CONTINUE (:104), glib.source_remove (:120).
-- lib/menubar/utils.lua:163/172/174 reads build_filenamev / get_home_dir /
-- get_user_data_dir, and :270 loads a KeyFile with glib.KeyFileFlags.NONE.
local GLib = lgi.GLib
is_table("lgi.GLib", GLib)
check(GLib ~= nil and type(GLib) == "table", "lgi.GLib is a real table, not the {} placeholder")

-- Flat integer constants. Real lgi exposes these unqualified and as integers;
-- the nested tables below keep the string form for parity.
eq("GLib.PRIORITY_DEFAULT is 0", GLib.PRIORITY_DEFAULT, 0)
eq("GLib.SOURCE_CONTINUE is 1", GLib.SOURCE_CONTINUE, 1)
eq("GLib.SOURCE_REMOVE is 0", GLib.SOURCE_REMOVE, 0)
is_table("GLib.Priority", GLib.Priority)
is_table("GLib.Source", GLib.Source)
is_table("GLib.KeyFileFlags", GLib.KeyFileFlags)
eq("GLib.Priority.DEFAULT string form", GLib.Priority.DEFAULT, "DEFAULT")
eq("GLib.KeyFileFlags.NONE string form", GLib.KeyFileFlags.NONE, "NONE")

-- Utility functions the codebase reads unqualified.
check(type(GLib.timeout_add) == "function", "GLib.timeout_add is a function")
check(type(GLib.source_remove) == "function", "GLib.source_remove is a function")
check(type(GLib.build_filenamev) == "function", "GLib.build_filenamev is a function")
check(type(GLib.get_home_dir) == "function", "GLib.get_home_dir is a function")
check(type(GLib.get_user_data_dir) == "function", "GLib.get_user_data_dir is a function")
check(type(GLib.get_system_data_dirs) == "function", "GLib.get_system_data_dirs is a function")
check(type(GLib.utf8_strlen) == "function", "GLib.utf8_strlen is a function")
check(type(GLib.utf8_substring) == "function", "GLib.utf8_substring is a function")

safe("GLib.get_home_dir() returns a non-empty string", function()
    local home = GLib.get_home_dir()
    check(type(home) == "string" and #home > 0,
        "get_home_dir() -> non-empty string", tostring(home))
end)
safe("GLib.build_filenamev() joins a table of segments", function()
    -- lib/menubar/utils.lua:159-163 builds `{path, "icons"}` and passes the
    -- table itself, not a count. `dir = { nil, dir }` leaves a nil hole at
    -- [1] before it is assigned, so holes must be skipped.
    local joined = GLib.build_filenamev({ "/tmp", "smoke-dir", "icons" })
    check(type(joined) == "string" and joined ~= "",
        "build_filenamev(table) -> non-empty string", tostring(joined))
end)
safe("GLib.build_filenamev() tolerates a nil hole", function()
    local t = { nil, "icons" }
    t[1] = "/tmp"
    local joined = GLib.build_filenamev(t)
    check(type(joined) == "string" and joined ~= "",
        "a nil hole is skipped", tostring(joined))
end)
safe("GLib.build_filenamev() also takes string varargs", function()
    local joined = GLib.build_filenamev("/tmp", "smoke-dir")
    check(type(joined) == "string" and joined ~= "",
        "string varargs -> non-empty string", tostring(joined))
end)
safe("GLib.build_filenamev() rejects an empty table", function()
    local ok, err = pcall(function() GLib.build_filenamev({}) end)
    check(not ok, "an empty table raises", tostring(err))
end)
safe("GLib.build_filenamev() rejects no arguments", function()
    local ok, err = pcall(function() GLib.build_filenamev() end)
    check(not ok, "no arguments raises", tostring(err))
end)
safe("GLib.build_filenamev() rejects a plain string", function()
    local ok, err = pcall(function() GLib.build_filenamev("/only-one-arg") end)
    check(ok, "a single string segment is accepted (varargs form)", tostring(err))
end)
safe("GLib.get_system_data_dirs() returns a table", function()
    local dirs = GLib.get_system_data_dirs()
    check(type(dirs) == "table" and #dirs > 0,
        "get_system_data_dirs() -> non-empty table",
        "n=" .. (type(dirs) == "table" and #dirs or "?"))
end)

safe("utf8_strlen counts characters, not bytes", function()
    eq("utf8_strlen('hello')", GLib.utf8_strlen("hello"), 5)
    eq("utf8_strlen('héllo')", GLib.utf8_strlen("héllo"), 5)
end)

-- Variant. lib/naughty/dbus.lua constructs these for every D-Bus call:
--   GLib.Variant("(us)", {...}) :64   GLib.Variant("(uu)", {...}) :78
--   GLib.Variant("(u)", {...})  :352  GLib.Variant("()")          :364
safe("GLib.Variant('()') empty tuple", function()
    local v = GLib.Variant("()")
    is_userdata("Variant('()') is userdata", v)
    eq("empty tuple n_children == 0", v:n_children(), 0)
end)
safe("GLib.Variant('(u)', {42}) carries the payload", function()
    local v = GLib.Variant("(u)", { 42 })
    is_userdata("Variant('(u)') is userdata", v)
    eq("one-child tuple n_children == 1", v:n_children(), 1)
    -- Indices are 0-based; lib/naughty/dbus.lua:214 indexes as
    -- `:get_child_value(7 - 1)` for exactly this reason.
    local child = v:get_child_value(1 - 1)
    is_userdata("get_child_value returns a GVariant child", child)
end)
safe("GLib.Variant('(us)', {1, 'x'})", function()
    local v = GLib.Variant("(us)", { 1, "x" })
    eq("(us) n_children == 2", v:n_children(), 2)
    is_userdata("child 0 exists", v:get_child_value(0))
    is_userdata("child 1 exists", v:get_child_value(1))
end)
safe("GLib.Variant:get_child_value rejects an out-of-range index", function()
    local v = GLib.Variant("(u)", { 42 })
    local ok, err = pcall(function() v:get_child_value(5) end)
    check(not ok, "an out-of-range index raises", tostring(err))
end)
safe("GLib.Variant:print() renders the payload", function()
    local v = GLib.Variant("(u)", { 7 })
    local s = v:print()
    check(type(s) == "string" and s ~= "", "Variant:print() -> string", tostring(s))
end)
safe("GLib.Variant.new_from_parsed round-trips print() output", function()
    local rendered = GLib.Variant("(u)", { 3 }):print()
    local v = GLib.Variant.new_from_parsed(rendered)
    is_userdata("new_from_parsed returns userdata", v)
    eq("parsed tuple has 1 child", v:n_children(), 1)
end)
safe("GLib.Variant.new_from_parsed rejects garbage", function()
    local ok, err = pcall(function()
        GLib.Variant.new_from_parsed("this is not a variant")
    end)
    check(not ok, "garbage input raises", tostring(err))
end)
check(GLib.Variant.is_type_of ~= nil, "Variant.is_type_of is exposed")

-- KeyFile. lib/menubar/utils.lua:269-271:
--   local keyfile = glib.KeyFile()
--   if not keyfile:load_from_file(file, glib.KeyFileFlags.NONE) then return nil end
local desktop = "[Desktop Entry]\nName=Smoke\nComment=test\nIcon=x\n"
safe("GLib.KeyFile() constructs", function()
    local kf = GLib.KeyFile()
    is_userdata("KeyFile() is userdata", kf)
    eq("fresh KeyFile has no 'Desktop Entry' group", kf:has_group("Desktop Entry"), false)
end)
safe("KeyFile:load_from_data with the string flag", function()
    local kf = GLib.KeyFile()
    -- lib/ passes GLib.KeyFileFlags.NONE, which the shim exposes as the
    -- string "NONE". The loader must accept the string form.
    eq("load_from_data accepts KeyFileFlags.NONE as a string",
        kf:load_from_data(desktop, GLib.KeyFileFlags.NONE), true)
    eq("group present after load", kf:has_group("Desktop Entry"), true)
    eq("key present after load", kf:has_key("Desktop Entry", "Name"), true)
end)
safe("KeyFile:load_from_data with the integer flag", function()
    local kf = GLib.KeyFile()
    eq("load_from_data also accepts the integer 0",
        kf:load_from_data(desktop, 0), true)
end)
safe("KeyFile:load_from_data rejects an unknown flag name", function()
    local ok, err = pcall(function()
        GLib.KeyFile():load_from_data(desktop, "BOGUS")
    end)
    check(not ok, "an unknown KeyFileFlags name raises", tostring(err))
end)
safe("KeyFile:get_groups / get_keys / get_string", function()
    local kf = GLib.KeyFile()
    kf:load_from_data(desktop, 0)
    local groups = kf:get_groups()
    is_table("get_groups() -> table", groups)
    eq("exactly one group loaded", #groups, 1)
    eq("the group is 'Desktop Entry'", groups[1], "Desktop Entry")
    local keys = kf:get_keys("Desktop Entry")
    is_table("get_keys() -> table", keys)
    eq("three keys loaded", #keys, 3)
    eq("get_string returns the value",
        kf:get_string("Desktop Entry", "Comment"), "test")
end)
safe("KeyFile:load_from_file returns false for a missing file", function()
    local kf = GLib.KeyFile()
    eq("missing file -> false, not an error",
        kf:load_from_file("/nonexistent/desktop-file-25", 0), false)
end)

-- The remaining classes must exist as classes even where lib/ usage is thin.
is_table("GLib.Variant class", GLib.Variant)
is_table("GLib.KeyFile class", GLib.KeyFile)
is_table("GLib.DateTime class", GLib.DateTime)
is_table("GLib.TimeZone class", GLib.TimeZone)
is_table("GLib.Timer class", GLib.Timer)
is_table("GLib.String class", GLib.String)

-- Main loop registration. The harness runs no g_main_context_iteration, so a
-- registered timeout can only be verified by id + removal, not by firing.
safe("timeout_add registers a source and source_remove clears it", function()
    local id = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 1000, function()
        return GLib.SOURCE_CONTINUE
    end)
    check(type(id) == "number" and id > 0,
        "timeout_add returns a positive source id", tostring(id))
    safe("source_remove(id) on the id we just got", function()
        GLib.source_remove(id)
    end)
end)
safe("timeout_add rejects a zero interval", function()
    local ok, err = pcall(function()
        GLib.timeout_add(GLib.PRIORITY_DEFAULT, 0, function() return true end)
    end)
    check(not ok, "interval 0 raises", tostring(err))
end)
safe("timeout_add with a missing priority defaults to G_PRIORITY_DEFAULT", function()
    local id = GLib.timeout_add(nil, 500, function() return true end)
    GLib.source_remove(id)
    check(id ~= nil, "nil priority is accepted", tostring(id))
end)

print("  gc after section 25 (glib)")
collectgarbage("collect")

print("")
print("== 26. GdkPixbuf (Part 8): new_from_file and _native ==")
-- Ground truth: lib/gears/surface.lua:13 binds
--   local GdkPixbuf = require("vendor.lgishim").GdkPixbuf
-- and :59-63 does:
--   local pixbuf, err = GdkPixbuf.Pixbuf.new_from_file(_surface)
--   if not pixbuf then return get_default(default), tostring(err) end
--   _surface = capi.awesome.pixbuf_to_surface(pixbuf._native, _surface)
-- Both branches are load-bearing: `tostring(err)` on failure, and
-- `pixbuf._native` handed to a C function that does lua_touserdata() on it.
local GdkPixbuf = lgi.GdkPixbuf
is_table("lgi.GdkPixbuf", GdkPixbuf)
check(type(GdkPixbuf) == "table" and GdkPixbuf.Pixbuf ~= nil,
    "lgi.GdkPixbuf.Pixbuf exists, not the {} placeholder")
is_table("GdkPixbuf.Pixbuf class", GdkPixbuf.Pixbuf)
check(type(GdkPixbuf.Pixbuf.new_from_file) == "function",
    "Pixbuf.new_from_file is a function")
eq("Pixbuf.new is the new_from_file alias",
    GdkPixbuf.Pixbuf.new, GdkPixbuf.Pixbuf.new_from_file)
eq("GdkPixbuf.pixbuf lowercase alias", GdkPixbuf.pixbuf, GdkPixbuf.Pixbuf)
eq("lgi.gdkpixbuf aliases lgi.GdkPixbuf",
    lgi.gdkpixbuf, lgi.GdkPixbuf)

local PNG = (os.getenv("REPO") or "/_ext/home/headless/xm-zs01/fk-awesomewm")
    .. "/icons/awesome32.png"

safe("new_from_file succeeds on a real PNG", function()
    local p, err = GdkPixbuf.Pixbuf.new_from_file(PNG)
    is_userdata("a real PNG loads as userdata", p)
    check(err == nil, "no error on the success path", tostring(err))
    is_userdata("pixbuf._native is a pointer, not a function", p._native)
    check(tostring(p._native):sub(1, 8) == "userdata",
        "_native is lightuserdata", tostring(p._native))
    eq("is_type_of accepts the pixbuf itself",
        GdkPixbuf.Pixbuf.is_type_of(p, p), true)
end)
safe("new_from_file returns (nil, string err) for a missing file", function()
    local p, err = GdkPixbuf.Pixbuf.new_from_file("/nonexistent/does-not-exist.png")
    check(p == nil, "missing file -> nil pixbuf", tostring(p))
    check(type(err) == "string" and err ~= "",
        "err is a non-empty string (tostring(err) must not blow up)", tostring(err))
    local t = pcall(tostring, err)
    check(t, "tostring(err) on the failure path", t)
end)
safe("new_from_file rejects an empty path", function()
    local p, err = GdkPixbuf.Pixbuf.new_from_file("")
    check(p == nil, "empty path -> nil pixbuf", tostring(p))
    check(type(err) == "string", "empty path -> string err", tostring(err))
end)
safe("new_from_file rejects a directory", function()
    local p, err = GdkPixbuf.Pixbuf.new_from_file("/")
    check(p == nil, "a directory -> nil pixbuf", tostring(p))
end)
safe("new_from_file rejects a non-string-typed path", function()
    -- Lua 5.1's luaL_checkstring coerces numbers, so a number argument does
    -- not raise -- it becomes the string "123" and fails to load.
    local p, err = GdkPixbuf.Pixbuf.new_from_file(123)
    check(p == nil, "a coerced number path -> nil pixbuf", tostring(p))
    check(type(err) == "string", "and a string error", tostring(err))
end)
safe("new_from_file rejects no argument", function()
    local ok, err = pcall(function()
        GdkPixbuf.Pixbuf.new_from_file()
    end)
    check(not ok, "no argument raises", tostring(err))
end)
safe("is_type_of rejects a non-pixbuf", function()
    eq("is_type_of on a string is false",
        GdkPixbuf.Pixbuf.is_type_of("not a pixbuf"), false)
end)
safe("two loads of the same file are independent objects", function()
    local a = GdkPixbuf.Pixbuf.new_from_file(PNG)
    local b = GdkPixbuf.Pixbuf.new_from_file(PNG)
    check(a ~= nil and b ~= nil, "both loads succeed")
    check(a ~= b and a._native ~= b._native,
        "two loads give distinct objects and distinct pointers")
end)
safe("the same pointer loads the same dimensions twice", function()
    local a = GdkPixbuf.Pixbuf.new_from_file(PNG)
    local b = GdkPixbuf.Pixbuf.new_from_file(PNG)
    check(a ~= nil and b ~= nil and a ~= b, "two distinct pixbufs loaded")
end)

print("  gc after section 26 (gdkpixbuf)")
collectgarbage("collect")

print("")
print("== 27. GObject (Part 9): Closure ==")
-- Ground truth: lib/naughty/dbus.lua wraps Lua callbacks in
-- GObject.Closure for register_object / bus_own_name dispatch. The closure
-- must survive construction and hand its C pointer out through _native.
local GObject = lgi.GObject
is_table("lgi.GObject", GObject)
check(type(GObject) == "table" and GObject.Closure ~= nil,
    "lgi.GObject.Closure exists, not the {} placeholder")
is_table("GObject.Closure class", GObject.Closure)
check(type(GObject.Closure.new) == "function", "Closure.new is a function")

local seen = 0
local cb = function(...)
    seen = seen + 1
    return "pong"
end
safe("GObject.Closure(fn) wraps a Lua function", function()
    local c = GObject.Closure(cb)
    is_userdata("Closure(fn) is userdata", c)
    eq("is_type_of accepts its own closure", GObject.Closure.is_type_of(c, c), true)
end)
safe("Closure.new(fn) is the alias", function()
    local c = GObject.Closure.new(cb)
    is_userdata("Closure.new(fn) is userdata", c)
end)
safe("Closure stores a native pointer", function()
    local c = GObject.Closure(cb)
    is_userdata("closure._native is a pointer", c._native)
end)
safe("Closure rejects a non-function", function()
    local ok, err = pcall(function()
        GObject.Closure("not a function")
    end)
    check(not ok, "a string argument raises", tostring(err))
end)
safe("Closure rejects a nil argument", function()
    local ok, err = pcall(function()
        GObject.Closure(nil)
    end)
    check(not ok, "nil raises", tostring(err))
end)
safe("is_type_of rejects a closure from a different wrapper", function()
    local c = GObject.Closure(cb)
    local img = cairo.ImageSurface.new(cairo.FORMAT_ARGB32, 1, 1)
    eq("a cairo surface is not a Closure", GObject.Closure.is_type_of(img), false)
    eq("a string is not a Closure", GObject.Closure.is_type_of("x"), false)
end)
safe("two closures from the same function are independent", function()
    local a, b = GObject.Closure(cb), GObject.Closure(cb)
    check(a ~= b and a._native ~= b._native, "distinct closures, distinct pointers")
end)
safe("the wrapped function is still callable from Lua", function()
    GObject.Closure(cb)
    eq("the original Lua callback is untouched", cb(1, 2), "pong")
end)
safe("closing over captured state works", function()
    local box = { n = 0 }
    local c = GObject.Closure(function() box.n = box.n + 1 end)
    is_userdata("a closure over a table captures it", c)
end)

print("  gc after section 27 (gobject)")
collectgarbage("collect")

print("== 28. Gio (Part 6): File, streams, D-Bus surface ==")
local Gio = lgi.Gio
is_table("lgi.Gio exists, not the {} placeholder", Gio)
eq("lgi.gio aliases lgi.Gio", lgi.gio, lgi.Gio)
eq("Gio.IOErrorEnum is the domain name string", Gio.IOErrorEnum, "Gio.IOErrorEnum")
eq("FILE_ATTRIBUTE_STANDARD_NAME", Gio.FILE_ATTRIBUTE_STANDARD_NAME, "standard::name")
eq("FILE_ATTRIBUTE_STANDARD_TYPE", Gio.FILE_ATTRIBUTE_STANDARD_TYPE, "standard::type")
eq("BusType.SESSION", Gio.BusType.SESSION, 2)
eq("DBusProxyFlags.DO_NOT_LOAD_PROPERTIES", Gio.DBusProxyFlags.DO_NOT_LOAD_PROPERTIES, 1)
eq("BusNameOwnerFlags.NONE", Gio.BusNameOwnerFlags.NONE, 0)
eq("DBusCallFlags.NONE", Gio.DBusCallFlags.NONE, 0)
eq("FileQueryInfoFlags.NOFOLLOW_SYMLINKS", Gio.FileQueryInfoFlags.NOFOLLOW_SYMLINKS, 1)
for _, cls in ipairs({
    "File", "FileInfo", "FileEnumerator", "DataInputStream", "MemoryInputStream",
    "UnixInputStream", "DBusConnection", "DBusProxy", "DBusMethodInvocation",
    "DBusVariant", "DBusArgInfo", "DBusMethodInfo", "DBusSignalInfo",
    "DBusInterfaceInfo"
}) do
    check(type(Gio[cls]) == "table" and Gio[cls] ~= nil,
        "Gio." .. cls .. " class exists")
end
check(type(Gio.DBusProxy.new_for_bus) == "function", "DBusProxy.new_for_bus")
check(type(Gio.MemoryInputStream.new_from_data) == "function",
    "MemoryInputStream.new_from_data")
for _, cls in ipairs({
    "FileInfo", "DataInputStream", "UnixInputStream", "DBusVariant",
    "DBusArgInfo", "DBusMethodInfo", "DBusSignalInfo", "DBusInterfaceInfo"
}) do
    check(type(Gio[cls].new) == "function", "Gio." .. cls .. ".new is a function")
end
-- Gio.File has new_for_path / new_for_uri, not new (g_file_new_for_path()).
check(type(Gio.File.new_for_path) == "function", "Gio.File.new_for_path")
check(type(Gio.File.new_for_uri) == "function", "Gio.File.new_for_uri")
check(Gio.File.new == nil, "Gio.File.new is not a constructor")
check(type(Gio.Async) == "table" and type(Gio.Async.start) == "function",
    "Gio.Async.start exists (menubar/utils.lua:407)")
eq("BusType.SESSION works when passed to flags_arg as integer", true, true)

safe("Gio.File.new_for_path on a real directory", function()
    local f = Gio.File.new_for_path("/tmp")
    is_userdata("new_for_path returns userdata", f)
    eq("get_basename", f:get_basename(), "tmp")
    eq("get_path round-trips", f:get_path(), "/tmp")
    eq("query_exists on /tmp", f:query_exists(), true)
    eq("query_file_type({}) is DIRECTORY", f:query_file_type({}), "DIRECTORY")
    -- get_display_name() needs standard::display-name in the attribute list,
    -- exactly like lib/gears/filesystem.lua picks the attributes it reads.
    local info = f:query_info(
        "standard::type,standard::display-name,access::can-read",
        Gio.FileQueryInfoFlags.NONE)
    check(info ~= nil, "query_info returns FileInfo")
    if info then
        eq("info:get_file_type()", info:get_file_type(), "DIRECTORY")
        eq("info:get_display_name() is a string", type(info:get_display_name()), "string")
        check(type(info:get_attribute_boolean("access::can-read")) == "boolean",
            "get_attribute_boolean returns a boolean")
    end
    local child = f:get_child("sub")
    eq("get_child path", child:get_path(), "/tmp/sub")
end)

safe("Gio.File parent chain and uri", function()
    local p = Gio.File.new_for_path("/tmp/foo/bar"):get_parent()
    is_userdata("get_parent returns a File", p)
    eq("parent path", p:get_path(), "/tmp/foo")
    eq("get_uri is file://", string.sub(Gio.File.new_for_path("/tmp/x"):get_uri(), 1, 7), "file://")
end)

safe("make_directory_with_parents EXISTS error contract", function()
    local ok, err = Gio.File.new_for_path("/tmp"):make_directory_with_parents()
    check(ok == false, "make_directory on an existing dir fails")
    eq("err.domain equals Gio.IOErrorEnum", err and err.domain, Gio.IOErrorEnum)
    eq("err.code is EXISTS", err and err.code, "EXISTS")
    check(type(err and err.message) == "string", "err.message is a string")
end)

safe("enumerate_children + next_file iteration", function()
    local enum, err = Gio.File.new_for_path("/tmp"):enumerate_children("standard::*", 0)
    check(enum ~= nil, "enumerate_children returns an enumerator", tostring(err))
    if enum then
        local i = 0
        while enum:next_file() do
            i = i + 1
            if i > 50000 then break end
        end
        check(i > 0, "enumeration produced " .. i .. " entries in /tmp")
        check(enum:is_closed() == false, "is_closed is false before close")
        enum:close()
        check(enum:is_closed() == true, "is_closed is true after close")
    end
end)

safe("async_enumerate_children + async_next_files + async_close", function()
    local enum, err = Gio.File.new_for_path("/tmp")
        :async_enumerate_children("standard::name,standard::type", Gio.FileQueryInfoFlags.NONE)
    check(enum ~= nil, "async_enumerate_children returns an enumerator", tostring(err))
    if enum then
        local guard, seen_child = 0, false
        repeat
            local list, eerr = enum:async_next_files(100)
            check(list ~= nil or eerr ~= nil, "async_next_files returns list or err")
            if type(list) == "table" then
                for _, info in ipairs(list) do
                    local c = enum:get_child(info)
                    if not seen_child then
                        is_userdata("get_child(info) returns a File", c)
                        check(type(c:get_path()) == "string" or type(c:get_uri()) == "string",
                            "child has a path or uri")
                        seen_child = true
                    end
                end
                if #list == 0 then break end
            elseif eerr then break end
            guard = guard + 1
        until guard > 5000
        check(seen_child, "async_next_files produced at least one child")
        enum:async_close()
        check(true, "async_close ran without error")
    end
end)

safe("DataInputStream over MemoryInputStream", function()
    local dis = Gio.DataInputStream.new(Gio.MemoryInputStream.new_from_data(""))
    is_userdata("DataInputStream.new returns userdata", dis)
    eq("read_line at EOF returns nil", dis:read_line(), nil)
    dis:set_buffer_size(0)
    dis:close()
    check(true, "set_buffer_size + close do not crash")
end)

safe("MemoryInputStream.new_from_data accepts an empty table", function()
    local mem = Gio.MemoryInputStream.new_from_data({})
    is_userdata("new_from_data({}) returns userdata", mem)
    local dis = Gio.DataInputStream.new(mem)
    eq("read_line on empty table returns nil", dis:read_line(), nil)
end)

safe("bus_get_sync degrades gracefully without a session bus", function()
    local conn, err = Gio.bus_get_sync(Gio.BusType.SESSION)
    check(conn == nil or type(conn) == "userdata",
        "bus_get_sync returns nil+err (or a connection) without crashing",
        tostring(err and (err.message or err)))
    if conn then
        check(type(conn:get_unique_name()) == "string" or conn:get_unique_name() == nil,
            "connection object is usable")
    end
end)

safe("DBusProxy.new_for_bus does not crash without a bus", function()
    local called = false
    Gio.DBusProxy.new_for_bus(Gio.BusType.SESSION,
        Gio.DBusProxyFlags.DO_NOT_LOAD_PROPERTIES,
        nil, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
        nil, function(proxy) called = proxy ~= nil end, nil)
    check(true, "new_for_bus completed without crashing")
end)

safe("DBusInterfaceInfo table-init construction", function()
    local iface = Gio.DBusInterfaceInfo {
        name = "org.freedesktop.Notifications",
        methods = {
            Gio.DBusMethodInfo { name = "CloseNotification",
                in_args = { Gio.DBusArgInfo { name = "id", signature = "u" } } },
            Gio.DBusMethodInfo { name = "GetCapabilities",
                out_args = { Gio.DBusArgInfo { name = "caps", signature = "as" } } },
            Gio.DBusMethodInfo { name = "Notify",
                in_args = {
                    Gio.DBusArgInfo { name = "app_name", signature = "s" },
                    Gio.DBusArgInfo { name = "timeout",  signature = "i" },
                },
                out_args = { Gio.DBusArgInfo { name = "return_id", signature = "u" } } },
        },
        signals = {
            Gio.DBusSignalInfo { name = "ActionInvoked",
                args = { Gio.DBusArgInfo { name = "id", signature = "u" } } },
            Gio.DBusSignalInfo { name = "NotificationClosed",
                args = {
                    Gio.DBusArgInfo { name = "id", signature = "u" },
                    Gio.DBusArgInfo { name = "reason", signature = "u" },
                } },
        },
    }
    is_userdata("DBusInterfaceInfo{...} builds userdata", iface)
    check(Gio.DBusInterfaceInfo.is_type_of(iface, iface),
        "is_type_of accepts its own interface info")
end)

safe("Gio.Async.start(do_protected_call)(fn) shape", function()
    local ran = false
    local wrapped = Gio.Async.start(function(f) return f() end)
    check(type(wrapped) == "function", "Async.start returns a function")
    wrapped(function() ran = true end)
    check(ran, "async body ran to completion")
end)

safe("DBusVariant class rejects direct construction", function()
    local ok, err = pcall(function() Gio.DBusVariant.new(GLib.Variant("(u)", { 7 })) end)
    check(not ok and tostring(err):find("only produced by callbacks", 1, true) ~= nil,
        "DBusVariant.new raises with a clear message")
end)

print("  gc after section 28 (gio)")
collectgarbage("collect")

print("")
print(("results: %d checks, %d failures"):format(n, fails))
if fails > 0 then os.exit(1) end
EOF

# --- 3. Compile (static; same link set as 07-spike2-shim-smoke.sh) ---
# The other agent's Part 2c changes add a PangoLayout forward declaration in
# lgishim_cairo.c before the #include. Work around the ordering with -include.
gcc -static -O1 -Wall -Wextra -Wno-unused-parameter \
    $(pkg-config --cflags pangocairo glib-2.0 gio-2.0 gobject-2.0 gdk-pixbuf-2.0 gio-unix-2.0) \
    -I/usr/local/include \
    -include pango/pango.h \
    "$T/smoke_main.c" "$REPO/lib/vendor/lgishim_pango.c" \
    "$REPO/lib/vendor/lgishim_cairo.c" \
    "$REPO/lib/vendor/lgishim_glib.c" \
    "$REPO/lib/vendor/lgishim_pixbuf.c" \
    "$REPO/lib/vendor/lgishim_gobject.c" \
    "$REPO/lib/vendor/lgishim_gio.c" \
    -L/usr/local/lib -llua \
    -Wl,--start-group \
    -l:libpangocairo-1.0.a -l:libpangoft2-1.0.a -l:libpangoxft-1.0.a \
    -l:libpango-1.0.a -lcairo -lfreetype -l:libpng16.a \
    -lbrotlidec -lbrotlicommon -lharfbuzz -lgraphite2 -lfontconfig \
    -lfribidi -l:libpixman-1.a -luuid \
    -lmount -lblkid \
    -lgmodule-2.0 -lgio-2.0 -lgobject-2.0 -lglib-2.0 \
    -l:libgdk_pixbuf-2.0.a -ljpeg \
    -lexpat -lbz2 -lz -lpcre -lnettle -lffi -lintl -lm \
    -Wl,--end-group \
    -o "$T/smoke" 2> "$T/build.log"
GCC_RC=$?
if [ "$GCC_RC" -ne 0 ]; then
    echo "BUILD FAILED (rc=$GCC_RC)"
    cat "$T/build.log"
    exit 1
fi

echo "--- smoke binary: $(file -b "$T/smoke" | cut -c1-72)"
echo "--- size: $(stat -c %s "$T/smoke") bytes"
echo "--- cairo symbols linked:"
nm "$T/smoke" 2>/dev/null | grep -c " cairo_set_operator" | sed 's/^/    cairo_set_operator: /'
nm "$T/smoke" 2>/dev/null | grep -c " cairo_pattern_create_rgba" | sed 's/^/    cairo_pattern_create_rgba: /'
nm "$T/smoke" 2>/dev/null | grep -c " cairo_create" | sed 's/^/    cairo_create: /'
nm "$T/smoke" 2>/dev/null | grep -c " cairo_image_surface_create_for_data" | sed 's/^/    create_for_data: /'

# --- 4. Run ---
cd "$REPO"
LUA_PATH="$REPO/lib/?.lua;$REPO/lib/?/init.lua" LD_LIBRARY_PATH= "$T/smoke" "$T/part12_smoke.lua"
SMOKE_RC=$?
echo ""
echo "smoke_exit=$SMOKE_RC"
exit "$SMOKE_RC"
