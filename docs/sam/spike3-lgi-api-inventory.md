# LGI API Inventory (Spike 3 D.1)

采集时间：2026-09-23 23:04 CST
范围：`lib/` 全量（`grep -rn`）
命令参考：
```bash
grep -rn '\._native' lib/           # 11 行
grep -rn ':finish()' lib/           # 18 行
grep -rn ':is_type_of()' lib/       # 0 行（带参数形式见下）
grep -rohE 'lgi\.[A-Za-z_]+' lib/ | sort | uniq -c | sort -rn
```

---

## 1. `._native` 调用统计

跨 C 边界通道：LGI userdata → `lightuserdata`（原始指针）→ C API。
grep 命中 11 行；其中 `lib/vendor/lgishim_pango.c:10` 是 C 源码注释（描述本机制），**实际代码调用为 10 处**，全部集中在 image / pixbuf / surface 的传递链上。

| # | 文件:行 | 上下文 | 语义 |
|---|---------|--------|------|
| 1 | `lib/gears/surface.lua:63` | `_surface = capi.awesome.pixbuf_to_surface(pixbuf._native, _surface)` | pixbuf → C API（cairo surface） |
| 2 | `lib/gears/surface.lua:227` | `draw.shape_bounding = img._native` | image surface → shape |
| 3 | `lib/wibox/init.lua:133` | `self.shape_bounding = img._native` | background image → wibox shape |
| 4 | `lib/wibox/init.lua:152` | `self.shape_clip = img._native` | clip image → wibox shape |
| 5 | `lib/wibox/init.lua:173` | `self.shape_input = img._native` | input image → wibox shape |
| 6 | `lib/awful/_compat.lua:71` | `pcall(function() return pattern._native end)` | 探测 pattern 是否原生 |
| 7 | `lib/awful/_compat.lua:76` | `return root._wallpaper(pattern._native)` | 传统 `_wallpaper` 回退路径 |
| 8 | `lib/awful/mouse/snap.lua:70` | `placeholder_w.shape_bounding = img._native` | snap 占位图形 |
| 9 | `lib/awful/client/shape.lua:105` | `c.shape_bounding = res and res._native` | client shape（bounding） |
| 10 | `lib/awful/client/shape.lua:117` | `c.shape_clip = res and res._native` | client shape（clip） |

参考注释：`lib/vendor/lgishim_pango.c:10` — “`._native` returns lightuserdata (the raw pointer), the only channel across the C boundary”。

**总数：11（grep 原始）／10（去注释后代码调用）**

---

## 2. `:finish()` 调用统计

全部命中都是 cairo Surface / 类 Surface userdata 的 `finish()`，用于释放 `ImageSurface` / `SvgSurface` 等资源。裸 `finish()`（无冒号）grep 结果为 **0**——所有释放都走方法形式。

| # | 文件:行 | 上下文 |
|---|---------|--------|
| 1 | `lib/gears/surface.lua:228` | `img:finish()`（紧接 `draw.shape_bounding = img._native` 之后） |
| 2 | `lib/gears/wallpaper.lua:59` | `paper.surface:finish()` |
| 3 | `lib/gears/wallpaper.lua:154` | `surf:finish()` |
| 4 | `lib/gears/wallpaper.lua:184` | `surf:finish()` |
| 5 | `lib/gears/wallpaper.lua:227` | `surf:finish()` |
| 6 | `lib/gears/wallpaper.lua:268` | `surf:finish()` |
| 7 | `lib/naughty/dbus.lua:122` | `surf:finish()` |
| 8 | `lib/wibox/container/tile.lua:175` | `ret._private.surface:finish()` |
| 9 | `lib/wibox/container/background.lua:241` | `s:finish()` |
| 10 | `lib/wibox/init.lua:134` | `img:finish()`（紧接 133 行 `shape_bounding`） |
| 11 | `lib/wibox/init.lua:153` | `img:finish()`（紧接 152 行 `shape_clip`） |
| 12 | `lib/wibox/init.lua:174` | `img:finish()`（紧接 173 行 `shape_input`） |
| 13 | `lib/wibox/widget/init.lua:69` | `img:finish()` |
| 14 | `lib/awful/mouse/snap.lua:71` | `img:finish()`（紧接 70 行 `shape_bounding`） |
| 15 | `lib/awful/wallpaper.lua:378` | `target:finish()` |
| 16 | `lib/awful/client/shape.lua:55` | `shape_img:finish()` |
| 17 | `lib/awful/client/shape.lua:108` | `res:finish()`（紧接 105 行 `shape_bounding`） |
| 18 | `lib/awful/client/shape.lua:120` | `res:finish()`（紧接 117 行 `shape_clip`） |

**总数：18**

---

## 3. `:is_type_of()` 调用统计

- 严格空参形式 `:is_type_of()`：**0**
- 带参数形式 `:is_type_of(x)`（这才是实际调用形态）：**9**

| # | 文件:行 | 上下文 |
|---|---------|--------|
| 1 | `lib/gears/color.lua:263` | `if cairo.Pattern:is_type_of(col) then` |
| 2 | `lib/gears/color.lua:288` | `if cairo.Pattern:is_type_of(col) then` |
| 3 | `lib/gears/surface.lua:54` | `if cairo.Surface:is_type_of(_surface) then` |
| 4 | `lib/gears/surface.lua:67` | `if cairo.Surface:is_type_of(_surface) then` |
| 5 | `lib/gears/wallpaper.lua:104` | `if cairo.Surface:is_type_of(pattern) then` |
| 6 | `lib/gears/wallpaper.lua:110` | `if not cairo.Pattern:is_type_of(pattern) then` |
| 7 | `lib/wibox/widget/imagebox.lua:408` | `... Rsvg and Rsvg.Handle:is_type_of(image) ...` |
| 8 | `lib/wibox/widget/imagebox.lua:423` | `elseif Rsvg and Rsvg.Handle:is_type_of(image) then` |
| 9 | `lib/wibox/widget/imagebox.lua:427` | `elseif cairo.Surface:is_type_of(image) then` |

按被检查类型分：
- `cairo.Surface:is_type_of(...)` — 4 处
- `cairo.Pattern:is_type_of(...)` — 3 处
- `Rsvg.Handle:is_type_of(...)` — 2 处

**总数：9（带参数）**

---

## 4. lgi 模块表分布

`grep -rohE 'lgi\.[A-Za-z_]+' lib/ | sort | uniq -c | sort -rn`：

| 模块 | 出现次数 | 主要使用者 |
|------|:-------:|-----------|
| `lgi.Pango` | 15 | `lib/gears/color.lua`, `lib/wibox/widget/textbox.lua`, `lib/naughty/notification.lua`, `lib/awful/hotkeys_popup/widget.lua`, `lib/beautiful/init.lua`, `lib/vendor/lgishim_pango.c` |
| `lgi.PangoCairo` | 4 | `lib/beautiful/init.lua`, `lib/wibox/widget/textbox.lua`, `lib/vendor/lgishim_pango.c` |
| `lgi.GLib` | 4 | `lib/awful/spawn.lua`, `lib/menubar/utils.lua`, `lib/naughty/dbus.lua`, `lib/wibox/container/scroll.lua` |
| `lgi.Gio` | 4 | `lib/awful/spawn.lua`, `lib/gears/filesystem.lua`, `lib/menubar/utils.lua`, `lib/naughty/dbus.lua` |
| `lgi.cairo` | 3 | `lib/gears/color.lua`, `lib/naughty/dbus.lua`, `lib/wibox/widget/imagebox.lua` |
| `lgi.version` | 1 | `lib/vendor/lgishim.lua`（元表：版本查询） |
| `lgi.Rsvg` | 1 | `lib/wibox/widget/imagebox.lua` |
| `lgi.require` | 1 | `lib/beautiful/gtk.lua`（元表：模块加载） |
| `lgi.GObject` | 1 | `lib/naughty/dbus.lua`（`Object` 基类） |

**模块总数：9**（含 `lgi.version` / `lgi.require` 两个 lgi 自身的元 API，实际业务模块 7 个：`Pango` / `PangoCairo` / `GLib` / `Gio` / `cairo` / `Rsvg` / `GObject`）

**GioUnix 应为 0**（b16 一·6 硬约束），实际值：**0** ✓

补充：lib/ 全量 grep `GioUnix` 无任何命中，符合约束。X11 / XrRandR / OverlayScroll 等模块也均为 0——它们只在 `modules/` 侧被直接加载，未通过 `lgi.XXX` 形式出现在 lib 中。

---

## 5. 结论

- **`._native` 调用面：小、聚焦**。10 处代码调用，全部围绕 “cairo/LGI 图形对象 → C API 指针传递” 这一条通道，集中在 5 个文件（`surface.lua` / `wibox/init.lua` / `awful/_compat.lua` / `awful/mouse/snap.lua` / `awful/client/shape.lua`）。所有调用都发生在 image/surface/pattern 三个类型上。shim 覆盖面小、类型单一。

- **`:finish()` 调用面：中、集中**。18 处，全部是 Surface 释放；`lib/gears/wallpaper.lua` 独占 5 处，`lib/awful/client/shape.lua` 3 处，`lib/wibox/init.lua` 3 处。无裸 `finish()`，无跨模块的 API 变体。shim 只需保证 `Surface:finish()` 释放路径生效。

- **`:is_type_of()` 调用面：小**。9 处带参数调用（无空参调用），检查目标只有 3 类：`cairo.Surface` / `cairo.Pattern` / `Rsvg.Handle`。集中在 `lib/gears/color.lua` / `lib/gears/surface.lua` / `lib/gears/wallpaper.lua` / `lib/wibox/widget/imagebox.lua`。

- **需 shim 覆盖的 lgi 模块清单**（按 lib/ 出现）：
  - `lgi.cairo` — 覆盖 cairo 类型检查 / surface / pattern 的构造与 `:is_type_of()` 判定
  - `lgi.Pango` — lib 内最高频，15 处；文字排版主链路
  - `lgi.PangoCairo` — Pango 与 cairo 桥接，4 处
  - `lgi.GLib` — 通用工具（spawn、scroll 等），4 处
  - `lgi.Gio` — D-Bus / filesystem / spawn，4 处
  - `lgi.Rsvg` — SVG 加载，1 处（imagebox.lua）
  - `lgi.GObject` — 仅 D-Bus Object 基类，1 处
  - 元 API（`lgi.version` / `lgi.require`）需保持兼容以支持 `lgishim.lua` 与 `beautiful/gtk.lua` 的加载路径

- **异常观察**：
  - `lgi.Pango` 频次（15）远高于第二名 `lgi.PangoCairo`（4），符合预期——pango 是排版主入口。
  - `lgi.GObject` 仅 1 处但存在于 `naughty/dbus.lua`，属基类依赖，不可移除。
  - `lgi.cairo` 出现 3 次，但 `:is_type_of()` 与 `:finish()` 的实际调用远多于 `lgi.cairo` 字面量——因为大部分 cairo 对象通过 `lgishim.lua` 别名（`cairo = lgishim.cairo` 之类）引用，字面统计会低估 cairo 的实际调用面。**shim 覆盖 cairo 时不应只看 `lgi.cairo` 字面量**。
  - GioUnix 严格 0，无违规。
