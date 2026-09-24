# Spike 4 — lib/ → lgishim 覆盖盘点（全静态 Phase 2）

> **本文跨全部 Part（全 lib 调用面盘点）。逐 Part 的执行记录见 `2026-09-23-record2.md` 顶部对照表；本文校定出的改动已就地写回 01/05/06 三份旧 scope。**

> 口径：**一切以 lib/ 真实代码为准**。旧 scope 文档（`spike4-phase2-05-glib-gio-scope.md`、`spike4-phase2-06-x11-pixbuf-scope.md`）是 Part 5/6/8/9 开工前写的，已证实会漏项，仅作对照（见 §F）。
>
> 本文件是**只读盘点产出**。不修改任何已存在文件。不编译、不跑测试。

- 生成时间：2026-09-24
- 仓库：`fk-awesomewm`
- 方法：静态 grep / sed 全库扫描，范围 `lib/ manpages/ utils/ awesome.c awesome.h`
- shim 源码：`lib/vendor/lgishim_*.c`

## 0. 执行摘要

### 0.1 一句话结论

**Pango / PangoCairo / Cairo / GdkPixbuf 的 shim 覆盖已达可用（Cairo 仅缺 3 项、Pango/PangoCairo/GdkPixbuf 零缺口）。真正的阻塞集中在两个「空表占位」：`M.Gio = {}` 与 `M.Rsvg = {}`。** 只要它们还是空表，`gears/filesystem.lua`、`beautiful/init.lua`、`awful/spawn.lua`、`naughty/dbus.lua`、`wibox/widget/imagebox.lua` 五条主路径全部不可用。

### 0.2 汇总数字

扫描口径（全文一致）：

```
grep -rh -- 'MOD\.' lib/gears lib/beautiful lib/wibox lib/naughty lib/awful lib/menubar lib/ruled \
  | grep -v -E '^[[:space:]]*--' \
  | grep -o -E 'MOD\.[A-Za-z_][A-Za-z0-9_]*(\.[A-Za-z_][A-Za-z0-9_]*)?' | sort -u
```

排除 `lib/vendor/`（shim 自身源码，会重复计数）与整行 `--` 注释（lib/ 里有大量被注释掉的用法示例，逐条核过见 §4 C 级）。

**表 1 —— 点号静态 API 原子（`mod.Mem` / `mod.Mem.Sub`）**

| 模块 | distinct 原子 | 出现次数 | 缺口原子 |
|---|---|---|---|
| cairo | 36 | 189 | 1（`SvgSurface.create`） |
| Gio | 16 | 32 | **16** |
| gio | 5 | 5 | **5** |
| GLib | 9 | 20 | 0 |
| glib | 14 | 20 | 0 |
| GdkPixbuf | 1 | 1 | 0 |
| pixbuf | 1 | 1 | 0 |
| Pango | 7 | 15 | 0 |
| PangoCairo | 1 | 3 | 0 |
| GObject | 1 | 4 | 0 |
| Rsvg | 3 | 4 | **3** |
| **合计** | **94** | **294** | **25** |

→ 点号原子已覆盖 **69 / 94**。

**表 2 —— 冒号实例方法缺口（不在上表 94 内，另计）**

| 类 | 缺口方法 | 调用点 |
|---|---|---|
| Cairo `Context` | `new_sub_path`、`user_to_device_distance` | `gears/shape.lua:287,622,661`；`wibox/widget/systray.lua:75` |
| GLib `Timer` | `stop`、`continue`、`start` | `wibox/container/scroll.lua:232,244,255,257` |
| Gio 各对象 | 见 §3.7.1，共约 26 个方法 | 4 个文件 |

**总计真实缺口 = 25（点号原子）+ 5（Cairo/GLib 冒号方法）= 30 个 distinct API。**
Gio 单看还有约 26 个冒号方法缺口（上表 21 个点号原子之外），所以**Gio 的实际缺口面是 40+ 个 API 名**，这也是它排在 §0.5 第 2 位的原因。

涉及 lib/ 文件数：**34**（`lib/` 下 .lua 共 171 个）。

### 0.3 三个必须知道的结论

1. **`lgishim.lua` 从不 `require` `lgishim_gio`。** 它是硬编码 `M.Gio = {}`。所以即使 `lib/vendor/lgishim_gio.c` 写完并编进二进制，**Gio 依然是空表**——除非同时改 `lgishim.lua`（加一行 `pcall(require, "lgishim_gio")`）和 `luaa.c`（加 preload 注册）。这是给续写 Gio shim 的任务的最重要前置条件。
2. **`M.Rsvg = {}` 是真值表，会让 imagebox 的 Rsvg 守卫形同虚设。** `imagebox.lua:54` 的 `pcall(function() Rsvg = lgi.Rsvg end)` 成功执行，`Rsvg` 被赋成 `{}`，于是 `if not Rsvg then return end` **不成立**，代码继续走到 `Rsvg.Handle.new_from_file()` → 对 nil 取字段 → **未捕获异常**。imagebox 的整条 svg 路径（含 PNG 回退路径）都会先在这里炸。
3. **`awful/spawn.lua` 在模块加载期就执行了一个 Gio 调用。** 该调用外面包了一层 `pcall`，但 pcall 失败后的 else 分支（第 247 行）**没有保护**，直接裸调。所以 `require("awful.spawn")` 必然失败——而 `awful._compat`（被 `awful/init.lua` 顶层 require）依赖它。

### 0.4 `启动即炸` 清单（详见 §4）

| 触发点 | 缺的 API | 触发条件 |
|---|---|---|
| `lib/beautiful/init.lua:274` | `Gio.File.new_for_path` | `beautiful.init("<theme 路径字符串>")` — rc.lua 常规写法 |
| `lib/awful/spawn.lua:247` | `Gio.DataInputStream.new` / `Gio.MemoryInputStream.new_from_data` | `require("awful.spawn")` — 加载期即炸 |
| `lib/naughty/dbus.lua:439` | `Gio.DBusProxy.new_for_bus` | `require("naughty.dbus")` — 加载期即炸（`naughty/init.lua:12` 的 `if dbus then` 恒真，因为 `dbus` 是 C 注册的全局表） |
| `lib/wibox/widget/imagebox.lua:87,89,408,423` | `Rsvg.Handle` | 任何 `imagebox:set_image()` — 常规 UI 路径 |
| `lib/gears/surface.lua:255` | `cairo.SvgSurface.create` | `gears.surface.widget_to_svg()`（已标记 deprecated） |
| `lib/wibox/widget/init.lua:62` | `cairo.SvgSurface.create` | `wibox.widget.draw_to_svg_file()` |
| `lib/gears/shape.lua:287,622,661` | `Context:new_sub_path()` | `shape.partially_rounded_rect` / `shape.pie` / `shape.arc` |
| `lib/wibox/widget/systray.lua:75` | `Context:user_to_device_distance()` | `wibox.widget.systray` 的绘制 |
| `lib/wibox/container/scroll.lua:232,244,255,257` | `GLib.Timer:stop/continue/start` | `wibox.container.scroll` 的 pause/continue/reset_scrolling |

### 0.5 分模块覆盖一览

| 模块 | lib/ 去重 API | 已覆盖 | 缺口 | 判定 |
|---|---|---|---|---|
| Cairo | 36 点号 + 48 冒号方法 | 全部除 3 项 | `SvgSurface.create`、`Context:new_sub_path`、`Context:user_to_device_distance` | ✅ 基本可用 |
| Pango | 7 | 全部 | **0** | ✅ 完全可用 |
| PangoCairo | 1 | 全部 | **0** | ✅ 完全可用 |
| GdkPixbuf | 2 | 全部 | **0** | ✅ 完全可用 |
| GObject | 1（`Closure`×4 处） | 全部 | **0** | ✅ 完全可用 |
| GLib | 23 点号 + Timer 方法 | 点号全覆盖 | `Timer:stop/continue/start` | ⚠️ 局部缺口 |
| **Gio** | **21 点号 + 约 26 冒号方法** | **0** | **全部**（`M.Gio = {}` 不可达） | ❌ 完全不可用 |
| **Rsvg** | **3** | **0** | **全部**（真值表陷阱） | ❌ 完全不可用 |

### 0.6 最该修的 4 件事（按性价比排序）

| 序 | 动作 | 成本 | 收益 |
|---|---|---|---|
| 1 | `lgishim.lua:34` 的 `M.Rsvg = {}` → `M.Rsvg = nil` | **改 1 个字符** | 恢复 svg→png 降级链，修掉 B1（标题栏/菜单/任务栏图标全挂） |
| 2 | `lgishim.lua` 加 `pcall(require, "lgishim_gio")` + `luaa.c` 加 preload | 2 行 | 让已写完的 `lgishim_gio.c`（1330 行）真正可达，解开 A1/A2/A3 的总闸 |
| 3 | `lgishim_glib.c` 的 `TIMER_M` 补 `stop`/`continue`/`start` | ~30 行 | 修 B6（`wibox.container.scroll`） |
| 4 | `CTX_M` 补 `new_sub_path`、`user_to_device_distance`；新增 `SvgSurface.create` | ~50 行 | 修 B4/B5/B2/B3 |

### 0.7 旧 scope 的一处硬伤（详见 §F1）

`spike4-phase2-01-scope.md:149` 判定「`Rsvg = {}` 空表使短路求值返回 nil（不报错），后续走 PNG 降级路径」——**Lua 语义错误**。`{}` 是真值，`a and b` 不会短路，右侧照常被求值并报错。`:295`/`:361` 据此得出「保持空表（安全降级）」的结论。这是三份旧文档里唯一一条会导致实际事故的错误判断。

## 1. A — 全量 require 面

扫描范围：`lib/ manpages/ utils/ awesome.c awesome.h`。

### 1.1 总账

| 形态 | 命中 | 备注 |
|---|---|---|
| `require("vendor.lgishim")`（含 `.Y` 后缀形式） | 39 | 含 1 处注释（`textbox.lua:374`） |
| `require('vendor.lgishim')`（**单引号**） | **0** | 全库不存在 |
| `require("lgi")` | **1** | 仅在 `lib/vendor/lgishim.lua:44`（shim 自身的真 lgi 回退分支） |
| `lgi.require(` | **1** | `lib/beautiful/gtk.lua:74`，`lgi.require('Gtk', '3.0')` |
| 涉及文件数 | **34** | `manpages/`、`utils/`、`awesome.c`、`awesome.h` 均 **0** |

### 1.2 直接绑定形态分布

| 绑定语句 | 处数 |
|---|---|
| `local cairo = require("vendor.lgishim").cairo` | 20 |
| `local glib/GLib = require("vendor.lgishim").GLib` | 5 |
| `local Gio = require("vendor.lgishim").Gio` | 2 |
| `local GdkPixbuf = require("vendor.lgishim").GdkPixbuf` | 1 |
| `local lgishim_version = require("vendor.lgishim").version` | 1 |
| `local lgi = require("vendor.lgishim")`（裸绑定） | 9（+1 注释） |
| **合计** | **38 处真实 + 1 处注释** |

**关键观察：没有任何文件用 `.Pango` / `.PangoCairo` / `.GObject` / `.Rsvg` 直接绑定** —— 这四个模块全部通过「裸 `lgi` 绑定 → `lgi.X`」路径访问。因此如果 `lgishim.lua` 里的 `M.Pango` / `M.PangoCairo` / `M.GObject` 是空表，**不会有任何直接绑定方**暴露问题；问题只会通过 `lgi.X` 的形态显现。

### 1.3 裸绑定（`local lgi = require("vendor.lgishim")`）的 9 个文件及其用到的成员

| 文件 | 绑定/派生 | 实际用到的 lgi 成员 |
|---|---|---|
| `lib/awful/spawn.lua:226` | `Gio = lgi.Gio`、`GLib = lgi.GLib` | `Gio.DataInputStream`、`Gio.MemoryInputStream`、`Gio.UnixInputStream`、`GLib.String`、`GLib.String.hash` |
| `lib/beautiful/init.lua:16` | `Pango`、`PangoCairo`；另 `Gio` 直接绑定 | `Pango.FontDescription.from_string`、`PangoCairo.font_map_get_default`、`Pango.SCALE`、`Pango.FontDescription`、`Gio.File.new_for_path` |
| `lib/beautiful/gtk.lua:73` | — | `lgi.require('Gtk', '3.0')` |
| `lib/gears/color.lua:49` | `cairo = lgi.cairo`、`Pango = lgi.Pango` | `cairo.Pattern.*`、`cairo.Format.*`、`Pango.Color.parse` |
| `lib/menubar/utils.lua:16` | `gio = lgi.Gio`、`glib = lgi.GLib` | `gio.File.new_for_path`、`gio.Async.start`、`gio.FileQueryInfoFlags`、`gio.FILE_ATTRIBUTE_*`、`glib.KeyFile`、`glib.KeyFileFlags` |
| `lib/naughty/dbus.lua:18` | `Gio`、`GLib`、`GObject` | `Gio.DBusProxy.new_for_bus`、`Gio.bus_own_name`、`Gio.DBus*Info`、`Gio.BusType`、`Gio.DBus*Flags`、`Gio.DBusCallFlags`、`GLib.Variant`、`GObject.Closure` |
| `lib/wibox/container/scroll.lua:32` | `GLib = lgi.GLib` | `GLib.Timer()`、`:stop()`、`:continue()`、`:start()` |
| `lib/wibox/widget/imagebox.lua:27` | `Rsvg = lgi.Rsvg` | `Rsvg.Handle`、`Rsvg.Handle.new_from_data`、`Rsvg.Handle.new_from_file`、`Rsvg.Handle:is_type_of` |
| `lib/wibox/widget/textbox.lua:16` | `Pango`、`PangoCairo` | `Pango.Layout.new`、`Pango.parse_markup`、`Pango.units_from_double`、`Pango.AttrList`（仅注释） |

### 1.4 两个非 lgi 但同源的调用点

- **`lib/beautiful/gtk.lua:74` — `lgi.require('Gtk', '3.0')`**：shim 的 `M` 表**没有 `require` 字段**，所以 `lgi.require` 是 nil。但该调用被 `pcall(function() ... end)` 包住（`beautiful/gtk.lua:72`），失败后走 `if not _gtk_status or not Gtk then ... print_warning(...) return nil` 分支。**优雅降级，不炸**，代价是 GTK 主题变量取不到。
- **`lib/gears/surface.lua:22-27` — `require("vendor.lgishim").version`**：shim 的 `M` 在 shim 分支下**不设置 `version`**（只有真 lgi 回退分支设，`lgishim.lua:46`），所以 `lgishim_version` 为 nil，`if lgishim_version then` 跳过版本检查。代码第 18-20 行已有注释显式说明这一点。**已知的、已被接受的行为**。

## 2. B — 顶层别名普查（大小写分别统计）

统计口径：`grep -rh -o -E "lgi\.ALIAS"` 对 `lib/ manpages/ utils/`，大小写**完全区分**，逐名独立计数。

| 顶层成员 | `lgi.<大写>` | 文件数 | `lgi.<小写>` | 文件数 |
|---|---|---|---|---|
| GLib / glib | 5 | 5 | **0** | 0 |
| Gio / gio | 5 | 5 | **0** | 0 |
| GdkPixbuf / gdkpixbuf | 2 | 1 | **0** | 0 |
| Cairo / cairo | **0** | 0 | 5 | 5 |
| Pango / pango | 19 | 6 | **0** | 0 |
| PangoCairo / pangocairo | 4 | 3 | **0** | 0 |
| GObject / gobject | 2 | 2 | **0** | 0 |
| Rsvg / rsvg | 1 | 1 | **0** | 0 |
| `require` | 1 | 1 | — | — |
| `version` | 1 | 1 | — | — |

**本轮复核结论：全库 `lgi.<小写别名>` 的命中数除 `lgi.cairo`（5）外一律为 0。** `lgishim.lua` 只注册了 `M.cairo`（第 37 行）和 `M.gdkpixbuf`（第 38 行）两个小写别名，而 lib/ 恰好只用了 `lgi.cairo`，`M.gdkpixbuf` 目前**无人使用**。因此别名注册是**够用但不对称**的：给 `GLib`/`Gio`/`Pango` 补小写别名不会被 lib/ 用到，不必做。

### 2.1 历史漏项的真相（重要修正）

之前「漏掉 `menubar/utils.lua:409` 的小写 `gio.` 别名」这个结论**方向对，但机理描述需要更正**：`menubar/utils.lua` **没有**用 `lgi.gio`。它用的是**文件内局部变量**：

```
lib/menubar/utils.lua:17  local gio  = lgi.Gio      -- 大写来源
lib/menubar/utils.lua:18  local glib = lgi.GLib
```

之后 `gio.File.new_for_path(dir_path)`（第 409 行）、`gio.Async.start`（407）、`gio.FileQueryInfoFlags.NONE`（371）、`gio.FILE_ATTRIBUTE_STANDARD_NAME`（370）全走这个局部别名。

**教训**：只 grep `lgi.<mod>` 会同时漏掉「`lgi.X` 来源的局部小写别名」和「`require("vendor.lgishim").X` 直接绑定」。§1.2/§1.3 的两张表已把这三条路径全部合并。同类局部别名还有：`lib/gears/color.lua` 的 `cairo`/`Pango`、`lib/gears/timer.lua` 的 `glib`、`lib/awful/keygrabber.lua` 的 `glib`、`lib/wibox/widget/textclock.lua` 的 `DateTime`/`TimeZone`、`lib/awful/screenshot.lua` 的 `datetime`/`timezone`。

### 2.2 按局部别名归并后的真实调用面（大小写合并）

| 逻辑模块 | 访问它的所有别名 | distinct 原子 | 出现次数 |
|---|---|---|---|
| Cairo | `cairo` | 36 | 189 |
| GLib | `GLib`, `glib`, `datetime`, `timezone`, `DateTime`, `TimeZone` | 9 + 14 = 23 | 20 + 20 = 40 |
| Gio | `Gio`, `gio` | 16 + 5 = 21 | 32 + 5 = 37 |
| Pango | `Pango` | 7 | 15 |
| PangoCairo | `PangoCairo` | 1 | 3 |
| GdkPixbuf | `GdkPixbuf`, `pixbuf` | 1 + 1 = 2 | 1 + 1 = 2 |
| GObject | `GObject` | 1 | 4 |
| Rsvg | `Rsvg` | 3 | 4 |
| **合计** | | **94** | **294** |

## 3. C — 按 shim 模块的覆盖矩阵（核心产出）

### 3.0 顶层绑定形态（`lib/vendor/lgishim.lua`，49 行）

shim 分支（`lgishim.lua:20-42`）实际构建的表：

| 键 | 来源 | 状态 |
|---|---|---|
| `M.shim` | 常量 `true` | ✅ |
| `M.Pango` | `pcall(require, "lgishim_pango")` | ✅ 已注册 |
| `M.PangoCairo` | `pcall(require, "lgishim_pangocairo")` | ✅ 已注册（实现体在 `lgishim_pango.c`） |
| `M.Cairo` | `pcall(require, "lgishim_cairo")` | ✅ 已注册 |
| `M.GdkPixbuf` | `pcall(require, "lgishim_pixbuf")` | ✅ 已注册 |
| `M.GLib` | `pcall(require, "lgishim_glib")` | ✅ 已注册 |
| `M.GObject` | `pcall(require, "lgishim_gobject")` | ✅ 已注册 |
| **`M.Gio`** | **硬编码 `{}`** | ❌ **空表占位** |
| **`M.Rsvg`** | **硬编码 `{}`** | ❌ **空表占位（真值表陷阱）** |
| `M.cairo` | `= M.Cairo` | ✅ 小写别名 |
| `M.gdkpixbuf` | `= M.GdkPixbuf` | ✅ 小写别名（无人使用） |
| `M.GdkPixbuf.pixbuf` | `= M.GdkPixbuf.Pixbuf` | ✅ C 侧不能做身份拷贝，只能在 Lua 侧做 |
| `M.version` | **仅真 lgi 分支设置**（`:46`） | shim 下为 nil（已知，`surface.lua:18-20` 有注释） |
| **`M.require`** | **不存在** | ❌ `lgi.require('Gtk','3.0')` 返回 nil（有 pcall 兜底） |

**⚠️ 给续写 Gio shim 的任务的前置条件**：`lgishim.lua` **没有任何一行** `pcall(require, "lgishim_gio")`。`M.Gio = {}` 是字面量。所以就算 `lib/vendor/lgishim_gio.c` 写完、编进二进制、甚至在 `luaa.c` 里注册了 preload，**`M.Gio` 依然是空表**。必须同时改 `lgishim.lua`（照 `lgishim.lua:25` 的模式加 `local ok7, Gio = pcall(require, "lgishim_gio")` 并把 `M.Gio` 换成 `ok7 and Gio or {}`）+ `luaa.c`（加 extern + preload）。

同理 `M.Rsvg = {}` 是字面量，若日后要补 Rsvg shim 也要走同样的两处改动。

### 3.1 Cairo

shim：`lib/vendor/lgishim_cairo.c`（1568 行，实为 2380+ 行）。**已注册的 16 个 luaL_Reg 表 + 16 个 enum**：

| 表 | 条目数 | 导出名 |
|---|---|---|
| `CTX_M` | 74 | `Context` |
| `CTX_PG` / `CTX_P` | 11 / 12 | Context 属性 |
| `SURF_M` | 15 | `Surface` |
| `IMAGE_M` | 2 | `ImageSurface`（复用 C_SURFACE 元表） |
| `RECORDING_M` | 1 | `RecordingSurface`（复用 C_SURFACE 元表） |
| `PAT_M` / `PAT_PG` / `PAT_P` | 31 / 4 / 4 | `Pattern` |
| `MATRIX_M` / `MATRIX_PG` | 7 / 2 | `Matrix` |
| `REGION_M` / `REGION_PG` | 21 / 1 | `Region` |
| `FONTOPT_M` | 1 | `FontOptions` |
| `RECT_M` | 1 | `Rectangle`、`RectangleInt`（纯表构造器，`make_call_table`，**无 `is_type_of`**） |
| enum | 16 | `Operator` `Format` `Content` `Status` `Antialias` `LineCap` `LineJoin` `FillRule` `Path` `Extend` `FontSlant` `FontWeight` `PatternType` `Filter` `SurfaceType` `Overlap` |

另外 `is_type_of` 与 `finish` **不在 luaL_Reg 表里**，由 `class_setup()` / `make_class_table()` 自动挂载（`lgishim_cairo.c:534-575`、`644-646`），所以每个 class userdata 上都有，无需逐个核对。`Format.stride_for_width` 是 `install_enum` 之后手动挂的函数（`lgishim_cairo.c:2368-2371`）。

#### 3.1.1 lib/ 实际调用 vs shim 注册 —— 覆盖矩阵

| lib/ 实际调用的 API | 次数 | 行号 | shim 是否注册 | 缺口 |
|---|---|---|---|---|
| `cairo.Context` / `cairo.Context.create` | 37 | `gears/surface.lua:305`、`wibox/widget/init.lua:63` 等 | ✅ `CTX_M.new` / `CTX_M.create` | — |
| `cairo.ImageSurface` / `.create_for_data` | 31 | `gears/matrix.lua:24`、`naughty/layout/legacy.lua` 等 | ✅ `IMAGE_M.new` / `.create_for_data` | — |
| `cairo.Surface` | 10 | `gears/surface.lua`、`wibox/drawable.lua` | ✅ `SURF_M` | — |
| `cairo.Pattern` + `create_for_surface/linear/radial/rgba` | 19 | `gears/color.lua:196-260` | ✅ `PAT_M` 四个构造器全在 | — |
| `cairo.Matrix` | 5 | `gears/matrix.lua` | ✅ `MATRIX_M` | — |
| `cairo.Region` + `create` / `create_rectangle` | 8 | `awful/client/shape.lua`、`wibox/container/border.lua` | ✅ `REGION_M` | — |
| `cairo.RecordingSurface` | 3 | `beautiful/theme_assets.lua:202` | ✅ `RECORDING_M` | — |
| `cairo.Rectangle` / `RectangleInt` | 22 | `beautiful/theme_assets.lua`、`gears/matrix.lua` | ✅ `make_call_table` | — |
| `cairo.FontOptions` | — | 间接（`CTX.get_font_options` 返回） | ✅ `FONTOPT_M` | — |
| 16 个 enum（`Operator.SOURCE/CLEAR/OVER/IN`、`Format.ARGB32/RGB24/RGB32/A1/stride_for_width`、`Content.COLOR/COLOR_ALPHA/ALPHA`、`Extend.PAD/REPEAT/REFLECT`、`FillRule.EVEN_ODD`、`LineCap`、`Filter`） | ~120 | 全库 | ✅ 全部 16 个 enum 已 `install_enum` | — |
| **`cairo.SvgSurface.create`** | **2** | **`gears/surface.lua:255`、`wibox/widget/init.lua:62`** | **❌ shim 无 `SvgSurface` 表** | **缺口 1** |
| **`Context:new_sub_path()`** | **3** | **`gears/shape.lua:287, 622, 661`** | **❌ 未在 `CTX_M` 注册** | **缺口 2** |
| **`Context:user_to_device_distance()`** | **1** | **`wibox/widget/systray.lua:75`** | **❌ 未在 `CTX_M` 注册** | **缺口 3** |

#### 3.1.2 `CTX_M` 的 48 个被调用方法核对结果

lib/ 实际以 `:method()` 形式调用的 Cairo 上下文方法共 **48 个 distinct**，逐一对比 `CTX_M`（74 条）：

- **46 个已覆盖**（`line_to` 83 次、`translate` 66、`set_source` 60、`rectangle` 38、`paint` 37、`stroke` 32、`save` 30、`fill` 30、`move_to` 28、`restore` 26、`arc` 26、`set_line_width` 24、`clip` 22、`set_source_surface` 21、`set_operator` 20、`close_path` 19、`set_source_rgba` 15、`fill_preserve` 12、`scale` 11、`rel_line_to` 9、`rotate` 7、`mask` 4、`stroke_preserve`/`reset_clip`/`push_group_with_content`/`pop_group`/`get_source`/`clip_extents` 各 3、其余 18 个各 2 次）。
- **2 个缺口**：`new_sub_path`（3 处）、`user_to_device_distance`（1 处）。
- 另有 `:get_path(` 命中 3 次（`beautiful/init.lua:275`、`menubar/utils.lua:365,387`）——**那是 `Gio.File:get_path`，不是 Cairo**，已计入 §3.7。

#### 3.1.3 重要：三个「看似缺口、实为误报」的项

| 疑似缺口 | 实际 | 证据 |
|---|---|---|
| `cairo.SurfacePattern` / `cairo.RasterSourcePattern` | **仅出现在注释**，不是代码 | `lib/wibox/container/background.lua:109-110`：`-- Note that technically, we could handle cairo.SurfacePattern and -- cairo.RasterSourcePattern. However, ...` |
| `cairo.surface`（小写） | **仅出现在文档注释** | `lib/gears/surface.lua:182`：`-- @treturn cairo.surface the new surface` |
| `cairo.Matrix.new` / `cairo.Matrix.identity` | **lib/ 中不存在**（只在 shim 源码注释里） | `grep -rn 'Matrix\.\(new\|identity\)' lib/` 只命中 `lgishim_cairo.c:511,779` 的注释 |

→ **Cairo 的真实缺口只有 3 个**，前一轮的疑点里 `SurfacePattern` 一类不需要实现。

#### 3.1.4 Surface / Pattern / Matrix / Region 实例方法核对

全部已覆盖，无缺口。实测被调用的项：`create_similar` 7、`finish` 18、`get_height` 11、`get_width` 9、`is_type_of` 9、`union_rectangle` 5、`multiply` 5、`get_surface` 4、`set_extend` 3、`invert` 3、`get_rgba` 3、`add_color_stop_rgba` 3、`get_rectangle` 2、`set_matrix` 2、`get_matrix` 2、`get_extend` 2、`get_color_stop_rgba` 2、`get_color_stop_count` 2、`set_filter` 1、`num_rectangles` 1、`is_empty` 1、`intersect` 1、`init` 1、`get_type` 1、`get_data` 1、`flush` 1。全部能在 `SURF_M` / `PAT_M` / `MATRIX_M` / `REGION_M` 或 `class_setup` 自动挂载项中找到。

### 3.2 Pango

shim：`lib/vendor/lgishim_pango.c`（1568 行中的一部分）。已注册表：`LAYOUT_M`(28)、`LAYOUT_PG`/`LAYOUT_P`(4/4)、`FONTSPEC_M`(6)、`COLOR_M`(2)、`COLOR_PG`(3)、`FONTMAP_M`(1)、`CONTEXT_M`(5)、`METRICS_M`(2)、`ATTRLIST_M`(1)，另模块级常量/函数 `SCALE`、`units_from_double`、`parse_markup`。

| lib/ 实际调用 | 次数 | 行号 | shim 注册 | 缺口 |
|---|---|---|---|---|
| `Pango.FontDescription.from_string` | 2 | `beautiful/init.lua:170, 219` | ✅ `FONTSPEC_M.from_string` | — |
| `FontDescription:from_string/merge/to_string/get_size/copy_static/new` | 多处 | `beautiful/init.lua:162,175,181,219` | ✅ 全部在 `FONTSPEC_M` | — |
| `Pango.Layout.new` | 2 | `textbox.lua:514, 547` | ✅ `LAYOUT_M.new` | — |
| `Layout:set_text/set_width/set_height/set_indent/set_wrap/set_ellipsize/set_justify/set_alignment/set_line_spacing/set_font_description/set_attributes` | 多处 | `textbox.lua` | ✅ `LAYOUT_M` 全在 | — |
| `Layout:get_pixel_extents/get_extents/get_size/get_baseline/get_line_count/get_text/...` | 多处 | `textbox.lua` | ✅ `LAYOUT_M` 全在 | — |
| `Pango.parse_markup` | 2 | `textbox.lua:161, 552` | ✅ 模块级 | — |
| `Pango.units_from_double` | 4 | `textbox.lua:36,37,140,491` | ✅ 模块级 | — |
| `Pango.SCALE` | 2 | `beautiful/init.lua:179,181` | ✅ 模块级 | — |
| `Pango.Color` / `.parse` | 2 | `gears/color.lua` | ✅ `COLOR_M.parse` | — |
| `Pango.AttrList` | **0（仅注释）** | `textbox.lua:162` 是注释文字 | ✅ 已注册（富余） | — |
| `Pango.FontDescription`（裸类） | 1 | 类型注释 | ✅ | — |

**Pango：零缺口。**

**⚠️ 一个容易被误判成缺口的项**：`textbox.lua:379-382` 出现 `font_map:list_families()`、`v:get_name()`、`v:is_monospace()`、`v2:get_face_name()`，若照字面看需要 `FontMap` / `Family` / `Face` 三类共约 8 个方法。**但整段是文档示例，全部被 `--` 注释掉**（`textbox.lua:373-390` 是一整个 `# 用法示例` 注释块）：

```
--    local pangocairo = lgi.PangoCairo
--    local font_map = pangocairo.font_map_get_default()
--    for k, v in pairs(font_map:list_families()) do
--        print(v:get_name(), "monospace?: "..tostring(v:is_monospace()))
```

→ **不需要实现 `FontMap`/`Family`/`Face`**。同理 `Pango.Context.new`、`FontMap.create_context` 中的 `Pango.Context.new` 在 lib/ 里**没有任何调用点**（`grep 'Pango\.Context' lib/` 排除 vendor 后 0 命中），属于已注册的富余 API。`beautiful/init.lua:171` 用的是 `PangoCairo.font_map_get_default():create_context()`，走的是 `FONTMAP_M.create_context`。

### 3.3 PangoCairo

**`lgishim_pangocairo.c` 文件不存在** —— 已确认。`luaopen_lgishim_pangocairo` 定义在 `lgishim_pango.c` 内部（`grep -n 'luaopen_lgishim_pangocairo' lib/vendor/lgishim_pango.c` 有定义，`lib/vendor/` 下无同名 .c 文件）。`luaa.c:71` 的 `extern int luaopen_lgishim_pangocairo` 是**独立 extern**，与实现所在文件无关，链接时由 `lgishim_pango.c` 提供的符号满足，因此**编译/链接没有问题**，preload 注册也正常。

| lib/ 实际调用 | 次数 | 行号 | shim 注册 | 缺口 |
|---|---|---|---|---|
| `PangoCairo.font_map_get_default` | 3 | `beautiful/init.lua:171`、`textbox.lua` 2 处 | ✅ `luaopen_lgishim_pangocairo` 内 `luaL_register` | — |

**PangoCairo：零缺口。唯一缺口是「文件名与 extern 名不一致」带来的可读性问题，不是功能缺口。**

### 3.4 GLib

shim：`lib/vendor/lgishim_glib.c`（1568 行）。已注册：

- 类表：`VARIANT_M`(7)、`KEYFILE_M`(**14**：`new`、`load_from_file`、`load_from_data`、`has_group`、`has_key`、`get_groups`、`get_keys`、`get_string`、`get_boolean`、`get_int`、`get_double`、`get_string_list`、`get_locale_string`、`get_locale_string_list`)、`DATETIME_M`(5)、`TIMEZONE_M`(3)、`TIMER_M`(**2**)、`STRING_M`(2)
- 枚举：`Priority`、`Source`、`KeyFileFlags`
- 模块级常量：`PRIORITY_DEFAULT`、`SOURCE_CONTINUE`、`SOURCE_REMOVE`
- 模块级函数（`MODULE_F`，8 个）：`timeout_add`、`source_remove`、`build_filenamev`、`get_home_dir`、`get_user_data_dir`、`get_system_data_dirs`、`utf8_strlen`、`utf8_substring`

| lib/ 实际调用 | 次数 | 行号 | shim 注册 | 缺口 |
|---|---|---|---|---|
| `glib.timeout_add` / `GLib.timeout_add` | 2 | `gears/timer.lua:102`、`screenshot.lua` | ✅ `MODULE_F` | — |
| `glib.source_remove` | 1 | `gears/timer.lua:120` | ✅ `MODULE_F` | — |
| `glib.PRIORITY_DEFAULT` / `SOURCE_CONTINUE` | 4 | `gears/timer.lua:102-104,118` | ✅ 模块级常量 | — |
| `GLib.build_filenamev` | 多处 | `menubar/icon_theme.lua` | ✅ `MODULE_F` | — |
| `GLib.get_home_dir` / `get_user_data_dir` / `get_system_data_dirs` | 多处 | `menubar/icon_theme.lua`、`menubar/utils.lua` | ✅ `MODULE_F` | — |
| `glib.utf8_strlen` / `utf8_substring` | 4 | `awful/keygrabber.lua:210,211` | ✅ `MODULE_F` | — |
| `glib.KeyFile()` + `:load_from_file` | 2 | `menubar/utils.lua:269-270` | ✅ `KEYFILE_M` | — |
| `glib.KeyFileFlags.NONE` | 1 | `menubar/utils.lua:270` | ✅ enum | — |
| `glib.DateTime` / `glib.TimeZone` | 多处 | `screenshot.lua:44-45`、`textclock.lua:52-53` | ✅ `DATETIME_M` / `TIMEZONE_M` | — |
| `GLib.Variant(...)` | 9 | `naughty/dbus.lua:64,78,352,356,364,369,377,496` | ✅ `VARIANT_M.new` | — |
| `GLib.String(cmd)` / `GLib.String.hash(g)` | 2 | `awful/spawn.lua:305,307` | ✅ `STRING_M` | — |
| `GLib.Timer()` 构造 | 1 | `wibox/container/scroll.lua:425` | ✅ `TIMER_M.new` | — |
| **`Timer:stop()`** | **1** | **`wibox/container/scroll.lua:232`** | **❌ `TIMER_M` 只有 2 条** | **缺口 1** |
| **`Timer:continue()`** | **1** | **`wibox/container/scroll.lua:244`** | **❌** | **缺口 2** |
| **`Timer:start()`** | **2** | **`wibox/container/scroll.lua:255, 257`** | **❌** | **缺口 3** |
| `GLib.Error` | 0 | 仅注释 | ❌ 未注册 | **非缺口** |
| `GLib.Bytes` | 0 | 仅注释 | ❌ 未注册 | **非缺口** |

**GLib 真实缺口：`GLib.Timer` 缺 `stop` / `continue` / `start` 三个方法**（`wibox/container/scroll.lua` 的 `pause` / `continue` / `reset_scrolling`）。

> **文件名更正**：前一轮说「`lib/awful/scroll.lua` 调 `:stop()/:continue()/:start()`」——**该文件不存在**。实际是 **`lib/wibox/container/scroll.lua`**（`grep -n 'GLib.Timer\|:stop()\|:continue()\|:start()' lib/wibox/container/scroll.lua` → 232/244/255/257）。

**两个「不在 shim 里」但**不是**缺口的项**：`GLib.Error`（仅 `textbox.lua:162` 的注释文字）与 `GLib.Bytes`（仅 `spawn.lua:536` 的注释文字）。lib/ 里没有任何一处真实构造或调用它们。

### 3.5 GdkPixbuf

shim：`lib/vendor/lgishim_pixbuf.c`（387 行）。注册：`PIXBUF_M`（3 条：`new`、`new_from_file`、`finish`）、`PIXBUF_PG`（`_native`），模块名 `lgishim_pixbuf.Pixbuf`。

| lib/ 实际调用 | 次数 | 行号 | shim 注册 | 缺口 |
|---|---|---|---|---|
| `GdkPixbuf.Pixbuf.new_from_file` | 1 | `gears/surface.lua:59` | ✅ `PIXBUF_M.new_from_file` | — |
| `pixbuf._native` | 1 | `gears/surface.lua:63` | ✅ `PIXBUF_PG._native` | — |

**GdkPixbuf：零缺口。** `lib/gears/surface.lua:13` 是唯一绑定方（`local GdkPixbuf = require("vendor.lgishim").GdkPixbuf`），全库仅 1 处调用链：`load_surface` → `new_from_file` → `.native` → `capi.awesome.pixbuf_to_surface`。

注：`lgishim.lua:42` 的 `M.GdkPixbuf.pixbuf = M.GdkPixbuf.Pixbuf` 与 `M.gdkpixbuf` 别名（`:38`）**目前无人使用**，但保留无害（是 C 侧无法做身份拷贝时的正确做法）。

### 3.6 GObject

shim：`lib/vendor/lgishim_gobject.c`（667 行）。注册 `OBJECT_M` + `CLOSURE_M`，仅对外导出 `Closure`（`lgishim_gobject.c:664 lua_setfield(L,-2,"Closure")`），`Object` 类表建了但不导出。

| lib/ 实际调用 | 次数 | 行号 | shim 注册 | 缺口 |
|---|---|---|---|---|
| `GObject.Closure(fn)` | 4 | `naughty/dbus.lua:434, 541, 542, 542` | ✅ `CLOSURE_M` + `make_class_table` 的 `__call` | — |

**GObject：零缺口**（lib/ 里唯一的 GObject API 就是 `Closure` 构造，已实现且带 registry 强引用防 GC）。

### 3.7 Gio（shim 续写中，按「未实现」标注）

**状态：`lib/vendor/lgishim_gio.c` 已存在（1330 行，另一个任务在续写）**，已有表：`FILE_M`(:1050)、`FILEINFO_M`(:1070)、`FILEENUM_M`(:1080)、`DATAIN_M`(:1308)、`MEMIN_M`(:1318)、`UNIXIN_M`(:1324)，以及 `luaopen_lgishim_gio`(:95)。**但它对 lib/ 完全不可达**，原因见 §3.0：`lgishim.lua` 硬编码 `M.Gio = {}`，且 `luaa.c` 未注册 `lgishim_gio` preload。

**下列全部按「❌ 不可达」计**（即使 C 侧某项已写，Lua 侧也取不到）：

#### 3.7.1 需要实现的 Gio 类（10 个）

| 类 | 需要的成员 | 调用点 |
|---|---|---|
| `Gio.File` | `new_for_path`、`get_parent`、`query_info`、`query_file_type`、`enumerate_children`、`async_enumerate_children`、`make_directory_with_parents`、`get_path`、`get_uri` | `filesystem.lua:18,34,47,55-59,67-71,79-83,91-95,103,202`；`beautiful/init.lua:274-275`；`menubar/utils.lua:365,371,385,387,409` |
| `Gio.FileInformation` | `get_file_type`、`get_attribute_boolean` | `filesystem.lua:58-59,70-71,82-83,94-95,208`；`menubar/utils.lua:384` |
| `Gio.FileEnum` | `async_next_files`、`get_child`、`async_close`、`get_file_type` | `filesystem.lua:202-208`；`menubar/utils.lua:378,385,404` |
| `Gio.DataInputStream` | `new`、`read_line`、`read_line_async`、`read_line_finish`、`close`、`set_buffer_size` | `awful/spawn.lua:245,247,249,510,513,515,522,525` |
| `Gio.MemoryInputStream` | `new_from_data` | `awful/spawn.lua:245,247` |
| `Gio.UnixInputStream` | `new` | `awful/spawn.lua:421,425` |
| `Gio.DBusProxy` | `new_for_bus`、`call_sync` | `naughty/dbus.lua:439,502` |
| `Gio.DBusConnection` | `emit_signal`、`register_object` | `naughty/dbus.lua:62,76,433` |
| `Gio.DBusInterfaceInfo` / `DBusMethodInfo` / `DBusSignalInfo` / `DBusArgInfo` | 表构造器 `{...}` | `naughty/dbus.lua:396,398,399,401` |
| `Gio.DBusMethodInvocation` | `return_value` | `naughty/dbus.lua:352,356,364,369,377` |
| `Gio.Async` | `start` | `menubar/utils.lua:407` |

#### 3.7.2 需要实现的 Gio 枚举/常量（10 个）

`BusType.SESSION`、`BusNameOwnerFlags.NONE`、`DBusProxyFlags.DO_NOT_LOAD_PROPERTIES`、`DBusCallFlags.NONE`、`FileQueryInfoFlags.NONE`、`IOErrorEnum`、`FILE_ATTRIBUTE_STANDARD_NAME`、`FILE_ATTRIBUTE_STANDARD_TYPE`、`DBusConnection`（仅注释提及，可不实现）、`DBusVariant`（lib/ 实际用 `GLib.Variant` 替代，可不实现）。

#### 3.7.3 需要实现的 Gio 模块级函数（1 个）

`Gio.bus_own_name`（`naughty/dbus.lua:540`）。

#### 3.7.4 统计

distinct 原子 **21**（`Gio.*` 16 + `gio.*` 5），出现 **37** 次，涉及 **4 个文件**：`gears/filesystem.lua`、`beautiful/init.lua`、`awful/spawn.lua`、`menubar/utils.lua`、`naughty/dbus.lua`（5 个文件）。

> 注：`naughty/dbus.lua` 里 `Gio.DBusConnection` 只出现在第 38 行注释（`-- This is either nil or a Gio.DBusConnection ...`），`awful/spawn.lua:501` 的 `Gio.InputStream` 只在 `@tparam` 文档注释里。这两项**不算真实调用点**，但 `Gio.DBusConnection` 的 `emit_signal`/`register_object` 方法调用（62/76/433 行）是真实的。

### 3.8 Rsvg

**`M.Rsvg = {}` 空表占位，且是真值表 → 守卫失效。**

| lib/ 实际调用 | 次数 | 行号 | shim 注册 | 缺口 |
|---|---|---|---|---|
| `Rsvg = lgi.Rsvg`（pcall 内） | 1 | `imagebox.lua:54` | ❌ 空表 | 见下 |
| `Rsvg.Handle.new_from_data(file)` | 1 | `imagebox.lua:87` | ❌ | 缺口 1 |
| `Rsvg.Handle.new_from_file(file)` | 1 | `imagebox.lua:89` | ❌ | 缺口 2 |
| `Rsvg.Handle:is_type_of(image)` | 2 | `imagebox.lua:408, 423` | ❌ | 缺口 3 |

**失效机理（关键）**：

```lua
local Rsvg = nil
do
    local success, err = pcall(function() Rsvg = lgi.Rsvg end   -- :54 成功，Rsvg = {}
    if not success then ... print_warning(...) end
end
...
local function imagebox._load_rsvg_handle(file, style)
    if not Rsvg then return end          -- :70  Rsvg 是 {} → 真值 → 不 return
    ...
    if file:match("<[?]?xml") or file:match("<svg") then
        handle, err = Rsvg.Handle.new_from_data(file)   -- :87 Rsvg.Handle 是 nil → 崩
    else
        handle, err = Rsvg.Handle.new_from_file(file)   -- :89 崩
```

- `:70` 的 `if not Rsvg then return end` **被绕过**（空表是真值）。
- `:408` `if type(image) == "userdata" and not (Rsvg and Rsvg.Handle:is_type_of(image))` → 对 nil 取字段 → 崩。这条在 `set_image` 的 userdata 分支，**只要传了 cairo surface userdata 就会到**。
- `:423` `elseif Rsvg and Rsvg.Handle:is_type_of(image) then` → 同理。

**为什么没有 pcall 兜底**：`load_and_apply`（`imagebox.lua:131-139`）**不是 protected call**——它直接 `local object, cache = image_loader(file, ...)`，异常原样上抛到 `set_image` 调用方。

**回退路径为何也炸**：正常期望是「Rsvg 不可用 → 跳过 svg → 走 pixbuf 加载 PNG」。但这个回退依赖 `:70` 的 `if not Rsvg then return end` 提前返回，而空表让该判断失效，**代码在回退之前就已经崩了**。也就是说：**当前实现下没有可用的 Rsvg 回退路径**。

**修法（给主任务，只列不改）**：把 `lgishim.lua:34` 的 `M.Rsvg = {}` 改成 `M.Rsvg = nil`（或 `M.Rsvg = false`），`if not Rsvg then return end` 即恢复语义，svg 静默跳过、PNG 回退生效。**不要**用 `pcall(require, "lgishim_rsvg")`——那会重新落回空表陷阱，除非同时保证失败时赋 `nil` 而非 `{}`。

## 4. D — 未覆盖的硬阻塞分级清单

### A 级 — 启动即炸（rc.lua 一执行就崩）

#### A1 🔴 `require("awful")` → **rc.lua 第 1 行即崩**（最高危）

**缺失 API**：`Gio.DataInputStream.new`、`Gio.MemoryInputStream.new_from_data`

**炸点**：`lib/awful/spawn.lua:247`

```lua
    local input
    if not pcall(function()
        input = Gio.DataInputStream.new(Gio.MemoryInputStream.new_from_data(""))  -- 245，被 pcall 保护
    end) then
        input = Gio.DataInputStream.new(Gio.MemoryInputStream.new_from_data({}))  -- 247，无保护！
    end
    local line, length = input:read_line()                                        -- 249，无保护
```

`:237-250` 是**模块顶层**代码（`awk 'NR<=237 && /^function/'` 无匹配，即不在任何函数内）。`Gio = {}` 时 `Gio.DataInputStream` 为 nil：
- 第 245 行在 pcall 内 → pcall 返回 false
- 第 247 行**不在** pcall 内 → `attempt to call a nil value (field 'DataInputStream')` **上抛**

**可达性证据 —— 4 条独立路径全部在 `require("awful")` 时触发**：

`lib/awful/init.lua` 的 `ret` 是**立即求值的表字面量**，不是懒加载：

```lua
require("awful._compat")                      -- init.lua:10  ← 第 1 条路径
local ret = {
    client  = require("awful.client");
    ...
    menu    = require("awful.menu");          -- init.lua:26  ← 第 2 条路径
    ...
    rules   = require("awful.rules");         -- init.lua:38  ← 第 3 条路径
    ...
    spawn   = require("awful.spawn");         -- init.lua:41  ← 第 4 条路径
}
```

| 路径 | 链 |
|---|---|
| ① | `awful/init.lua:10` → `awful/_compat.lua:5` `local spawn = require("awful.spawn")`（**模块顶层**） |
| ② | `awful/init.lua:26` → `awful/menu.lua:15` `local spawn = require("awful.spawn")`（**模块顶层**） |
| ③ | `awful/init.lua:38` → `awful/rules.lua:11` `require("ruled.client")` → `ruled/client.lua:114` `local aspawn = require("awful.spawn")`（**模块顶层**） |
| ④ | `awful/init.lua:41` `require("awful.spawn")` 直接 |

**其他模块顶层 require `awful.spawn` 的**（同样会炸）：`awful/widget/prompt.lua:28`、`awful/widget/launcher.lua:18`、`awful/widget/watch.lua:40`、`ruled/client.lua:114`。

**唯一的惰性用法**是 `awful/client.lua:1538` 的 `spawn = spawn or require("awful.spawn")`（在 `awful.client.run_or_raise` 函数体内）——这是唯一一个不会在加载期炸的调用点。

**后果**：rc.lua 的 `local awful = require("awful")`（几乎每个 rc.lua 的第 1 行）抛错 → `luaA_loadrc` 的 `lua_pcall` 捕获 → `luaA_startup_error(err)` 记入 `globalconf.startup_errors`（`luaa.c:1277`）→ **rc.lua 从第 1 行起停止执行**。awesome 以「空配置」运行：无键位、无布局、无自动启动、无 wibox、无标题栏。

#### A2 🔴 `beautiful.init("<theme 路径>")` → **rc.lua 常见行即崩**

**缺失 API**：`Gio.File.new_for_path`

**炸点**：`lib/beautiful/init.lua:274`

```lua
        if t_config == 'string' then
            config = config:gsub("^~/", homedir .. "/")
            local dir = Gio.File.new_for_path(config):get_parent()   -- 274 ← nil: index
            rawset(beautiful, "theme_path", dir and (dir:get_path().."/") or nil)
```

**可达性证据**：`beautiful.init` 的 `string` 分支是**无条件的**。标准 awesome rc.lua 写 `beautiful.init("/usr/share/awesome/themes/default/theme.lua")` 或 `beautiful.init(awutil.get_themes_dir() .. "/default/theme.lua")`，两个都是 string → 必进此分支。`Gio = {}` → `Gio.File` 是 nil → `attempt to index a nil value (field 'File')`。

**注意 `lib/beautiful/init.lua` 不 require `gears.filesystem`**（全库搜 `gfs\.` 在该文件 0 命中），它直接用 `dofile` + `Gio.File`。所以这条路径**不依赖 gfs**，独立成立。

#### A3 🟠 `require("naughty")` → **rc.lua 常见行即崩**

**缺失 API**：`Gio.DBusProxy.new_for_bus`、`Gio.bus_own_name`、`Gio.DBusConnection`、`GObject.Closure`

**炸点**：`lib/naughty/dbus.lua:439` 与 `:540`，两者都在**模块顶层**（列 0，不在任何函数内）：

```lua
Gio.DBusProxy.new_for_bus(                  -- 439 ← 模块顶层，第一处崩点
    Gio.BusType.SESSION,
    ...
Gio.bus_own_name(Gio.BusType.SESSION, "org.freedesktop.Notifications",  -- 540
    Gio.BusNameOwnerFlags.NONE, GObject.Closure(on_bus_acquire),
    GObject.Closure(on_name_acquired), GObject.Closure(on_name_lost))
```

**可达性证据**：`lib/naughty/init.lua:12-14`

```lua
if dbus then
    naughty.dbus = require("naughty.dbus")
end
```

`dbus` 是 **C 注册的全局表**（`luaa.c:1170 luaA_registerlib(L, "dbus", awesome_dbus_lib)`），**恒非 nil**，所以 `if dbus then` **恒真**。只要 `require("naughty")` 就会被执行。

rc.lua 里 `naughty.notify(...)` 的用法必须先 `require("naughty")`（`naughty` 不是 C 注册的懒加载全局，`grep -n 'naughty' luaa.c` 0 命中），所以任何用桌面通知的 rc.lua 都会崩。此外 `lib/ruled/notification.lua:249` 在函数体内惰性 `require("naughty")`。

**后果**：与 A2 同——`lua_pcall` 捕获，rc.lua 截断。

### B 级 — 特定功能才炸

| # | 缺失 API | 炸点 | 触发条件 | 可达性证据 |
|---|---|---|---|---|
| B1 | `Rsvg.Handle.*`（3 项） | `imagebox.lua:87,89,408,423` | 任何 `imagebox:set_image()` | `load_and_apply`(:131-139) 非 protected call；调用方包括 `awful/menu.lua:637,651`、`awful/titlebar.lua:1029`、`awful/widget/layoutbox.lua:38`、`naughty/widget/icon.lua:105,178`、`naughty/layout/legacy.lua:535`。**标题栏图标、任务栏图标、菜单图标全是 string 路径 → 必炸** |
| B2 | `cairo.SvgSurface.create` | `gears/surface.lua:255` | `gears.surface.widget_to_svg()` | 函数已标 `@deprecated widget_to_svg`，但仍可调用 |
| B3 | `cairo.SvgSurface.create` | `wibox/widget/init.lua:62` | `wibox.widget.draw_to_svg_file()` | 同上，独立调用点 |
| B4 | `Context:new_sub_path()` | `gears/shape.lua:287, 622, 661` | `shape.partially_rounded_rect` / `shape.pie` / `shape.arc` | `partially_rounded_rect` 由用户经 `wibox.container.background:set_shape()` 选用（`wibox/container/background.lua:489-493`），**圆角 wibox 很常见**；`shape.arc` 被 `wibox/container/arcchart.lua:147,162` 调用；`shape.pie` lib/ 内无调用方 |
| B5 | `Context:user_to_device_distance()` | `wibox/widget/systray.lua:75` | `wibox.widget.systray` 绘制 | systray 是 wibox 组件，rc.lua 常见 |
| B6 | `GLib.Timer:stop/continue/start` | `wibox/container/scroll.lua:232,244,255,257` | `wibox.container.scroll` 的 `pause`/`continue`/`reset_scrolling` | `scroll.lua:425` 的 `GLib.Timer()` 构造本身**不会炸**（`TIMER_M.new` 已注册），只有 4 个控制方法炸 |
| B7 | `Gio.*` 全套（filesystem 相关） | `gears/filesystem.lua:18,22,34,47,55-59,67-71,79-83,91-95,103,202` | 任何 `gfs.make_directories` / `make_parent_directories` / `file_readable` / `file_executable` / `dir_readable` / `dir_writable` / `is_dir` / `get_random_file_from_dir` | **`file_readable` 全库 16 处调用**，涉及 `menubar/icon_theme.lua:68,157,178,214`、`menubar/utils.lua:225,231,237`、`awful/util.lua:251,257,273`、`awful/screenshot.lua:128`、`naughty/notification.lua:756`、`naughty/layout/legacy.lua:490`、`wibox/widget/imagebox.lua:149`。**图标加载路径全面不可用** |
| B8 | `gio.Async.start`、`Gio.FileEnum:async_next_files/get_child/async_close`、`FILE_ATTRIBUTE_*` | `menubar/utils.lua:370,371,378,384,385,387,404,407,409` | `menubar` 的异步目录枚举（`utils.load_all_desktop_files`） | menubar 加载 .desktop 文件时才走 |
| B9 | `Gio.DBusArgInfo/MethodInfo/SignalInfo/InterfaceInfo` 表构造器、`DBusMethodInvocation:return_value`、`DBusProxy:call_sync`、`BusType`/`BusNameOwnerFlags`/`DBusProxyFlags`/`DBusCallFlags` | `naughty/dbus.lua:352,356,364,369,377,396,398,399,401,502,504` | 通知的 DBus 方法实现 | 与 A3 同一模块，A3 触发时这一批根本到不了 |
| B10 | `GLib.KeyFile:*` | `menubar/utils.lua:269-270` | `menubar` 解析 .desktop 文件 | **已覆盖**，无缺口 —— 此行仅为完备性列出 |

### C 级 — 仅 spec 测试触达 / 死代码

| 项 | 证据 | 结论 |
|---|---|---|
| `GLib.Error` | 仅 `textbox.lua:162` 注释文字：`-- In case of error, attr is false and parsed is a GLib.Error instance.` | **非缺口** |
| `GLib.Bytes` | 仅 `spawn.lua:536` 注释文字 | **非缺口** |
| `cairo.SurfacePattern` / `cairo.RasterSourcePattern` | 仅 `wibox/container/background.lua:109-110` 注释 | **非缺口** |
| `cairo.surface`（小写） | 仅 `gears/surface.lua:182` 的 `@treturn` 注释 | **非缺口** |
| `Pango.FontMap` / `Family` / `Face`（`list_families`、`get_name`、`is_monospace`、`get_face_name`） | 仅 `textbox.lua:373-390` 的整段用法示例注释 | **非缺口** |
| `Pango.Context.new` | lib/ 排除 vendor 后 0 调用点（`beautiful/init.lua:171` 走的是 `PangoCairo.font_map_get_default():create_context()`） | **富余 API，无需实现** |
| `Gio.DBusVariant` | lib/ 无真实调用（dbus.lua 用 `GLib.Variant` 替代） | **非缺口** |
| `Gio.DBusConnection` / `Gio.InputStream` 类名 | 分别仅出现在 `dbus.lua:38`、`spawn.lua:501` 注释（但 `:emit_signal`/`:register_object` 方法调用是真实的，见 B9） | 类表可不建，方法必须有 |

### 4.x 顺带发现的两处文件清单错误

任务给的「重点核查文件」中有两个文件**不存在**：

- **`lib/wibox/drawingctx.lua` 不存在**。`lib/wibox/` 下只有 `drawable.lua`、`hierarchy.lua`、`init.lua`、`container/`、`layout/`、`widget/`。drawing 相关逻辑在 `wibox/drawable.lua`（绑定 `cairo`）和 `wibox/hierarchy.lua`。
- **`lib/awful/scroll.lua` 不存在**。scrolling 容器在 **`lib/wibox/container/scroll.lua`**（前一轮结论第 3 条把路径写错了，见 §7）。

## 5. E — Rsvg 与 X11 复核

### 5.1 Rsvg 复核：**结论成立，且比预想更严重**

**Q1：`M.Rsvg = {}` 到底谁在用？**

只有 **1 个文件**：`lib/wibox/widget/imagebox.lua`。

```
lib/wibox/widget/imagebox.lua:54   local success, err = pcall(function() Rsvg = lgi.Rsvg end)
lib/wibox/widget/imagebox.lua:87       handle, err = Rsvg.Handle.new_from_data(file)
lib/wibox/widget/imagebox.lua:89       handle, err = Rsvg.Handle.new_from_file(file)
lib/wibox/widget/imagebox.lua:408  if type(image) == "userdata" and not (Rsvg and Rsvg.Handle:is_type_of(image)) then
lib/wibox/widget/imagebox.lua:423  elseif Rsvg and Rsvg.Handle:is_type_of(image) then
```

**`lib/beautiful/` 全目录 0 处 Rsvg 引用**（`grep -c Rsvg lib/beautiful/*.lua` → init.lua:0、gtk.lua:0、theme_assets.lua:0、xresources.lua:0）。任务预设「beautiful 用 Rsvg 渲染 svg」**是错的**——`beautiful/theme_assets.lua` 只用 `cairo.RecordingSurface` / `cairo.ImageSurface` / `cairo.Pattern` / `cairo.Rectangle`。

**Q2：svg 图标缺 Rsvg 时的回退路径是什么？**

`imagebox:set_image(image)`（`imagebox.lua:397-435`）的设计意图是三层回退：

```
1. userdata 且不是 Rsvg.Handle  → surface.load(image)      → set_surface
2. string                      → load_and_apply(..., imagebox._load_rsvg_handle, set_handle)
                                  失败 → load_and_apply(..., surface.load, set_surface)   ← PNG 回退
3. Rsvg.Handle userdata          → set_handle
4. cairo.Surface userdata        → set_surface
```

**Q3：回退会不会失败？** —— **会，100% 失败。**

`_load_rsvg_handle`（`:67-92`）开头的守卫：

```lua
local function imagebox._load_rsvg_handle(file, style)
    ...
    if not Rsvg then return end          -- :70  期望：Rsvg 为空时提前返回 nil，触发第 2 层 pixbuf 回退
```

`Rsvg = {}` 是真值 → `not {}` 为 false → **不 return** → 继续执行到 `:87`/`:89` 的 `Rsvg.Handle.new_from_*`，而 `Rsvg.Handle` 是 nil → **`attempt to index a nil value (field 'Handle')`**。

而且 **`load_and_apply`（`:131-139`）不是 protected call**：

```lua
local function load_and_apply(ib, file, image_loader, image_setter)
    local image_applied
    local object, cache = image_loader(file, ib._private.stylesheet_og)   -- 无 pcall
    if object then image_applied = image_setter(ib, object, cache) end
    return image_applied
end
```

异常直接上抛到 `set_image` 的调用方。调用方无一包裹 pcall：`awful/menu.lua:637,651`、`awful/titlebar.lua:1029`、`awful/widget/layoutbox.lua:38`、`awful/widget/common.lua:194`、`naughty/widget/icon.lua:105,178`、`naughty/layout/legacy.lua:535`。

**结论**：`M.Rsvg = {}` 让守卫完全失效，**svg 回退路径不存在**。任何 `set_image("<png 路径>")` 都会在取 png 回退之前先崩在 `Rsvg.Handle`。这是 B1 级阻塞。

**修法（只列不改）**：`lgishim.lua:34` 的 `M.Rsvg = {}` 应改为 `M.Rsvg = nil`。这样 `:54` 的 `Rsvg = lgi.Rsvg` 赋 nil，`:70` 的 `if not Rsvg then return end` 恢复语义 → svg 静默跳过 → pixbuf PNG 回退生效。**注意不能**改成 `M.Rsvg = pcall(require, "lgishim_rsvg") and ... or {}`——那会重新落回真值表陷阱。

### 5.2 X11 复核：**「lib/ 零调用」结论成立，Part 10 可取消**

重新取证（`grep -rni` 大小写不敏感，排除 `lib/vendor/`）：

| 模式 | 命中 | 可执行代码命中 |
|---|---|---|
| `x11`（不区分大小写） | 7 | **0** |
| `xlib` / `xrandr` / `xinerama` / `xcb` / `lgi.X*` | 4 | **0** |

全部 11 处命中**逐条核实都是注释或文档字符串**：

| 行 | 内容 | 性质 |
|---|---|---|
| `awful/screen.lua:772` | `-- The default DPI comes from the X11 server.` | 注释 |
| `awful/screen.lua:217` | `-- (Xinerama) screen as a cairo surface.` | 注释 |
| `awful/screen.lua:746` | `-- It is calculated using the information provided from xrandr.` | 注释 |
| `awful/screen.lua:877` | `-- the xrandr command is needed.` | 注释 |
| `awful/permissions/init.lua:478` | `-- Computes the actual X11 atoms before/after` | 注释 |
| `awful/keyboard.lua:215` | `--- Get X11 keysym and ...` | 文档字符串 |
| `awful/spawn.lua:172` | `-- by X11 applications to bridge the chain ...` | 注释 |
| `awful/button.lua:83` | `-- @propertyunit X11 mouse button codes.` | 文档字符串 |
| `awful/wallpaper.lua:384,785` | `-- Uploading the surface to X11...` / `-- Repainting the native X11 wallpaper...` | 注释 |
| `awful/popup.lua:430` | `-- ... thanks to XCB` | 注释（反讽） |

**佐证**：awesome 的 X11 交互全部走 C 层（`objects/screen.c`、`objects/client.c`、`lib/xcbproxy` 一类），Lua 侧只通过 `screen.*` / `client.*` / `gears.surface` 间接使用，**没有任何一处 `lgi.X11` / `lgi.XRandR`**。`grep 'naughty\|awful' luaa.c` 亦无 X11 lgi 注册。

**结论**：`spike4-phase2-06-x11-pixbuf-scope.md:30` 的「X11/XRandR 无 LGI 侧调用面，Part 10 是空集」**复核成立**。Part 10 可取消。

## 6. F — 与旧 scope 文档的冲突清单

> **原口径是「只列差异，不改旧文档」；2026-09-24 已按 §F 的清单就地校定三份旧文档**，校定处以 `⚠ 2026-09-24 校定` 标记，每份文档尾部有「本次更新说明」列出改动项。本节保留原始清单作为审计依据——其中引用的行号是**校定前**快照，内容可用 `⚠ 校定` 检索。三份工作：`spike4-phase2-01-scope.md`、`spike4-phase2-05-glib-gio-scope.md`、`spike4-phase2-06-x11-pixbuf-scope.md`。

### F1 🔴 **最高危**：旧 scope 判定「`Rsvg = {}` 空表安全」—— 这个判断是错的

**旧文档**：`spike4-phase2-01-scope.md:149`

> `imagebox.lua:408,423` 用 `Rsvg and Rsvg.Handle:is_type_of(image)` 判空，**空表使短路求值返回 `nil`（不报错），后续走 PNG 降级路径**

`spike4-phase2-01-scope.md:295` 更进一步：`Handle×4（imagebox.lua 的 pcall 保护，空表 fallback 安全）`；`:361` `Rsvg | ❌ {} 空表 | 决策 B：保持空表（安全降级）`。

**复核结论：两条论据都不成立。**

**(a) 「空表使短路求值返回 nil」—— Lua 语义错误。**
`a and b` 只在 **`a` 为 false/nil** 时短路返回 `a`。`{}` 是**真值**，所以右侧**会被求值**：

```lua
Rsvg = {}                                     -- 真值，不短路
Rsvg.Handle                            -- → nil
Rsvg.Handle:is_type_of(image)          -- → attempt to index a nil value (field 'Handle')  ❌ 报错
```

短路要生效，`Rsvg` 必须是 `nil` 或 `false`。**这是空表占位最危险的地方：它把「判空守卫」变成了「空指针解引用」。**

**(b) 「pcall 保护」—— 保护范围看错。**
`imagebox.lua:52-59` 的 pcall 只包住**赋值**：

```lua
local Rsvg = nil
do
    local success, err = pcall(function() Rsvg = lgi.Rsvg end)   -- 只保护这一行赋值
    ...
```

`:87`、`:89`、`:408`、`:423` 的 `Rsvg.Handle.*` 调用**全部在 pcall 之外**，且 `load_and_apply`（`:131-139`）也不是 protected call。零保护。

**(c) 「保持空表」的决策后果**：决策 B「只 PNG、Rsvg 人天 0」本意是对的，但**实现方式错**。要「只 PNG」应把 `lgishim.lua:34` 的 `M.Rsvg = {}` 改成 `M.Rsvg = nil`，让守卫真正生效；而不是留空表让守卫失效。**修法成本 ≈ 改 1 个字符**，收益是恢复整条 svg→png 降级链。

### F2 三份 scope **全部漏掉 Cairo 的 3 个真实缺口**

`grep -c` 逐份核对，三份旧文档中 `SvgSurface` / `new_sub_path` / `user_to_device` 的命中数**均为 0**：

| 缺口 | 调用点 | 三份 scope 是否提到 |
|---|---|---|
| `cairo.SvgSurface.create` | `gears/surface.lua:255`、`wibox/widget/init.lua:62` | ❌ 0 处 |
| `Context:new_sub_path()` | `gears/shape.lua:287,622,661` | ❌ 0 处 |
| `Context:user_to_device_distance()` | `wibox/widget/systray.lua:75` | ❌ 0 处 |

其中 `new_sub_path` 最容易被漏：`lgishim_cairo.c:1827` **内部确有 `cairo_new_sub_path(cr)` 的调用**（在 `ctx_circle` 实现里画整圆用），容易让盘点者误以为「已实现」。**但 CTX_M 里没有对应的绑定函数，Lua 侧 `cr:new_sub_path()` 仍会报 `attempt to call a nil value`**。这是「C 侧用了 API ≠ Lua 侧暴露了方法」的典型陷阱。

### F3 `spike4-phase2-01-scope.md:114` 的 `new_for_path × 9` —— 已被 v2 自我修正

- `spike4-phase2-01-scope.md:114`：`Gio.File.new_for_path` = **9**（filesystem 8 + beautiful/init 1）
- `spike4-phase2-05-glib-gio-scope.md:168,465`：已修正为 **10**，明确点出遗漏 `menubar/utils.lua:409` 的 `gio.` 小写别名

**复核：10 处成立**（filesystem.lua:34,47,55,67,79,91,103,202 = 8；beautiful/init.lua:274 = 1；menubar/utils.lua:409 = 1）。差异是 **phase2-scope 尚未更新**，不是 v2 出错。**注意：本次盘点同时发现 `spike4-phase2-01-scope.md` 的 KeyFile 命中数为 0 —— 即「完全没列 KeyFile」指的是这一份，不是 v2**（v2 在 `:112-127` 已列 KeyFile 10 个 API）。

### F4 `spike4-phase2-06-x11-pixbuf-scope.md:90` 「GdkPixbuf 去重 = 1 个 API」

- 旧文档：`GdkPixbuf.Pixbuf.new_from_file(path)` —— 仅此 1 个
- 本盘点：**2 个原子**（`GdkPixbuf.Pixbuf.new_from_file` + `pixbuf._native`），但 `_native` 是属性读取而非方法调用，**按「可调用 API」口径旧文档的 1 是对的**，不算实质冲突。

### F5 `spike4-phase2-05-glib-gio-scope.md:418` KeyFile 覆盖清单含 2 个 lib/ 未用到的方法

旧 scope 列的 KeyFile 覆盖清单里有 `:has_group`。复核：**`has_group` 确实被用到了**（`menubar/utils.lua` `keyfile:has_group` 1 处），所以这条**不是**多余实现。实测 lib/ 用到的 KeyFile 面是 **10 个名字**（`KeyFile()` + `load_from_file` + `has_group` + `get_keys` + `get_string` + `get_string_list` + `get_locale_string` + `get_locale_string_list` + `get_boolean` + `KeyFileFlags.NONE`），而 shim 的 `KEYFILE_M` 有 **14** 条，**富余 5 条**（`load_from_data`、`has_key`、`get_groups`、`get_int`、`get_double`）—— 富余无害，非缺口。

### F6 三份 scope 都**没提** `GLib.Timer` 缺 3 个方法

`spike4-phase2-05-glib-gio-scope.md` 提到 Timer 9 次，但**没有一条**指出 `Timer:stop/continue/start` 缺失。`spike4-phase2-01-scope.md` 提到 Timer 5 次，同样没有。`wibox/container/scroll.lua` 的 4 个调用点（232/244/255/257）在三份文档里均无记录。

### F7 三份 scope 都**没提** `beautiful/gtk.lua:74` 的 `lgi.require`

`grep -c 'lgi\.require'` 在三份文档中均为 0。这是 lib/ 里唯一一处 `lgi.require` 调用（调用 `lgi.require('Gtk', '3.0')`），shim 的 `M` 表**没有 `require` 字段**。好在有 pcall 兜底，优雅降级，但旧 scope 完全未记录这个调用点。

### F8 旧 scope 未记录的关键前置条件：`lgishim.lua` 从不 require `lgishim_gio`

三份 scope 均未指出 `lgishim.lua` **没有任何一行** `pcall(require, "lgishim_gio")`，`M.Gio = {}` 是**字面量**。这导致「写完 `lgishim_gio.c` + 注册 preload」仍然不足以让 Gio 生效——必须同时改 `lgishim.lua`。这是给当前 Gio 续写任务的最重要前置条件（见 §3.0）。

## 7. 前一轮 9 条待核实项复核结论

| # | 结论 | 判定 | 关键证据 |
|---|---|---|---|
| 1 | luaa.c 只注册 3 个 shim | **✅ 确认（已修复）** | 见下 7.1 |
| 2 | `M.Rsvg = {}` 是真值表陷阱 | **✅ 确认（比预想更严重）** | 见下 7.2 |
| 3 | `GLib.Timer` 缺 stop/continue/start | **✅ 确认（但文件名写错）** | 见下 7.3 |
| 4 | `GLib.Error`、`GLib.Bytes` 不在 shim | **⚠️ 部分成立** | 见下 7.4 |
| 5 | Cairo 侧 SvgSurface / new_sub_path / user_to_device_distance | **✅ 三条全确认** | 见下 7.5 |
| 6 | `beautiful/init.lua` 零 Rsvg 引用 | **✅ 确认** | 见下 7.6 |
| 7 | `lgishim_pangocairo.c` 不存在，实现体在 pango.c | **✅ 确认** | 见下 7.7 |
| 8 | `lib/awful/util.lua` 零 lgi | **✅ 确认** | 见下 7.8 |
| 9 | X11 侧 lib/ 零调用 | **✅ 确认** | 见下 7.9 |

### 7.1 #1：luaa.c preload 只注册 3 个 —— ✅ 确认（且主任务已修复）

**当前 `luaa.c:1140-1157` 已注册全部 6 个**：

```c
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "preload");
    lua_pushcfunction(L, luaopen_lgishim_pango);      lua_setfield(L, -2, "lgishim_pango");
    lua_pushcfunction(L, luaopen_lgishim_pangocairo); lua_setfield(L, -2, "lgishim_pangocairo");
    lua_pushcfunction(L, luaopen_lgishim_cairo);      lua_setfield(L, -2, "lgishim_cairo");
    lua_pushcfunction(L, luaopen_lgishim_glib);       lua_setfield(L, -2, "lgishim_glib");
    lua_pushcfunction(L, luaopen_lgishim_pixbuf);     lua_setfield(L, -2, "lgishim_pixbuf");
    lua_pushcfunction(L, luaopen_lgishim_gobject);    lua_setfield(L, -2, "lgishim_gobject");
    lua_pop(L, 2);
```

`luaa.c:70-75` 的 6 个 `extern int luaopen_lgishim_*` 声明也齐全。**主任务的修复到位。**

**为什么冒烟脚本漏掉它 —— 机理已确认**：`docs/sam/scripts/10-spike4-cairo-smoke.sh:50-55` 自己写了 6 个 `extern`，`:72-83` 自己写了一段 `lua_getfield(L,-1,"preload")` + `lua_pushcfunction` 循环，把 6 个 shim **在测试宿主 `smoke_main.c` 里重新注册了一遍**，然后测试脚本 `:134` 再 `require("vendor.lgishim")`。

→ **冒烟 harness 完全绕过了 `luaa.c:1140-1157`**。luaa.c 里哪怕漏注册全部 6 个，冒烟也照样全绿。这是「harness 复制粘贴了被测代码的逻辑」这类结构性缺陷——harness 与被测物同因同果，失去对照意义。`07-spike2-shim-smoke.sh:23` 甚至注释写着「照抄 luaa.c 的 preload 注册方式」，等于自证。

**给主任务的建议**：冒烟 harness 应改为在 `smoke_main.c` 里**只注册 `lgishim_pango`**（最小必要条件，因为 `lgishim.lua:20` 的 `pcall(require, "lgishim_pango")` 成功与否决定走 shim 分支还是真 lgi），其余 5 个**故意不注册**，然后加一条断言 `check(lgi.GLib and lgi.GLib.Variant, ...)` 去暴露 luaa.c 侧的注册。**或者更彻底**：让 harness 直接 include `luaa.c` 的注册函数，不要复制。

**同时补充一个旧结论未提的点**：`lgishim.lua` **从不** `pcall(require, "lgishim_gio")`，`M.Gio = {}` 是字面量。所以即使为 `lgishim_gio` 补齐 extern + preload，Gio 仍不可达。**必须同时改 `lgishim.lua`。**

### 7.2 #2：`M.Rsvg = {}` 是真值表 —— ✅ 确认，且旧 scope 有反向错误结论

`lgishim.lua:34` 的 `M.Rsvg = {}` 使 `imagebox.lua:54` 的 `pcall` 赋值成功 → `Rsvg = {}` 真值 → `imagebox.lua:70` 的 `if not Rsvg then return end` 守卫失效 → 走到 `:87/:89` 的 `Rsvg.Handle.*` → 对 nil 取字段崩溃。

**旧 scope 明确把这个判成「安全」**（`spike4-phase2-01-scope.md:149,295,361`），推理是「空表使短路求值返回 nil」——**Lua 语义错误**：`{}` 是真值，`a and b` 不会短路，右侧照常被求值。详见 §5.1 与 §F1。

### 7.3 #3：`GLib.Timer` 缺 3 个方法 —— ✅ 确认，**但文件名写错了**

**前一轮说 `lib/awful/scroll.lua` —— 该文件不存在**（`ls lib/awful/scroll.lua` → No such file）。

**实际文件是 `lib/wibox/container/scroll.lua`**，实测：

```
lib/wibox/container/scroll.lua:232   self._private.timer:stop()
lib/wibox/container/scroll.lua:244   self._private.timer:continue()
lib/wibox/container/scroll.lua:255   self._private.timer:start()
lib/wibox/container/scroll.lua:257   self._private.timer:stop()
lib/wibox/container/scroll.lua:425   ret._private.timer = GLib.Timer()
```

shim 侧 `lgishim_glib.c:729` 的 `TIMER_M` 只有 **2 条**，确认无 stop/continue/start。

**注意构造本身不炸**：`:425` 的 `GLib.Timer()` 走 `TIMER_M.new`（已注册），所以在创建 scroll widget 时不会崩；只有调用 `pause`/`continue`/`reset_scrolling` 时才炸。

### 7.4 #4：`GLib.Error` / `GLib.Bytes` —— ⚠️ 部分成立

「不在 shim 里」**成立**（`lgishim_glib.c` 无相关表）。但「是缺口」**不成立** —— lib/ 里**没有任何真实调用点**：

- `GLib.Error`：唯一命中是 `wibox/widget/textbox.lua:162` 的**注释**：`-- In case of error, attr is false and parsed is a GLib.Error instance.`
- `GLib.Bytes`：唯一命中是 `awful/spawn.lua:536` 的**注释**

→ 都是文档文字，不是代码。**无需实现**。

### 7.5 #5：Cairo 三项缺口 —— ✅ 三条全部确认

| 断言 | 复核 |
|---|---|
| `cairo.SvgSurface.create` 有 2 个真实调用点 | ✅ `gears/surface.lua:255`、`wibox/widget/init.lua:62` |
| shim 没有 `SvgSurface` | ✅ `lgishim_cairo.c` 中 grep `SvgSurface` 0 命中 |
| `lgishim_cairo.c:1827` 有 `new_sub_path` 实现但没注册进 CTX_M | ✅ `:1827` 确为 `cairo_new_sub_path(cr);`，但它在 `ctx_circle` 函数**内部**用于画整圆，**不是绑定函数**。CTX_M（74 条）中无 `new_sub_path` 条目 |
| `user_to_device_distance` 不在 CTX_M | ✅ CTX_M 中无此条目 |

lib/ 调用点：`gears/shape.lua:287,622,661`（new_sub_path）、`wibox/widget/systray.lua:75`（user_to_device_distance）。

**补充**：`ctx_circle` 里用了 `cairo_new_sub_path` 恰恰说明**这个 C API 是必要的**——`cr:new_sub_path()` 的 Lua 绑定缺失是真实缺陷，不是「多余实现」。

### 7.6 #6：`beautiful/init.lua` 零 Rsvg 引用 —— ✅ 确认

```
$ grep -c Rsvg lib/beautiful/*.lua
lib/beautiful/gtk.lua:0      lib/beautiful/init.lua:0
lib/beautiful/theme_assets.lua:0    lib/beautiful/xresources.lua:0
```

**任务预设「beautiful 用 Rsvg 渲染 svg」是错的**。`beautiful/theme_assets.lua` 只用 Cairo：`cairo.RecordingSurface`(:202)、`cairo.ImageSurface`、`cairo.Pattern`、`cairo.Rectangle`。全库唯一使用 Rsvg 的文件是 `lib/wibox/widget/imagebox.lua`。

### 7.7 #7：`lgishim_pangocairo.c` 不存在 —— ✅ 确认

```
$ ls lib/vendor/lgishim_*.c
lgishim_cairo.c  lgishim_gio.c  lgishim_glib.c  lgishim_gobject.c  lgishim_pango.c  lgishim_pixbuf.c
```

无 `lgishim_pangocairo.c`。`grep -n 'luaopen_lgishim_pangocairo' lib/vendor/lgishim_pango.c` 有定义（在 pango.c 内部）。`luaa.c:71` 的 `extern int luaopen_lgishim_pangocairo` 是独立声明，**链接期由 pango.c 提供的符号满足**，因此构建不受影响。

→ **不是功能缺口，是可读性/可维护性问题**。真正的风险是：将来有人照「一个文件一个 luaopen」的直觉去新建 `lgishim_pangocairo.c`，会造成重复符号。

### 7.8 #8：`lib/awful/util.lua` 零 lgi —— ✅ 确认

`grep -n 'lgishim\|lgi\.' lib/awful/util.lua` → 0 行。该文件只用 `gears.*` 与 `capi`。

**但它间接依赖 Gio**：`lib/awful/util.lua:251,257,273` 调用 `gfs.file_readable(...)`（`gfs` = `gears.filesystem`），而 `gears/filesystem.lua` 整个模块建立在 `Gio.File` / `Gio.IOErrorEnum` 上 → **`awful.util.get_icon_name` 等在图标加载时会间接炸**（B7）。所以「零 lgi」≠「零阻塞」。

### 7.9 #9：X11 侧 lib/ 零调用 —— ✅ 确认

`grep -rni 'x11' lib/ --include=*.lua | grep -v lib/vendor/` → **7 命中，全部是注释/文档字符串**；`xlib|xrandr|xinerama|xcb|lgi.X*` → **4 命中，也全部是注释**。可执行 X11 lgi 调用 = **0**。

详见 §5.2。`spike4-phase2-06-x11-pixbuf-scope.md:30,243` 的「Part 10 取消」结论复核成立。

### 7.10 复核过程中新发现的（前一轮未提）

1. **`require("awful")` 是启动即炸的最高危入口**（§A1）—— 4 条独立路径全部在模块加载期触发，比 `beautiful.init` 更早。前一轮 9 条里没有这一条。
2. **`lib/naughty/dbus.lua:439,540` 是模块顶层调用**，`require("naughty")` 即崩；且 `naughty/init.lua:12` 的 `if dbus then` **恒真**（`dbus` 是 C 注册的全局表，`luaa.c:1170`）。
3. **`lib/awful/init.lua` 的 `ret` 表是立即求值**，不是懒加载 —— 这是 #10 能升级为「启动即炸」的原因。
4. **`lib/wibox/drawingctx.lua` 不存在**（任务文件清单笔误），**`lib/awful/scroll.lua` 不存在**（前一轮笔误）。
5. **`beautiful/gtk.lua:74` 的 `lgi.require('Gtk','3.0')`** 是 lib/ 里唯一一处 `lgi.require`，shim 的 `M` 无 `require` 字段，但有 pcall 兜底，**优雅降级不炸**。三份旧 scope 均未记录。
6. **旧 scope 把 `Rsvg = {}` 判成「安全」**，这是三份文档里唯一一条会导致实际事故的错误判断（§F1）。

---

## 附 X：本次更新说明（2026-09-24 校定）

1. **§F 头部口径改写**：原文「本矩阵只列新发现，不改旧文档」在今天的就地校定中**作废**——本文 §F1（Rsvg 空表误判）已经在 `spike4-phase2-04-c-api-conventions.md §八·6` 就地改写。§F 保留原始清单供审计，行号是**校定前快照**；实际改动详见 01 / 04 / 05 / 06 各自尾部的「本次更新说明」。
2. **§7.9 文件名引用修复**：`spike4-x11-pixbuf-scope.md` → `spike4-phase2-06-x11-pixbuf-scope.md`（新命名规范）。
3. **顶部标注**：加一行「本矩阵是 Phase 2 期间的并行分析文档，跨全部 Part。逐 Part 执行记录见 `2026-09-23-record2.md` 顶部对照表」。
4. **本文 §F1 判断本身已失效**：`Rsvg = {}` 不再判为「安全」——见 04-c-api-conventions §八·6 的三档约定与两道接线护栏。
