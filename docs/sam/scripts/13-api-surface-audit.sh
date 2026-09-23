#!/bin/bash
# 13-api-surface-audit.sh —— API 面差异审计（静态 vs apt 动态）
#
# 分两步：
#   usage   路径 B：从 lib/ 提取「使用面清单」（别名绑定→链），A/B 双跑验证逐条
#           存在性/可调用性，diff 出「仅静态缺」= 真实缺口候选
#   surface 路径 A：全 lgi shim 表面枚举（递归 + 环守卫 + 常量表），A/B diff
#           出「没用到但 shim 缺」的观察项
#   all     两者都跑（默认）
#
# 产物：/tmp/api-surface-audit/  （dump + diff + 报告）
#
# 注意：apt 是 Ubuntu 4.3-5build1（含发行版补丁），静态是本 fork（基于上游 4.3）——
#   判差异时先看「是上游 4.3 本来就没有（apt 补丁加进来的）」还是「上游有我们裁了」，
#   别把发行版补丁当静态缺口。
#
# 可调参数（环境变量）：
#   LIB_STATIC   静态 lib 目录，默认 /usr/local/static/awm/share/awesome/lib
#   BIN_STATIC   静态二进制，默认 /usr/local/static/awm/bin/awesome
#   BIN_APT      apt 对照二进制，默认 /usr/bin/awesome
#   LIB_APT      apt lib 目录，默认 /usr/share/awesome/lib
#   DISP_START   Xvnc 起始显示号，默认 40（A/B 各占一个，VNC 监听口 = 5900 + 该值）
#   OUT          产物目录，默认 /tmp/api-surface-audit
set -u

LIB_STATIC=${LIB_STATIC:-/usr/local/static/awm/share/awesome/lib}
BIN_STATIC=${BIN_STATIC:-/usr/local/static/awm/bin/awesome}
BIN_APT=${BIN_APT:-/usr/bin/awesome}
LIB_APT=${LIB_APT:-/usr/share/awesome/lib}
DISP_START=${DISP_START:-40}
OUT=${OUT:-/tmp/api-surface-audit}

MODE=${1:-all}
command -v python3 >/dev/null || { echo "缺 python3"; exit 64; }
[ -d "$LIB_STATIC" ] || { echo "静态 lib 不存在: $LIB_STATIC"; exit 64; }

mkdir -p "$OUT"

# ---------- 提取器（路径 B 使用面）----------
cat > "$OUT/extract_usage.py" <<'PYEOF'
import re, glob, sys
lib = sys.argv[1]
def strip_lua_comment(line):
    out, i = [], 0
    while i < len(line):
        c = line[i]
        if c in '"\'`':
            q = c; j = i + 1
            while j < len(line):
                if line[j] == '\\': j += 2; continue
                if line[j] == q: break
                j += 1
            out.append(line[i:j+1]); i = j + 1
        elif line[i:i+2] == '--':
            break
        else:
            out.append(c); i += 1
    return ''.join(out)

lgi_use, lgi_single, capi_use, capi_single = set(), set(), set(), set()
SKIP = {'Rsvg', 'SvgSurface'}
for path in glob.glob(lib + '/**/*.lua', recursive=True):
    text = open(path).read()
    al = {}
    for mm in re.finditer(r'local\s+(\w+)\s*=\s*(?:require\("vendor\.lgishim"\)|lgi)\.(\w+)', text):
        al[mm.group(1)] = mm.group(2)
    for mm in re.finditer(r'local\s+(\w+)\s*=\s*require\("vendor\.lgishim_(\w+)"\)', text):
        al[mm.group(1)] = mm.group(2)
    for mm in re.finditer(r'local\s+lgi\s*=\s*require\("vendor\.lgishim"\)', text):
        al['lgi'] = 'lgi'
    for raw in text.splitlines():
        line = strip_lua_comment(raw)
        if re.match(r'^\s*local\s+[\w,\s]+\s*=\s*(?:require\("vendor\.lgishim[^"]*"\)|lgi)\.', line):
            continue
        for mm in re.finditer(r'\b(\w+)\.([A-Za-z_]\w*(?:\.[A-Za-z_]\w*){1,3})', line):
            a, ch = mm.group(1), mm.group(2)
            if a in al:
                b = al[a]
                if b == 'lgi': lgi_use.add('lgi.' + ch)
                elif b not in SKIP: lgi_use.add(b + '.' + ch)
        for mm in re.finditer(r'\b(\w+)\.(\w+)\b', line):
            a, at = mm.group(1), mm.group(2)
            if a in al and at != 'new':
                b = al[a]
                if b == 'lgi': lgi_single.add('lgi.' + at)
                elif b not in SKIP: lgi_single.add(b + '.' + at)
        for mm in re.finditer(r'\b(\w+):(\w+)\s*\(', line):
            a, mt = mm.group(1), mm.group(2)
            if a in al:
                b = al[a]
                if b == 'lgi': lgi_single.add('lgi.:' + mt)
                elif b not in SKIP: lgi_single.add(b + '.:' + mt)
        for mm in re.finditer(r'\bcapi\.(\w+)\.(\w+)', line):
            capi_use.add('capi.' + mm.group(1) + '.' + mm.group(2))
        for mm in re.finditer(r'\bcapi\.(\w+):(\w+)', line):
            capi_single.add('capi.' + mm.group(1) + '.:' + mm.group(2))
        for mm in re.finditer(r'\b(root|screen)\.(\w+)\b', line):
            capi_single.add(mm.group(1) + '.:' + mm.group(2))
lgi_single = {x for x in lgi_single if not x.startswith('lgi.')}
for f, s in [('lgi_use.txt', lgi_use), ('lgi_single.txt', lgi_single),
             ('capi_use.txt', capi_use), ('capi_single.txt', capi_single)]:
    open(sys.argv[2] + '/' + f, 'w').write('\n'.join(sorted(s)) + '\n')
print("usage: lgi chains=%d single/method=%d capi chains=%d single/method=%d" %
      (len(lgi_use), len(lgi_single), len(capi_use), len(capi_single)))
PYEOF

# ---------- 验证探针（A/B 通用）----------
cat > "$OUT/verify.lua" <<'LUAE'
local function read_list(path)
    local out = {}
    local f = io.open(path, "r")
    if not f then return out end
    for line in f:lines() do
        line = line:gsub("^%s+", ""):gsub("%s+$", "")
        if line ~= "" then out[#out + 1] = line end
    end
    f:close()
    return out
end
local lgi_ok, lgi = pcall(require, "lgi")
if not lgi_ok then lgi = require("vendor.lgishim") end
print("LGI =", lgi_ok and "real-lgi(apt)" or "vendor.lgishim(static)")
local capi_map = { root = root, screen = screen, client = client, mouse = mouse,
    awesome = awesome, tag = tag, button = button, key = key, drawin = drawin,
    mousegrabber = mousegrabber, keygrabber = keygrabber }
local function resolve_path(base, parts)
    local cur = base
    for i, p in ipairs(parts) do
        if cur == nil then return nil, i, "broke" end
        local okr, v = pcall(function() return cur[p] end)
        if not okr then return nil, i, "RAISE" end
        cur = v
    end
    return cur, #parts, nil
end
local function check_entry(entry, kind)
    local parts = {}
    local base
    if kind == "lgi" then
        entry = entry:gsub("^lgi%.", "")
    elseif kind == "single" then
        base = lgi
    elseif kind == "method" then
        entry = entry:gsub("%.%:", ".", 1); base = lgi
    elseif kind == "capi" then
        entry = entry:gsub("^capi%.", ""); base = capi_map[entry:match("^([^.]+)")]
        if base == nil then return false, "NOOBJ " .. entry end
        entry = entry:gsub("^[^.]*%.?", "")
    elseif kind == "capi_method" then
        entry = entry:gsub("%.%:", ".", 1); base = capi_map[entry:match("^([^.]+)")]
        if base == nil then return false, "NOOBJ " .. entry end
        entry = entry:gsub("^[^.]*%.?", "")
    end
    if base == nil then base = lgi end
    for p in entry:gmatch("[^.]+") do parts[#parts + 1] = p end
    if #parts == 0 then return true, "OK" end
    local v, depth = resolve_path(base, parts)
    if v == nil then
        return false, "MISS@" .. depth .. " " .. table.concat(parts, ".", 1, depth)
    end
    if kind == "method" or kind == "capi_method" then
        if type(v) ~= "function" then return false, "NOTFUNC " .. type(v) end
    end
    return true, "OK"
end
local missing = {}
local function run(list, kind)
    for _, e in ipairs(list) do
        local ok, msg = check_entry(e, kind)
        if not ok then missing[#missing + 1] = kind .. " " .. e .. " => " .. msg end
    end
end
local function safe_run(list, kind, tag)
    local okr, errr = pcall(run, list, kind)
    if not okr then print("RUN ERR " .. tag .. ": " .. tostring(errr)) end
end
safe_run(read_list("/tmp/api-surface-audit/lgi_use.txt"), "lgi", "lgi_use")
safe_run(read_list("/tmp/api-surface-audit/lgi_single.txt"), "single", "lgi_single")
safe_run(read_list("/tmp/api-surface-audit/capi_use.txt"), "capi", "capi_use")
safe_run(read_list("/tmp/api-surface-audit/capi_single.txt"), "capi_method", "capi_single")
print("=== MISSING ===")
for _, m in ipairs(missing) do print(m) end
print("missing count:", #missing)
LUAE

# ---------- 全表面枚举探针（路径 A）----------
cat > "$OUT/enum.lua" <<'LUAE'
local lgi_ok, lgi = pcall(require, "lgi")
if not lgi_ok then lgi = require("vendor.lgishim") end
print("LGI =", lgi_ok and "real-lgi(apt)" or "vendor.lgishim(static)")
local out = {}
local seen = {}
local function dump(t, prefix, depth)
    if depth > 5 then return end
    if seen[t] then return end
    seen[t] = true
    local keys = {}
    for k in pairs(t) do keys[#keys + 1] = k end
    local mt = getmetatable(t)
    if mt and type(mt.__index) == "table" then
        for k in pairs(mt.__index) do keys[#keys + 1] = k end
    end
    table.sort(keys, function(a, b) return tostring(a) < tostring(b) end)
    for _, k in ipairs(keys) do
        local ok, v = pcall(function() return t[k] end)
        if ok then
            local line = prefix .. "." .. tostring(k) .. "=" .. type(v)
            out[#out + 1] = line
            if type(v) == "table" then dump(v, prefix .. "." .. tostring(k), depth + 1) end
        end
    end
end
for _, mod in ipairs({ "cairo", "GLib", "Gio", "GdkPixbuf", "Pango", "PangoCairo", "GObject" }) do
    if lgi[mod] then dump(lgi[mod], "lgi." .. mod, 0) end
end
local f = io.open(os.getenv("ENUM_OUT") or "/tmp/api-surface-audit/enum.txt", "w")
for _, l in ipairs(out) do f:write(l, "\n") end
f:close()
print("enum lines:", #out)
LUAE

# X server: Xvnc (tigervnc) is the only headless server used here. Parameter
# notes, each load-bearing:
#   -ac              without it the X server refuses every client of :$disp
#                    with "authorization required"
#   -localhost yes   bind 127.0.0.1 only, so the VNC listener on 5900+disp is
#                    not reachable outside the sandbox. -nolisten tcp is NOT a
#                    substitute: it makes a second Xvnc believe the display is
#                    already active and refuse to start.
#   -geometry/-depth Xvnc has no Xvfb-style `-screen 0 WxHxD`
find_xvnc() {
    command -v Xvnc 2>/dev/null && return 0
    for c in /usr/local/static/tigervnc/bin/Xvnc /usr/bin/Xvnc; do
        [ -x "$c" ] && { echo "$c"; return 0; }
    done
}

run_one() {
    local side=$1 bin=$2 search=$3 disp=$4 cfg=$5
    local xb disps
    xb=$(find_xvnc) || { echo "Xvnc 没装: apk add tigervnc，或静态版放 /usr/local/static/tigervnc/bin/Xvnc"; exit 64; }
    # A stale .X$disp-lock makes Xvnc abort with "Server is already active for
    # display"; a stale /tmp/.X11-unix/X$disp makes $bin believe it connected
    # and fail with "cannot open display (error 1)". Clear both sides.
    rm -f "/tmp/.X$disp-lock" "/tmp/.X11-unix/X$disp"
    setsid "$xb" -ac -localhost yes ":$disp" -geometry 1280x800 -depth 24 \
        </dev/null >/dev/null 2>&1 &
    local xpid=$!
    sleep 2
    DISPLAY=:$disp HOME=/home/headless timeout -s KILL 15 "$bin" --search "$search" --config "$cfg" > "$OUT/$side$tag.txt" 2>&1
    kill $xpid 2>/dev/null
    rm -f "/tmp/.X$disp-lock" "/tmp/.X11-unix/X$disp"
}

do_usage() {
    python3 "$OUT/extract_usage.py" "$LIB_STATIC" "$OUT" || exit 1
    # tag prefixes the output filename; it is only assigned *after* run_one
    # returns, so the first call would read an unset variable under set -u.
    tag=
    run_one A "$BIN_APT" "$LIB_APT" $DISP_START "$OUT/verify.lua"; tag=_verify
    run_one B "$BIN_STATIC" "$LIB_STATIC" $((DISP_START + 1)) "$OUT/verify.lua"; tag=_verify
    python3 - "$OUT" <<'PYEOF'
import sys
def miss(f):
    s = set()
    for l in open(f):
        l = l.strip()
        if '=> MISS' in l or 'NOTFUNC' in l:
            s.add(l.split(' =>')[0])
    return s
A, B = miss(sys.argv[1] + '/A.txt'), miss(sys.argv[1] + '/B.txt')
print("== 仅静态缺（候选缺口，需排除 SvgSurface 决策项）==")
for x in sorted(B - A): print(" ", x)
print("== 仅 apt 缺（静态反而更全，无害）==")
for x in sorted(A - B): print(" ", x)
print("== 两端都缺（提取假阳性，如 screen.X / 绑定别名）==")
for x in sorted(A & B): print(" ", x)
PYEOF
}

do_surface() {
    tag=
    run_one A "$BIN_APT" "$LIB_APT" $DISP_START "$OUT/enum.lua"; tag=_enum
    cp "$OUT/A_enum_list.txt" "$OUT/enum_apt.txt" 2>/dev/null
    run_one B "$BIN_STATIC" "$LIB_STATIC" $((DISP_START + 1)) "$OUT/enum.lua"; tag=_enum
    cp "$OUT/B_enum_list.txt" "$OUT/enum_static.txt" 2>/dev/null
    echo "== surface diff（静态缺 = 观察项）=="
    diff "$OUT/enum_apt.txt" "$OUT/enum_static.txt" | grep '^<' | head -60
}

case "$MODE" in
    usage)   do_usage ;;
    surface) do_surface ;;
    all)     do_usage; do_surface ;;
    *) echo "用法: $0 [usage|surface|all]"; exit 64 ;;
esac
echo "产物目录: $OUT"
