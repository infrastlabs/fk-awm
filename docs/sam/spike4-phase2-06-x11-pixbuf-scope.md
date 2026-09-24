# Spike 4 — X11/Pixbuf Scope 盘点

> **本文界定 Part 8（GdkPixbuf）范围与 Part 10（X11/XRandR）取消依据。实际落地见 `2026-09-23-record2.md` 篇七/篇八，对照表见其顶部。**

> 只读盘点。X11/XRandR/GdkPixbuf 在 `lib/` 的 LGI 侧调用面、`dbus.c` 覆盖核实、`lgishim.lua` 接入点、方案评估、静态链接影响。

## 一、盘点方法与计数口径

| 维度 | 方法 | 计数口径 |
|------|------|----------|
| LGI 侧调用面 | `grep -rn 'lgi\.(X11|XRandR|Xrandr|x11|xrandr)' lib/` 以及 `X11\.` / `XRandR\.` 等别名 | 去重 API 名（如 `X11.Display:open` 算 1 个），不含注释/文档 |
| 上游 C 侧 | `grep -rn '#include.*(X11|Xrandr|xcb/randr)' *.c` | 按文件列，说明"不需要 shim" |
| GdkPixbuf | `grep -rn 'GdkPixbuf\|pixbuf' lib/` + `dbus.c` 专项核实 | 去重 API 名 |
| `lgishim.lua` | 全文 40 行，逐行标注 | 接入点 = 需要增/改的行号 |
| 静态链接 | `grep -n 'X11\|xrandr\|xcb' CMakeLists.txt` + `awesomeConfig.cmake` | 只看 `-l` 和 `pkg_check_modules` 行 |

**注意**：`lib/` 内 `capi.screen`、`capi.awesome` 等是 awesome C API，不是 LGI。不要混为一谈。

## 二、X11 / XRandR 调用面

### 2.1 LGI 侧（需要 shim 的）—— 结论：0 个 API

```bash
grep -rn 'lgi\.(X11|XRandR|Xrandr|x11|xrandr)' lib/
# 无结果
```

`lib/` 下没有任何 `lgi.X11` 或 `lgi.XRandR` 调用。`docs/sam/2026-09-23-record1.md:774-775` 已确认这两个 typelib **不存在**（真缺）。

测试文件有 `lgi.GdkX11`（`tests/test-geometry.lua:317,329`）和 `lgi.Gdk`（`tests/test-selection-*.lua`），但属测试层，Phase 2 不覆盖。

**结论**：X11/XRandR **无 LGI 侧调用面**，Part 10 "X11 shim" 是空集。

### 2.2 上游 C 侧（不需要 shim）

awesome 的 C 层直接用 XCB（不是 Xlib），`xcb/randr.h` 已在以下文件中 #include：

| 文件 | 行号 | 用途 |
|------|------|------|
| `objects/screen.c` | 57 | `xcb/randr.h` — `screen_scan_randr_monitors()` / `screen_scan_randr_crtcs()` |
| `event.c` | 43 | `xcb/randr.h` — RandR 事件处理 |
| `awesome.c` | 50 | `xcb/randr.h` — 初始化 |
| `common/version.c` | 32 | `xcb/randr.h` — `XCB_RANDR_GET_MONITORS` 版本宏 |
| `awesomeConfig.cmake` | 140 | `xcb-randr` pkg-config 依赖 |

这些是 awesome 内部实现，不经过 LGI，**不需要 shim**。

### 2.3 XRandR C 层 API 清单（供复用评估，`objects/screen.c`）

| 行号 | API | 用途 |
|------|-----|------|
| 653 | `xcb_randr_get_monitors()` | 获取 RandR 1.5 monitor 列表 |
| 654 | `xcb_randr_get_monitors_reply()` | 解析 reply |
| 655 | `xcb_randr_get_monitors_monitors_iterator()` | 迭代器 |
| 663 | `xcb_randr_monitor_info_next()` | 遍历 |
| 641 | `xcb_randr_monitor_info_outputs()` | 每个 monitor 的 output 列表 |
| 643 | `xcb_randr_monitor_info_outputs_length()` | output 数量 |
| 740 | `xcb_randr_get_screen_resources()` | 获取 CRTC 资源 |
| 741 | `xcb_randr_get_screen_resources_reply()` | 解析 reply |
| 749 | `xcb_randr_get_screen_resources_crtcs()` | CRTC 列表 |
| 754 | `xcb_randr_get_crtc_info()` | CRTC 详情 |
| 755 | `xcb_randr_get_crtc_info_reply()` | 解析 reply |
| 704 | `xcb_randr_get_crtc_info_outputs()` | CRTC 关联的 output |
| 706 | `xcb_randr_get_crtc_info_outputs_length()` | output 数量 |
| 708 | `xcb_randr_get_output_info()` | output 详情 |
| 709 | `xcb_randr_get_output_info_reply()` | 解析 reply |
| 717 | `xcb_randr_get_output_info_name_length()` | output 名称长度 |
| 719 | `xcb_randr_get_output_info_name()` | output 名称字符串 |
| 833-834 | `xcb_randr_query_version()` | 版本查询 |

共 ~18 个 XCB RandR 调用，全部在 `screen.c` 内部，不暴露给 Lua。

## 三、GdkPixbuf 调用面 + `dbus.c` 覆盖核实

### 3.1 `dbus.c` 覆盖结论：**未覆盖**

```bash
grep -n 'pixbuf\|surface' dbus.c
# 无结果
```

`dbus.c` 没有 pixbuf 相关代码。pixbuf 的 C 侧实现在 `luaa.c`（`pixbuf_to_surface`、`load_image`）和 `draw.c`（`draw_surface_from_pixbuf`、`draw_load_image`）。

### 3.2 LGI 侧 GdkPixbuf 调用面（去重 = 1 个 API）

| # | 文件:行号 | 调用 | 说明 |
|---|----------|------|------|
| 1 | `lib/gears/surface.lua:13` | `require("vendor.lgishim").GdkPixbuf` | 获取模块 |
| 2 | `lib/gears/surface.lua:59` | `GdkPixbuf.Pixbuf.new_from_file(_surface)` | **唯一 API 调用** |
| 3 | `lib/gears/surface.lua:63` | `capi.awesome.pixbuf_to_surface(pixbuf._native, _surface)` | C 侧 API，非 LGI |

**去重 API**：`GdkPixbuf.Pixbuf.new_from_file(path)` — 仅此 1 个。

### 3.3 调用链分析

```
surface.lua:59  GdkPixbuf.Pixbuf.new_from_file(path)
    ↓ 返回 { _native = <GdkPixbuf* lightuserdata> }
surface.lua:63  capi.awesome.pixbuf_to_surface(pixbuf._native, path)
    ↓ luaa.c:322  (GdkPixbuf *) lua_touserdata(L, 1)
    ↓ draw.c:85    draw_surface_from_pixbuf(buf) → 逐像素复制到 cairo surface
    ↓ luaa.c:326  lua_pushlightuserdata(L, surface)
```

**关键约束**：`pixbuf._native` 必须是 `GdkPixbuf*` 的 lightuserdata（C 指针）。`luaa.c:322` 用 `lua_touserdata` 强转，不是 `luaL_checkudata`。shim 必须返回真实的 `GdkPixbuf*`（通过 `gdk_pixbuf_new_from_file()`），包装为 `{_native = <lightuserdata>}` 表。

### 3.4 `imagebox.lua` 的 pixbuf 路径

`lib/wibox/widget/imagebox.lua:421`：
```lua
-- rsvg handle failed, try to load cairo surface with pixbuf
setup_succeed = load_and_apply(self, image, surface.load, set_surface)
```

这不直接调 GdkPixbuf，而是走 `surface.load()` → `surface.load_uncached_silently()` → `GdkPixbuf.Pixbuf.new_from_file()`。imagebox.lua 的 pixbuf 需求已由 surface.lua 的 1 个 API 覆盖。

### 3.5 `lgishim.lua` 缺少 GdkPixbuf（当前 Bug）

`lib/vendor/lgishim.lua` 的 M 表（第 23-32 行）没有 `GdkPixbuf` 键。`surface.lua:13` 的 `require("vendor.lgishim").GdkPixbuf` 在 shim 模式下返回 `nil`，导致 `surface.lua:59` 崩溃（`attempt to index a nil value (field 'Pixbuf')`）。这是 Part 8 要补的缺口。

### 3.6 测试文件中的 GdkPixbuf（不在此次范围）

| 文件 | 行号 | 说明 |
|------|------|------|
| `tests/test-selection-getter.lua` | 9,31,184 | `lgi.GdkPixbuf`，测试 selection 传输 |
| `spec/gears/surface_spec.lua` | 5 | `lgi.Gdk`（非 GdkPixbuf） |
| `tests/_client.lua` | 217 | `lgi.Gdk.Gravity`（非 GdkPixbuf） |

Phase 2 不覆盖测试文件。

## 四、`lgishim.lua` 现状与新增 C 模块的接入点

### 4.1 完整结构（40 行）

| 行号 | 内容 | 说明 |
|------|------|------|
| 1-15 | 注释头 | 说明 shim 策略、空表原因 |
| 17-18 | `local M` + `pcall(require, "lgishim_pango")` | 首步：尝试加载 Pango shim |
| 20-38 | 分支 | ok=true → 构建 shim 表；ok=false → 回退 `require("lgi")` |
| 21 | `pcall(require, "lgishim_pangocairo")` | PangoCairo shim |
| 22 | `pcall(require, "lgishim_cairo")` | Cairo shim |
| 23-32 | M 表 | `shim=true` + 已实现模块 + 空表 |
| 24 | `shim = true` | 标识位 |
| 25 | `Pango = Pango` | Pango 模块（来自 lgishim_pango.c） |
| 26 | `PangoCairo = ok2 and PangoCairo or {}` | PangoCairo 降级空表 |
| 27 | `Cairo = ok3 and Cairo or {}` | Cairo 降级空表 |
| ~~28~~ | ~~`GLib = {}`~~ | ~~空表占位~~ → **已交付**：`GLib = ok5 and GLib or {}`（Part 5，1568 行 C） |
| ~~29~~ | ~~`Gio = {}`~~ | ~~空表占位~~ → **已交付**：`Gio = ok7 and Gio or {}`（Part 6，2399 行 C） |
| **30** | **`Rsvg = {}`** | **仍是空表**（决策 B 砍掉）——⚠ 见下方校定，空表**不**是安全降级 |
| ~~31~~ | ~~`GObject = {}`~~ | ~~空表占位~~ → **已交付**：`GObject = ok6 and GObject or {}`（Part 9，667 行 C） |

> **⚠ 2026-09-24 校定：** `GLib`/`Gio`/`GObject` 三张整层空表已交付（Part 5/6/9），只剩 `Rsvg = {}`。且原稿把 Rsvg 空表判成「安全」是 Lua 语义错误——`{}` 是真值，`Rsvg and Rsvg.Handle` 不会短路。见 `spike4-phase2-01-scope.md` §七.1 校定与 `spike4-phase2-07-lib-lgi-coverage-matrix.md` §F1。
| 33 | `M.cairo = M.Cairo` | 小写别名 |
| 34-38 | else | `require("lgi")` 回退（正常构建） |

### 4.2 新增 C 模块需改动的精确行号

以新增 `lgishim_pixbuf`（Part 8）为例：

| 改动点 | 文件:行号 | 改动内容 |
|--------|----------|----------|
| C 源文件 | `lib/vendor/lgishim_pixbuf.c`（新建） | 实现 `luaopen_lgishim_pixbuf` |
| CMake 源文件 | `CMakeLists.txt:132` 之后 | `list(APPEND AWE_SRCS .../lgishim_pixbuf.c)` |
| CMake 链接 | `CMakeLists.txt:138-146` | 追加 `-l:libgdk_pixbuf-2.0.a`（如需静态） |
| extern 声明 | `luaa.c:72` 之后 | `extern int luaopen_lgishim_pixbuf(lua_State *L)` |
| preload 注册 | `luaa.c:1147` 之后 | `lua_pushcfunction(L, luaopen_lgishim_pixbuf); lua_setfield(L, -2, "lgishim_pixbuf")` |
| shim 表 | `lib/vendor/lgishim.lua:22` 之后 | `pcall(require, "lgishim_pixbuf")` |
| shim 表 | `lib/vendor/lgishim.lua:28` 之前 | `GdkPixbuf = ok4 and GdkPixbuf or {}` |

### 4.3 加载顺序约束

无。`lgishim.lua` 的 pcall 之间没有顺序依赖（每个 C 模块独立 `luaopen_*`）。唯一要求是 `luaa.c` 的 `package.preload` 注册在 `luaA_init()` 阶段完成（当前在 `luaa.c:1137-1147`）。

## 五、X11 shim 方案评估

### 5.1 三种方案

| 方案 | 描述 | 人天 |
|------|------|------|
| **砍** | 不做 X11/XRandR shim，像 Rsvg 一样明确声明不做 | 0 |
| **复用** | 用 awesome C 层已有的 `screen.c` XRandR 逻辑 | 0 |
| **做 shim** | 新建 `lgishim_x11.c`，实现 X11/XRandR LGI 侧绑定 | 无对象 |

### 5.2 结论：**砍（等价于复用，因为调用面为零）**

**一句话理由**：`lib/` 下 0 个 `lgi.X11` / `lgi.XRandR` 调用，没有 shim 对象，复用也无从谈起。

`lgi.X11` 和 `lgi.XRandR` typelib 在系统上不存在（`docs/sam/2026-09-23-record1.md:774-775`），awesome 从未在 Lua 层使用它们。所有 X11/XRandR 操作通过自己的 C 层（`screen.c` 的 XCB RandR 调用 + `capi.screen` 暴露给 Lua）完成。

### 5.3 对后续 Phase 的影响

| 影响 | 说明 |
|------|------|
| Part 10 取消 | 原 1-2 人天 → 0 |
| `lgishim.lua` 不改 | 不需要加 X11/XRandR 表项（不像 GLib/Gio 留空表） |
| 静态链接不变 | awesome 已链接所有需要的 X11/XCB 库 |
| `lgishim_x11.c` 不建 | 无文件产出 |

## 六、静态链接依赖影响

### 6.1 awesome 当前 X11 链接现状

**动态链接**（`CMakeLists.txt:107-115`，`AWESOME_STATIC_DEPS=ON` 时转为静态）：

| 行号 | 库 | 说明 |
|------|-----|------|
| 107 | `-lxcb-image -lxcb-shm -lxcb-render-util -lX11-xcb` | xcb 扩展 |
| 108 | `-lXdmcp -lXau -lXext` | X11 底层协议 |
| 114 | `-lXinerama -lXfixes -lXrandr -lXcomposite` | X11 扩展 |

**pkg-config 声明**（`awesomeConfig.cmake:130-163`）：

| 行号 | 依赖 | 说明 |
|------|------|------|
| 131 | `xcb>=1.6` | XCB 核心 |
| 138 | `x11` | Xlib（仅 `objects/key.c:42` 一处 `#include <X11/Xlib.h>`） |
| 140 | `xcb-randr` | RandR 扩展 |
| 143 | `xcb-xinerama` | Xinerama |
| 150 | `xcb-xfixes` | XFixes |
| 156 | `cairo-xcb` | Cairo XCB 后端 |

### 6.2 不做 X11 shim 的链接影响

**零新增**。awesome 已经链接了所有需要的 X11/XCB 库。`AWESOME_STATIC_DEPS` 已包含静态链接所需全量（线 105-116）。

### 6.3 GdkPixbuf 静态链接

`gdk-pixbuf-2.0` 已在 `awesomeConfig.cmake:136` 声明为 pkg-config 依赖。`draw.c:184` 直接调 `gdk_pixbuf_new_from_file()`，`draw.h:34` 有 `typedef struct _GdkPixbuf GdkPixbuf` 前向声明。

shim 的 `lgishim_pixbuf.c` 只需 `#include <gdk-pixbuf/gdk-pixbuf.h>` 并调用同名 C API，**不需要新增链接库**（GdkPixbuf 已在 awesome 的链接闭包内）。

静态模式下可能需要显式追加 `-l:libgdk_pixbuf-2.0.a` 到 `CMakeLists.txt:138-146` 的 group 内（如果当前 `.a` 未被其他依赖带入）。

## 七、施工切分建议

### 7.1 Part 8（GdkPixbuf shim）切分

| 批次 | 内容 | 预估 | 冒烟验证点 |
|------|------|------|------------|
| 8.1 | 新建 `lib/vendor/lgishim_pixbuf.c`：实现 `luaopen_lgishim_pixbuf`，暴露 `GdkPixbuf.Pixbuf.new_from_file(path)` → 返回 `{_native = <GdkPixbuf*>}`，内部调 `gdk_pixbuf_new_from_file()` | 0.5d | C 编译通过，`require("lgishim_pixbuf")` 返回表 |
| 8.2 | `lgishim.lua:22` 加 `pcall(require, "lgishim_pixbuf")`，M 表（第 28 行前）加 `GdkPixbuf` | 0.1d | `require("vendor.lgishim").GdkPixbuf.Pixbuf` 非 nil |
| 8.3 | `luaa.c:72` 加 extern，`luaa.c:1147` 加 preload 注册；`CMakeLists.txt:132` 加 `AWE_SRCS` | 0.2d | 完整编译通过，awesome 启动不报 GdkPixbuf 缺失 |
| 8.4 | 冒烟：`surface.load("test.png")` 返回有效 cairo surface；spec/gears/surface_spec.lua E1-E10 从 error 变 pass | 0.2d | 冒烟 542 + surface E1-E10 全绿 |

**Part 8 总计：1.0 人天**（与 b16 估算 0-2 一致）。

### 7.2 Part 10（原 "X11 shim"）→ **取消**

| 原计划 | 结论 | 理由 |
|--------|------|------|
| X11/XRandR shim | **不做** | LGI 侧调用面 = 0 |
| `lgishim_x11.c` | **不建** | 无对象需要 shim |
| `lgishim.lua` X11/XRandR 表项 | **不加**（不像 GLib/Gio 留空表） | 没有代码路径访问；加空表反而制造"静默 nil 传播" |

### 7.3 人天重分配

| Part | 内容 | 原估算 | 新估算 | 变化 |
|------|------|--------|--------|------|
| 8 | GdkPixbuf shim | 0-2 | **1.0** | 做实 |
| 9 | GLib/Gio/GObject shim | 待盘点 | 待盘点 | 不变 |
| 10 | ~~X11/XRandR shim~~ | 1-2 | **0（取消）** | -1~2d |

## 八、明确不做项

| 不做项 | 理由 |
|--------|------|
| X11/XRandR shim（`lgishim_x11.c`） | LGI 侧调用面 = 0，无 shim 对象 |
| X11/XRandR 空表加入 `lgishim.lua` | 没有代码路径访问 `lgi.X11` |
| `lgi.Gdk` / `lgi.GdkX11` shim | 仅测试文件使用，生产代码不需要 |
| `lgi.Gtk` shim | `beautiful/gtk.lua:74` 非核心路径 |
| Rsvg shim | `docs/sam/spike3-librsvg-decision.md` 已决策砍掉 |
| `surface.lua` 改为直接调 `capi.awesome.load_image` 绕过 pixbuf | Phase 2 不改源码，只补 shim |
| `lib/vendor/lgishim.lua` 移除 `Rsvg = {}` | 保持现状，让 imagebox.lua 的 pcall 自然失败 |

---

## 附录 A：X11 LGI 侧 grep 完整记录

以下 grep 覆盖 `lib/` 全部 171 个 `.lua` 文件：

| 搜索模式 | 结果 | 说明 |
|----------|------|------|
| `lgi\.X11` | 0 | lib/ 无任何 lgi.X11 调用 |
| `lgi\.XRandR` | 0 | lib/ 无任何 lgi.XRandR 调用 |
| `X11\.` | 0 | 无 X11 模块别名 |
| `XRandR\.` | 0 | 无 XRandR 模块别名 |
| `lgi\.GdkX11` | 0（lib/） | 仅 `tests/test-geometry.lua:317,329` 出现 |
| `lgi\.Gdk` | 0（lib/） | 仅测试文件出现 |

## 附录 B：GdkPixbuf LGI 侧 grep 完整记录

| 搜索模式 | lib/ 匹配文件 | 匹配行 |
|----------|-------------|--------|
| `GdkPixbuf` | `lib/gears/surface.lua` | 13, 59 |
| `pixbuf` | `lib/gears/surface.lua` | 59, 63 |
| `pixbuf` | `lib/wibox/widget/imagebox.lua` | 420（注释） |

## 附录 C：`dbus.c` 完整核实

```bash
$ grep -n 'pixbuf\|surface\|GdkPixbuf' dbus.c
# 无结果

$ wc -l dbus.c
509 dbus.c
```

`dbus.c` 共 509 行，处理 D-Bus 通知、图片数据编码（`_raw`、`_argb`），**不涉及 pixbuf 转换**。pixbuf 相关实现在 `luaa.c`（行 312-357）和 `draw.c`（行 80-193）。

## 附录 D：`lgishim.lua` 完整 M 表对照

```
行 17: local ok, Pango = pcall(require, "lgishim_pango")
行 21: local ok2, PangoCairo = pcall(require, "lgishim_pangocairo")
行 22: local ok3, Cairo = pcall(require, "lgishim_cairo")
        ↓ 需要在此新增 ↓
        local ok4, GdkPixbuf = pcall(require, "lgishim_pixbuf")

行 23: M = {
行 24:     shim = true,
行 25:     Pango = Pango,
行 26:     PangoCairo = ok2 and PangoCairo or {},
行 27:     Cairo = ok3 and Cairo or {},
        ↓ GdkPixbuf 应插入此处（按字母序，在 Cairo 之后） ↓
行 28:     GLib = {},          -- Part 9 占位    → 已交付：ok5 and GLib or {}
行 29:     Gio = {},           -- Part 9 占位    → 已交付：ok7 and Gio or {}
行 30:     Rsvg = {},          -- 已砍           → 仍是空表（⚠ 唯一未处理阻塞项）
行 31:     GObject = {},       -- Part 9 占位    → 已交付：ok6 and GObject or {}
行 32: }
```

Part 8 需在第 22 行后加 `pcall(require, "lgishim_pixbuf")`，在第 27 行后加 `GdkPixbuf = ok4 and GdkPixbuf or {},`。

---

## 附 F：本次更新说明（2026-09-24 校定）

1. **§五·M 表 28-31 行校定**：`GLib = {}` / `Gio = {}` / `GObject = {}` 三行原文写「已砍/占位」，**实际均已交付**（分别对应 Part 5 / Part 6 / Part 9），改成 `已交付：ok5 and GLib or {}` 等。
2. **Rsvg 行加警示**：`Rsvg = {}` 标记为**唯一未处理的阻塞项**——它现在**待交付**，不是安全空表；见 `spike4-phase2-04-c-api-conventions.md §八·6` 三档约定。
3. **顶部标注**：加一行「本文覆盖 X11（已砍）与 GdkPixbuf（Part 8）范围。逐 Part 执行见 `2026-09-23-record2.md` 顶部对照表」。
