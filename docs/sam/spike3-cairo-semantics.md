# Cairo API Semantics (Spike 3 D.3)

采集时间：2026-09-23 23:09 CST
范围：`lib/` 全量（171 个 .lua 文件）+ b16 一·3.1 / 一·8.5 的 oocairo 参照
方法：`find lib/ -name '*.lua'` 全量 grep，只读；未修改任何源码

## 0. 前置事实（与 b16 一·3.1 的差异校正）

b16 一·3.1 的"隐蔽坑"段落写道：
> `lib/gears/surface.lua` 第 35 / 48 / 70 / 109 / 119 行有 5 处 "lgi cairo surface" 类型判断，
> shim 必须提供可辨识标识（`__typename`），否则 `_to_surface` 这类转换函数**静默失败**。

**实测（本次核查）**：
- `lib/` 全量中 `__typename` 出现次数 = **0**。
- 真正的类型判定用的是 lgi 惯用式 `Xxx:is_type_of(obj)`，共 **9 处**（`surface.lua` 2、`color.lua` 2、`wallpaper.lua` 2、`imagebox.lua` 3）。
- 源码里 `-- Try to convert the argument into an lgi cairo surface.` 注释确实出现 5 次（`surface.lua:40/75/109/124` + 上下文），是 b16 计数来源；但注释和真正判定行是两回事。

**含义**：shim 必须实现 `is_type_of(self, other)` 元方法，而不是只塞 `__typename` 字段。`__typename` 依然建议保留（b16 一·2 L73-80 要求），但决定"静默失败还是画出来"的是 `is_type_of`。

---

## 1. Cairo 对象创建路径（`cairo.X(...)` 静态调用）

`lib/` 内出现的 cairo 静态类/常量：

| 静态标识 | 首次出现 | 出现次数 |
|---------|---------|:-------:|
| `cairo.Context` | `lib/gears/color.lua:353` | 35 |
| `cairo.Operator` | `lib/gears/surface.lua:170` | 33 |
| `cairo.ImageSurface` | `lib/gears/surface.lua:35` | 31 |
| `cairo.Format` | `lib/gears/surface.lua:35` | 25 |
| `cairo.Pattern` | `lib/gears/color.lua:137` | 20 |
| `cairo.Content` | `lib/gears/wallpaper.lua:52` | 12 |
| `cairo.RectangleInt` | `lib/wibox/drawable.lua:104` | 7 |
| `cairo.Extend` | `lib/gears/color.lua:151` | 6 |
| `cairo.Matrix` | `lib/gears/matrix.lua:118` | 5 |
| `cairo.Surface` | `lib/gears/surface.lua:54` | 5 |
| `cairo.Region` | `lib/wibox/drawable.lua:118` | 5 |
| `cairo.RecordingSurface` | `lib/beautiful/theme_assets.lua:202` | 3 |
| `cairo.Rectangle` | `lib/beautiful/theme_assets.lua:203` | 3 |
| `cairo.SvgSurface` | `lib/gears/surface.lua:255` | 2 |
| `cairo.FillRule` | `lib/wibox/container/margin.lua:36` | 1 |
| `cairo.SurfacePattern` | `lib/wibox/container/background.lua:109` | 1 |
| `cairo.RasterSourcePattern` | `lib/wibox/container/background.lua:110` | 1 |
| `cairo.LineCap` | `lib/wibox/layout/grid.lua:1456` | 1 |
| `cairo.Filter` | `lib/wibox/widget/imagebox.lua:328` | 1 |

合计 **19 个静态标识**。

### 1.1 具体构造点（按调用形态）

| 位置 | 构造形态 | 语义 |
|------|---------|------|
| `lib/gears/surface.lua:35` | `cairo.ImageSurface(cairo.Format.ARGB32, 0, 0)` | 空默认图，作为 load 失败 fallback |
| `lib/gears/surface.lua:72` | `cairo.Surface(_surface, true)` | **通用 surface 强制转换**（true = 不拷贝），b16 判定坑的关键 |
| `lib/gears/surface.lua:143/163/168/188/215/256/282/360` | `cairo.Context(surf)` | 从 surface 拿 ctx，共 8 处 |
| `lib/gears/surface.lua:255` | `cairo.SvgSurface.create(path, w, h)` | 输出 SVG |
| `lib/gears/surface.lua:281` | `cairo.ImageSurface(format or cairo.Format.ARGB32, w, h)` | widget_to_surface |
| `lib/gears/color.lua:137/151/354` | `cairo.Pattern.create_for_color / .create_linear / .create_radial` | 颜色渐变 |
| `lib/gears/color.lua:312` | `pattern.content = cairo.Content.COLOR_ALPHA` | **属性赋值**，不是方法 |
| `lib/gears/matrix.lua:118` | `cairo.Matrix{xx=..,yx=..,xy=..,yy=..,x0=..,y0=..}` | 构造 6 数矩阵 |
| `lib/gears/surface.lua:167` | `s:create_similar(s.content, w, h)` | 复制 source-compatible surface |
| `lib/gears/wallpaper.lua:52/64/139/181/224/251` | `source:create_similar(cairo.Content.COLOR, w, h)` | wallpaper 目标 |
| `lib/wibox/drawable.lua:104` | `cairo.RectangleInt{...}` 塞给 `dirty:union_rectangle` | 脏区矩形 |
| `lib/wibox/drawable.lua:118` | `cairo.Region.create()` + `:union_rectangle` + `:get_rectangle` + `:num_rectangles` + `:is_empty` | 脏区 Region |
| `lib/wibox/widget/init.lua:62` | `cairo.SvgSurface.create(path, w, h)` | `draw_to_svg_file` |
| `lib/wibox/container/background.lua:54` | `cairo.Pattern.create_linear(x0,y0,w,h)` | 线性渐变背景 |
| `lib/wibox/container/background.lua:76` | `cairo.Pattern.create_radial(...)` | 径向渐变 |
| `lib/wibox/container/background.lua:205` | `cairo.RecordingSurface(cairo.Content.COLOR_ALPHA, cairo.Rectangle{...})` | 遮罩记录 |
| `lib/awful/client/shape.lua:40` | `cairo.ImageSurface(cairo.Format.A1, w, h)` | 1-bit alpha 形状 |
| `lib/awful/screenshot.lua:70` | `cairo.ImageSurface(cairo.Format.RGB24, w, h)` | 屏幕截图 |
| `lib/awful/menu.lua:629` | `cairo.ImageSurface(cairo.Format.ARGB32, w, h)` | 菜单项渲染 |
| `lib/awful/widget/button.lua:43` | `img:create_similar(cairo.Content.COLOR_ALPHA, w, h)` | 按钮状态克隆 |
| `lib/wibox/container/tile.lua:35-36` | `cairo.Pattern.create_for_surface(surf)` + `.extend = cairo.Extend.REPEAT` | 平铺图案 |
| `lib/awful/placement.lua:832` | `cairo.Region.create_rectangle(cairo.RectangleInt(...))` | 区域相交 |

---

## 2. Cairo 方法调用清单（按类别）

调用形态均为 `obj:method(...)`（冒号调用），oocairo 天然支持冒号风格（`cr:line_to(x,y)`），
shim 的主要工作是把 oocairo 的**模块函数**（`context_create`、`image_surface_create` 等）
包装成冒号方法 + 属性 setter。

### 2.1 绘制原语

| 方法 | 首次出现 | 出现次数 | oocairo 支持 |
|------|---------|:-------:|:-----------:|
| `cr:line_to(x, y)` | `lib/gears/shape.lua:114` | **83** | ✓ |
| `cr:set_source(pattern)` | `lib/gears/color.lua:354` | **96** | ✓ |
| `cr:translate(x, y)` | `lib/gears/matrix.lua:74` | **52** | ✓ |
| `cr:fill()` / `cr:fill_preserve()` | `lib/gears/surface.lua:197` | 42 / 12 | ✓ |
| `cr:rectangle(x, y, w, h)` | `lib/gears/shape.lua:562` | 38 | ✓ |
| `cr:paint()` | `lib/gears/surface.lua:171` | 37 | ✓ |
| `cr:stroke()` / `cr:stroke_preserve()` | `lib/gears/shape.lua:13` | 35 / 3 | ✓ |
| `cr:arc(cx, cy, r, a1, a2)` | `lib/gears/shape.lua:240` | 27 | ✓ |
| `cr:move_to(x, y)` | `lib/gears/shape.lua:103` | 29 | ✓ |
| `cr:close_path()` | `lib/gears/shape.lua:157` | 19 | ✓ |
| `cr:scale(sx, sy)` | `lib/gears/matrix.lua:82` | 9 | ✓ |
| `cr:rel_line_to(x, y)` | `lib/gears/shape.lua:405` | 9 | ✓ |
| `cr:rotate(angle)` | `lib/gears/matrix.lua:89` | 6 | ✓ |
| `cr:mask(surface/pattern)` | `lib/gears/color.lua:355` | 4 | ✓ |
| `cr:rel_move_to(x, y)` | `lib/beautiful/theme_assets.lua:52` | 2 | ✓ |
| `cr:transform(matrix)` | `lib/gears/shape.lua:928` | 2 | ✓ |
| `cr:arc_negative(...)` | `lib/gears/shape.lua:759` | 1 | ✓ |
| `cr:mask_surface(surf, x, y)` | `lib/wibox/container/background.lua:231` | 1 | ✓（并入 mask） |
| `cr:paint_with_alpha(alpha)` | `lib/wibox/hierarchy.lua:378` | 1 | ✓ |
| `cr:show_text(str)` | `lib/wibox/widget/piechart.lua:44` | 1 | ✓ |
| `cr:stroke()`（形状轮廓） | — | 计上面 | ✓ |
| `cr:text_extents(str)` | `lib/wibox/widget/piechart.lua:37` | 1 | ✓ |
| `cr:text_path(str)` | — | 0（用 show_layout 代替） | ✓ |

### 2.2 状态（栈/变换/来源）

| 方法/属性 | 首次出现 | 次数 | oocairo 支持 |
|----------|---------|:---:|:-----------:|
| `cr:save()` / `cr:restore()` | `lib/gears/shape.lua:101/160` | 25 / 26 | ✓ |
| `cr:set_line_width(w)` | `lib/beautiful/theme_assets.lua:40` | 24 | ✓ |
| `cr:clip()` | `lib/gears/shape.lua:12` | 22 | ✓ |
| `cr:set_operator(op)` | `lib/gears/surface.lua:217` | 20 | ✓ |
| `cr:set_source_surface(surf, x, y)` | `lib/gears/surface.lua:169` | 20 | ✓（`set_source(surf,x,y)` 等价） |
| `cr:set_source_rgba(r,g,b,a)` | `lib/gears/surface.lua:218` | 15 | ✓ |
| `cr.operator = X` | `lib/gears/surface.lua:170` | **13** | ✗（oocairo 只有 `set_operator`） |
| `cr:set_source_rgb(r, g, b)` | `lib/wibox/drawable.lua:131` | 1 | ✓ |
| `cr:reset_clip()` | `lib/wibox/container/tile.lua:50` | 3 | ✓ |
| `cr:clip_extents()` | `lib/gears/surface.lua:144` | 3 | ✓ |
| `cr:push_group()` / `cr:push_group_with_content(c)` | `lib/wibox/hierarchy.lua:351` / `lib/wibox/container/background.lua:252` | 2 / 3 | ✓ / ✓（oocairo 名 `push_group([content])`） |
| `cr:pop_group()` / `cr:pop_group_to_source()` | `lib/wibox/container/background.lua:313` / `lib/wibox/hierarchy.lua:376` | 3 / 2 | ✓ / ✓ |
| `cr:new_path()` | `lib/wibox/hierarchy.lua:361` | 2 | ✓ |
| `cr:get_source()` | `lib/wibox/container/tile.lua:34` | 3 | ✓ |
| `cr:set_matrix(m)` / `cr:get_matrix()` | `lib/wibox/container/background.lua:209/202` | 1 / 2 | ✓ |
| `cr:set_fill_rule(r)` | `lib/wibox/container/margin.lua:36` | 1 | ✓ |
| `cr:set_dash(t, off)` | `lib/wibox/layout/grid.lua:1452` | 1 | ✓ |
| `cr:set_line_cap(cap)` | `lib/wibox/layout/grid.lua:1456` | 1 | ✓ |
| `cr.get_source().set_filter(f)` | `lib/wibox/widget/imagebox.lua:328` | 1 | ✓（`pattern:set_filter("bilinear")`） |
| `cr:get_current_point()` | `lib/wibox/widget/piechart.lua:33` | 1 | ✓ |

### 2.3 Surface 生命周期

| 方法 | 首次出现 | 次数 | oocairo 支持 |
|------|---------|:---:|:-----------:|
| `img:finish()` | `lib/gears/surface.lua:228` | **18** | ✓ |
| `surf:create_similar(content, w, h)` | `lib/gears/surface.lua:167` | 7 | ✓（`surface_create_similar(s, c, w, h)`） |
| `surf:flush()` | `lib/wibox/container/background.lua:228` | 1 | ✓ |
| `img._native` | 10 处 | 10 | ✗（oocairo 无此字段，需打补丁） |

### 2.4 属性赋值（**oocairo 不支持**）

| 属性 | 首次出现 | 次数 | oocairo 对应 |
|------|---------|:---:|:-----------:|
| `cr.operator = cairo.Operator.X` | `lib/gears/surface.lua:170` | 13 | `cr:set_operator("x")` |
| `pattern.extend = cairo.Extend.X` | `lib/gears/wallpaper.lua:179` | 2 | `pattern:set_extend("x")` |
| `pattern.content = cairo.Content.X` | `lib/gears/color.lua:312` | 7 | `pattern:set_content("x")` |
| `cr.fill_rule = X` | — | 1 | `cr:set_fill_rule("x")` |
| `cr.line_width = X` | `lib/wibox/container/background.lua:328` | 1 | `cr:set_line_width(x)` |
| `matrix.xx/yx/xy/yy/x0/y0 = X` | `lib/gears/matrix.lua` 全文件 | ~22 | oocairo matrix 就是 `{xx,yx,xy,yy,x0,y0}` 表 |
| `cr.status`（**读**） | `lib/wibox/drawable.lua:166` | 11 | ✗（oocairo 无 status 字段） |

### 2.5 Pango-Cairo 桥接（**oocairo 无**，须自建）

| 方法 | 位置 | 说明 |
|------|------|------|
| `cr:update_layout(layout)` | `lib/wibox/widget/textbox.lua:44` | PangoCairo 特有：把 Pango Layout 绑到 ctx |
| `cr:show_layout(layout)` | `lib/wibox/widget/textbox.lua:53` | PangoCairo 特有：用 ctx 画 Layout |
| `cr:text_extents(str)` | `lib/wibox/widget/piechart.lua:37` | oocairo 有，但返回表结构一致 |

---

## 3. Surface 类型检查

### 3.1 `__typename` 场景

**0 处**。lib/ 里没有一处使用 `__typename` 判定 cairo 对象类型。

### 3.2 `:is_type_of` 场景（9 处）

| 位置 | 检查对象 | 判定目标 |
|------|---------|---------|
| `lib/gears/surface.lua:54` | `_surface` | `cairo.Surface` |
| `lib/gears/surface.lua:67` | `_surface`（`pixbuf_to_surface` 返回值） | `cairo.Surface` |
| `lib/gears/color.lua:263` | `col` | `cairo.Pattern` |
| `lib/gears/color.lua:288` | `col` | `cairo.Pattern` |
| `lib/gears/wallpaper.lua:104` | `pattern` | `cairo.Surface` |
| `lib/gears/wallpaper.lua:110` | `pattern` | `cairo.Pattern` |
| `lib/wibox/widget/imagebox.lua:408` | `image` | `Rsvg.Handle`（**非 cairo**） |
| `lib/wibox/widget/imagebox.lua:423` | `image` | `Rsvg.Handle` |
| `lib/wibox/widget/imagebox.lua:427` | `image` | `cairo.Surface` |

`lib/` 里唯一的类型判定语义就是 `is_type_of`，且它同时被 `Rsvg.Handle` 用。shim 里必须：
1. 给 `Ctx`、`Surface`、`Pattern` 三个元表都实现 `is_type_of(self, other)`；
2. `Rsvg` 骨架（`lgishim.lua:29` 的空表）也要给 `Rsvg.Handle` 元表加 `is_type_of`；
3. `is_type_of` 语义 = "other 的元表是否是 self"（lgi 的等价实现）。

### 3.3 `._native` 场景（10 处）

`._native` 是**跨绑定裸指针通道**，b16 一·2 L73-80 明确要求"恒返回 lightuserdata"。

| 位置 | 用法 |
|------|------|
| `lib/gears/surface.lua:63` | `pixbuf._native` 传给 `capi.awesome.pixbuf_to_surface` |
| `lib/gears/surface.lua:227` | `img._native` 赋给 `draw.shape_bounding` |
| `lib/wibox/init.lua:133/152/173` | wibox 的 `shape_bounding / shape_clip / shape_input` |
| `lib/awful/_compat.lua:71/76` | `pattern._native` 判空 + 传给 `root._wallpaper` |
| `lib/awful/mouse/snap.lua:70` | `img._native` 赋 `placeholder_w.shape_bounding` |
| `lib/awful/client/shape.lua:105/117` | `res._native` 赋 `c.shape_bounding / c.shape_clip` |

**oocairo 无 `._native`**。需给 `Ctx / Surface / Pattern` 元表都加 `._native` 字段（返回裸指针）。

---

## 4. 与 oocairo 的对照覆盖度

oocairo（`awesomeWM/oocairo`，官方，2025-08 活跃）本身是**冒号方法风格**（`cr:line_to(...)`），
与 LGI 一致。差距不在方法调用语法，而在**构造函数、常量类型、属性赋值、Pango 桥接**四处。

- **lib/ 用到的 cairo 侧 API 总数**：**56**（含属性赋值 6 组）
- **oocairo 直接支持**：**47**（~84%）
- **需要 shim 层包装/补丁**：**9 类**，见下

### 4.1 直接支持（47）
绘制（24）+ 状态（20）+ 字体（3）+ Surface 生命周期（3，其中 2 个 `finish/flush` 全支持）+ Pango 无关的 `text_extents/show_text`。

### 4.2 未覆盖 / 需 shim 层处理（9 类，K=9）

| # | 缺口 | 位置数量 | 处理方式 |
|:-:|------|:------:|----------|
| 1 | **构造函数改名**：`cairo.Context(s)` → `context_create(s)`；`cairo.ImageSurface(...)` → `image_surface_create(...)`；`cairo.SvgSurface.create` → `svg_surface_create`；`cairo.RecordingSurface` → `recording_surface_create`；`cairo.Pattern.create_*` → `pattern_create_*`；`cairo.Matrix{...}` → `matrix_create()` + set；`cairo.Surface(obj, true)` → 通用包装（oocairo **无**此 API） | 20+ 处 | shim 层实现 `Ctx.Create(s)` / `ImageSurface.Create(fmt,w,h)` 等 factory |
| 2 | **常量枚举→字符串**：`cairo.Operator.SOURCE` → `"source"`；`cairo.Format.ARGB32` → `"argb32"`；`cairo.Content.COLOR` → `"color"`；`cairo.Extend.PAD` → `"pad"`；`cairo.FillRule.EVEN_ODD` → `"even-odd"`；`cairo.LineCap.SQUARE` → `"square"`；`cairo.Filter.BILINEAR` → `"bilinear"` | ~150 处 | shim 层做 enum→string 代理表；或者把 oocairo 的字符串值包装成 lgi 风格的伪常量表 |
| 3 | **属性赋值** `cr.operator = X` | 13 | metatable `__newindex` 拦截 → `set_operator` |
| 4 | **属性赋值** `pattern.extend/content = X` | 9 | 同上，`__newindex` → `set_extend/set_content` |
| 5 | **属性读** `cr.status`（string `"SUCCESS"`） | 11 | oocairo 无，shim 里加 getter（默认返回 `"SUCCESS"`） |
| 6 | **`._native` 裸指针** | 10 | 打补丁到 oocairo，或 shim 里加 `._native = function(self) return raw(self) end` |
| 7 | **`:is_type_of(other)`** | 9 | metatable `is_type_of` 方法（同时给 `Rsvg.Handle`） |
| 8 | **`cr:show_layout(layout)` / `cr:update_layout(layout)`** | 2 | PangoCairo 专有。需 `lgishim_pangocairo.c` 内部实现：从 Pango Layout 拿 text/glyphs，调 `cairo_show_glyphs`；lib/ 里只有 `piechart.lua` 用 `cr:text_extents + cr:show_text`，可以直接映射到 oocairo 的同名方法 |
| 9 | **`cairo.Surface(obj, true)` 通用转换**（`surface.lua:72`） | 1 | shim 里实现"obj 已是 Surface→原样返回；是 GdkPixbuf→转换；是字符串→从文件加载；其他→报错" |

**未覆盖 API 总数**：**9 类**（对应约 200 处调用点，但都在 shim 层一处搞定）。

---

## 5. 结论

### 5.1 施工成本估算（对齐 b16 一·7.1 的 "Cairo 2-4 人天"）

| 子项 | 估计 | 依据 |
|------|------|------|
| 19 个静态构造器包装（factory 层） | 0.3 人天 | 纯表格映射 |
| 7 个 enum → string 常量代理表 | 0.2 人天 | `Operator/Format/Content/Extend/FillRule/LineCap/Filter` |
| 47 个方法直接透传（冒号语法天然兼容） | 0 | oocairo 语法匹配 |
| metatable `__newindex` 拦截属性赋值（4 类） | 0.3 人天 | `cr.operator / pattern.extend / pattern.content / cr.line_width / cr.fill_rule` |
| `._native` 补丁 + `is_type_of` + `status` getter | 0.5 人天 | b16 一·2 L73-80 明确要求 |
| `cairo.Surface(obj, true)` 通用转换器 | 0.2 人天 | 参考现有 `surface.lua:48-72` 语义 |
| `show_layout` / `update_layout` Pango 桥接 | 0.5 人天 | 复用 `lgishim_pangocairo.c` 已有通道 |
| 集成测试（`gears.surface` 输出纯色图 + `imagebox` 加载 PNG + `wibox/hierarchy` 画 widget） | 0.5-1 人天 | b16 一·5 验收信号 |
| **合计** | **2.5-3 人天** | 落在 b16 "Cairo 2-4" 区间偏下 |

单位成本：约 **1 行/API 单位 × 56 API** = ~60 行工厂代码 + 40 行 enum 表 + 80 行 metatable 拦截 + 60 行 Pango 桥接 + 测试 ≈ **250-400 行 shim**，与 b16 一·3.1 "约 200-400 行" 一致。

### 5.2 关键难点

1. **`__typename` 是伪命题**（重要修正）。b16 一·3.1 把 `__typename` 说成隐蔽坑，但实测 lib/ 一次都不用。真正决定成败的是 **`is_type_of` 元方法**（9 处调用）和 **`._native` 裸指针**（10 处）。若只做 `__typename` 不做 `is_type_of`，`surface.load_*` 依然会静默走 fallback 分支——但报错信息是"画不出图"，排查方向一致，成本 2-3 天。**建议同时做**。

2. **常量类型是最大面积改造**。~150 处 `cairo.Operator.SOURCE` / `cairo.Format.ARGB32` / `cairo.Content.COLOR` 等，oocairo 用字符串。若把 oocairo 的字符串值包装成 lgi 风格的伪常量表（`cairo.Operator.SOURCE = "source"` 之类），可以零改动 lib/；否则每个调用点都要改。**推荐做伪常量表**，成本 0.2 人天。

3. **`cr.operator = X` 属性赋值**（13 处）。oocairo 无属性赋值支持。必须用 `__newindex` 拦截。同时 `pattern.extend / pattern.content` 也是 9 处。这些是**唯一需要给多个元表都写 `__newindex`** 的地方。

4. **`cr:show_layout(layout)` / `cr:update_layout(layout)`**。lib/wibox/widget/textbox.lua 完全依赖这两个方法，但 oocairo 里没有。必须在 `lgishim_pangocairo.c` 里实现：把 Pango Layout 的 glyph run 序列抽出来，转成 oocairo 的 `show_glyphs(glyphs)` 参数格式。**这是 0.5 人天的核心工作**。

5. **`cr.status` 属性读**（11 处）。lib/wibox/drawable.lua:166 的 `assert(cr.status == "SUCCESS", ...)` 是运行时错误检测的关键。oocairo 不返回 status。shim 里默认返回 `"SUCCESS"`（相当于关闭这个断言），或者通过 `cairo_status_check` 补丁加真值。**若不做，wibox 出错会静默**。

6. **`cairo.Surface(obj, true)` 通用转换**（`surface.lua:72`）。这个 API 是 awesome 的"万能入口"：obj 可以是 nil / string / GdkPixbuf / 已有 Surface / 其他。oocairo 无对应。shim 里 20 行代码可实现。

### 5.3 意外的 cairo 依赖

在扫描过程中发现的**非表面 cairo 依赖**：

| 依赖 | 文件 | 说明 |
|------|------|------|
| **Rsvg** | `lib/wibox/widget/imagebox.lua:52-56` | `pcall(function() Rsvg = lgi.Rsvg end)`。已按 `docs/sam/spike3-librsvg-decision.md` D.4 决策砍掉。**lib/wibox/widget/imagebox.lua 依赖 Rsvg 加载 SVG 图标**；默认主题若含 SVG 图标需要 shim（0 或 1 人天）。 |
| **PangoCairo** | `lib/beautiful/init.lua:18`、`lib/wibox/widget/textbox.lua:19` | 已有 `lgishim_pangocairo.c` 骨架。`cr:show_layout / update_layout` 必须在这里实现。 |
| **GdkPixbuf** | `lib/gears/surface.lua:13`、`lib/wibox/widget/imagebox.lua` | 图片加载。b16 一·3.6 单列 "GdkPixbuf 0-2 人天"。 |
| **Gdk 无** | — | 没有 `lgi.Gdk` 依赖（GTK3 层 0 人天，b16 一·3.5） |
| **`_native` 契约** | 10 处 | 跨绑定裸指针通道。**lib/wibox/init.lua:133/152/173** 是把 surface 传给 `capi.wibox` 的核心通道；`lib/awful/client/shape.lua:105/117` 是给 `capi.client` 设形状。这些必须返回 lightuserdata，不能是 Lua table 包装。 |
| **lib/naughty/dbus.lua:19** | `lgi.cairo, lgi.Gio, lgi.GLib, lgi.GObject` | dbus 通知里也用了 cairo（渲染 notification 图标），但依赖 Gio，属于 Spike D.2/D.4 已识别的"真真空缺"。 |
| **`lib/wibox/layout/grid.lua:1512`** | `create_border_mask(... cr:get_source())` | grid layout 的 border mask 依赖 `cr:get_source()` 返回 pattern 对象。**必须给 pattern 也实现 `:set_filter`**。 |

**未发现** `imagebox.lua` 之外的**独立** SVG 依赖路径；`lib/awful/menu.lua:629` 用 `cairo.ImageSurface` 渲染 menu 项，不走 Rsvg。

### 5.4 一句话结论

Cairo 层用 oocairo + 250-400 行 shim 完全可覆盖；**主要成本不在方法调用（oocairo 天然冒号语法），而在 4 个特殊点**：
1. `is_type_of` 元方法（**不是 `__typename`**，b16 一·3.1 有偏差）；
2. enum → string 常量代理（~150 处调用）；
3. `__newindex` 拦截属性赋值（22 处）；
4. `show_layout` / `update_layout` Pango 桥接（2 处但涉及 pangocairo.c 新增函数）。

总成本 **2.5-3 人天**，落在 b16 一·7.1 "Cairo 2-4" 区间下沿。
