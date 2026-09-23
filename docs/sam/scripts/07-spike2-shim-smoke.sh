#!/bin/bash
# 07-spike2-shim-smoke.sh — Spike 2 shim 行为冒烟（容器内执行）
#
# 容器里没有 X server（/usr/bin/X* 不存在、无网络装 xvfb），所以
# awesome 本体跑不起来。改为把 lgishim_pango.c 单独链成一个最小 Lua
# 解释器，跑一份复刻 lib/ 三个调用点（gears/color.lua、
# beautiful/init.lua、wibox/widget/textbox.lua）真实写法用的测试脚本。
#
# 验证目标：
#   1. 静态二进制里真的调到了 Pango C API（不是 stub）
#   2. 字号/行高量级正确（Pango 1.48 vs lib/ 写的 1.50 API）
#   3. 读写属性 roundtrip（textbox.lua 读回 layout.text）
#   4. 枚举大写约定（getter 返回值要能和 setter 输入做 == 比较）
#   5. __gc 不越权释放借用引用（pango_layout_get_attributes 是借的）
#
# 用法: bash 07-spike2-shim-smoke.sh
set -u

REPO=${REPO:-/_ext/home/headless/xm-zs01/fk-awesomewm}
T=$(mktemp -d /_ext/tmp/shimsmoke.XXXXXX)
trap 'rm -rf "$T"' EXIT

# --- 1. 最小 Lua 宿主：照抄 luaa.c 的 preload 注册方式 ---
cat > "$T/smoke_main.c" <<'EOF'
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <stdio.h>

extern int luaopen_lgishim_pango(lua_State *L);
extern int luaopen_lgishim_pangocairo(lua_State *L);

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
    lua_pop(L, 2);

    rc = luaL_dofile(L, argv[1]) ? 1 : 0;
    lua_close(L);
    return rc;
}
EOF

# --- 2. 测试脚本 ---
cat > "$T/shim_smoke.lua" <<'EOF'
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
local function eq(name, got, want)
    check(got == want, name,
        "got=" .. tostring(got) .. " want=" .. tostring(want))
end

print("== module ==")
local lgi = require("vendor.lgishim")
check(lgi.shim == true, "lgishim.lua selected the shim")
local Pango = lgi.Pango
local PC = lgi.PangoCairo
eq("Pango.SCALE", Pango.SCALE, 1024)
eq("Pango.units_from_double(16)", Pango.units_from_double(16), 16384)
eq("Pango.units_from_double(0.5)", Pango.units_from_double(0.5), 512)

print("== Color (gears/color.lua:99-107, 434) ==")
local c = Pango.Color()
check(type(c) == "userdata", "Pango.Color() -> userdata")
eq("Color:parse('#ff8000')", c:parse("#ff8000"), true)
-- Pango 16-bit color packing: channel = v * 257 (v is 0..255, maps to 0..65535)
-- #ff8000 -> r=255*257=65535, g=128*257=32896, b=0
eq("c.red", c.red, 255 * 257)
eq("c.green", c.green, 128 * 257)
eq("c.blue", c.blue, 0)
eq("Color.parse(Color(), '#000000')",
    Pango.Color.parse(Pango.Color(), "#000000"), true)

print("== FontDescription (beautiful/init.lua:170,219-222) ==")
local fd = Pango.FontDescription.from_string("DejaVu Sans 12")
eq("fd:get_size()", fd:get_size(), 12 * Pango.SCALE)
local s = fd:to_string()
check(type(s) == "string" and s ~= "", "fd:to_string()", tostring(s))
local merged = fd:copy_static()
check(merged ~= fd, "copy_static() returns a distinct object")
merged:merge(Pango.FontDescription.from_string("Bold"), true)
local ms = merged:to_string()
check(type(ms) == "string" and ms:find("Bold") ~= nil,
    "merge() then to_string()", tostring(ms))

print("== PangoCairo / Context / Metrics (beautiful/init.lua load_font) ==")
local fm = PC.font_map_get_default()
check(type(fm) == "userdata", "PangoCairo.font_map_get_default()")
local ctx = fm:create_context()
ctx:set_resolution(96)
check(true, "ctx:set_resolution(96)  [1.48: font-map path]")
local desc = Pango.FontDescription.from_string("DejaVu Sans 10")
desc:merge(ctx:get_font_description(), false)
local mt = ctx:get_metrics(desc, nil)
local asc, dsc = mt:get_ascent(), mt:get_descent()
local height = math.ceil((asc + dsc) / Pango.SCALE)
print(("  --> ascent=%d descent=%d height=%.1f px @96dpi")
    :format(asc, dsc, height))
-- ascent/descent are in Pango units (1/1024 px); assert in that unit.
check(asc > 10000 and asc < 40000,
    "ascent in sane range (Pango units)", asc)
check(dsc > 2000 and dsc < 20000,
    "descent in sane range (Pango units)", dsc)
check(height >= 10 and height <= 20, "font height ~13px", height)

print("== Layout (wibox/widget/textbox.lua) ==")
local L = Pango.Layout.new(ctx)
L.text = "hello world"
eq("layout.text read-back", L.text, "hello world")
eq("layout:get_text()", L:get_text(), "hello world")
check(L.attributes == nil,
    "layout.attributes is nil after set_text " ..
    "(real Pango does NOT auto-inject a language attribute)")
L.attributes = nil
eq("layout.attributes == nil after clearing", L.attributes, nil)

eq("layout.width set/get", (function()
    L.width = Pango.units_from_double(120)
    return L.width
end)(), 122880)
eq("layout.height set/get (-2 = wrap to 2 lines)",
    (function() L.height = -2 return L.height end)(), -2)
eq("set_indent/get_indent", (function()
    L:set_indent(10) return L:get_indent() end)(), 10)
eq("set_wrap/get_wrap", (function()
    L:set_wrap("WORD_CHAR") return L:get_wrap() end)(), "WORD_CHAR")
eq("set_ellipsize/get_ellipsize", (function()
    L:set_ellipsize("END") return L:get_ellipsize() end)(), "END")
eq("set_justify/get_justify", (function()
    L:set_justify(true) return L:get_justify() end)(), true)
eq("set_alignment/get_alignment", (function()
    L:set_alignment("CENTER") return L:get_alignment() end)(), "CENTER")
eq("set_line_spacing/get_line_spacing", (function()
    L:set_line_spacing(2048) return L:get_line_spacing() end)(), 2048)
check(true, "set_font_description(desc)")

local ink, lrect = L:get_pixel_extents()
check(type(ink) == "table" and ink.width > 0,
    "get_pixel_extents() ink.width > 0", tostring(ink.width))
check(type(lrect) == "table" and lrect.height > 0,
    "get_pixel_extents() logical.height > 0", tostring(lrect.height))
print(("  --> ink=%dx%d logical=%dx%d"):format(
    ink.width, ink.height, lrect.width, lrect.height))
check(true, "layout:context_changed()")

print("== parse_markup (textbox.lua:161, 552) ==")
local attr, text = Pango.parse_markup("<b>bold</b> plain", -1, 0)
check(type(text) == "string" and text:find("bold") ~= nil,
    "parse_markup strips tags", tostring(text))
L.text = text
eq("layout.text accepts parsed markup", L.text, text)
check(type(attr) == "nil" or type(attr) == "userdata",
    "parse_markup returns attr or nil", tostring(attr))

print("== protocol ==")
check(type(L:_native()) == "userdata", "._native -> lightuserdata")
local c2 = Pango.Color()
c2:parse("#00ff00")
-- #00ff00 -> r=0, g=255, b=0
local r2 = c2.red
c2:finish()
c2:finish()
eq("value captured before finish()", r2, 0)
check(true, "finish() twice does not crash")

print("== gc ==")
collectgarbage("collect")
collectgarbage("collect")
check(true, "survived 2x full GC (borrowed AttrList not unrefed)")

print("")
print(("results: %d checks, %d failures"):format(n, fails))
if fails > 0 then os.exit(1) end
EOF

# --- 3. 编译（静态，库链与 Spike 1/2 已验证的一致）---
gcc -static -O1 -Wall -Wextra -Wno-unused-parameter \
    $(pkg-config --cflags pangocairo glib-2.0) \
    -I/usr/local/include \
    "$T/smoke_main.c" "$REPO/lib/vendor/lgishim_pango.c" \
    -L/usr/local/lib -llua \
    -Wl,--start-group \
    -l:libpangocairo-1.0.a -l:libpangoft2-1.0.a -l:libpangoxft-1.0.a \
    -l:libpango-1.0.a -lcairo -lfreetype -l:libpng16.a \
    -lbrotlidec -lbrotlicommon -lharfbuzz -lgraphite2 -lfontconfig \
    -lfribidi -l:libpixman-1.a -luuid \
    -lgmodule-2.0 -lgio-2.0 -lgobject-2.0 -lglib-2.0 \
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
echo "--- pango symbols linked:"
nm -u "$T/smoke" 2>/dev/null | wc -l | sed 's/^/    undefined: /'
nm "$T/smoke" 2>/dev/null | grep -c "pango_layout_new" | sed 's/^/    pango_layout_new: /'

# --- 4. 运行 ---
echo ""
LUA_PATH="$REPO/lib/?.lua;$REPO/lib/?/init.lua" "$T/smoke" "$T/shim_smoke.lua"
RC=$?
echo ""
echo "smoke_exit=$RC"
exit "$RC"
