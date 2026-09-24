# Spike 4 — Cairo Shim 冒烟测试框架

> **本文覆盖 Cairo shim 冒烟框架的**方法学**：独立宿主、断言哲学、断言矩阵、实操要点、规模演进。§1-§20 是 Part 1-3 时期定型的框架；Part 4-9 追加的 §21-§28 段（含两道接线护栏）在 §三、§七 就地列出，完整时序见 `2026-09-23-record2.md` 顶部对照表。**

**日期：** 2026-09-24 01:35–14:00
**状态：** ✓ 完成（**829/829 全通过**）
**分支：** `feat/ai-260923-static`
**核心资产：** `docs/sam/scripts/10-spike4-cairo-smoke.sh`（**2283 行**，**28 section**）
**结论：** Cairo shim 真实接到了 `libcairo`，不是空实现——每一个 cairo 状态变化都能通过**真实往返**断言被验证。GLib / GdkPixbuf / Gio / GObject shim 也用同一套方法学覆盖到。

---

## 一、为什么需要独立宿主

### 1、问题

lgi 的冒烟要 lgi runtime + typelibs，静态构建没有这两样。`require("lgi").cairo` 走 `dlopen("libcairo.so")`——在纯静态二进制里 100% 失败。

### 2、方案

自己写最小 Lua 宿主 `smoke_main.c`（`10-spike4-cairo-smoke.sh:43-83`），不依赖 awesome 主程序、不走 CMake、不走 `luaa.c` 的启动流程。

结构：

```c
// smoke_main.c
extern int luaopen_lgishim_pango(lua_State *L);
extern int luaopen_lgishim_pangocairo(lua_State *L);
extern int luaopen_lgishim_cairo(lua_State *L);

int main(int argc, char **argv) {
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);

    // 把三个 shim 注册到 package.preload
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "preload");
    lua_pushcfunction(L, luaopen_lgishim_pango);
    lua_setfield(L, -2, "lgishim_pango");
    // ... pangocairo, cairo 同理
    lua_pop(L, 2);

    // 直接 dofile 跑 Lua 测试脚本
    int rc = luaL_dofile(L, argv[1]) ? 1 : 0;
    if (rc)
        fprintf(stderr, "ERR: %s\n", lua_tostring(L, -1));
    lua_close(L);
    return rc;
}
```

关键点：

| 设计点 | 做法 | 为什么 |
|--------|------|--------|
| `package.preload` 注册 | 三个 `lua_setfield(L, -2, "lgishim_*")` | 对 `vendor/lgishim.lua` 的 `pcall(require, ...)` 完全透明——shim 加载器的探测回退逻辑零改动 |
| `luaL_dofile` | 不是 `luaL_dostring`、不是 `loadfile` | 错误消息自带文件名和行号，无需手动包装 |
| 打印 `lua_tostring(L,-1)` | `luaL_dofile` 失败后栈上有错误对象 | **不打印就只看到 `exit=1`，连哪行报错都不知道**——前两次调试都栽在这（`record2.md` 篇二 §5） |
| 零 awesome 代码 | 不链 `luaa.c`、不走 `main.c` | 隔离 shim 自身问题与 awesome 启动环境问题 |

这个模式继承自 Spike 2 的 `07-spike2-shim-smoke.sh`（第一个冒烟），在 42 项 Pango 冒烟后验证了可靠性。

---

## 二、静态链接行

### 1、完整链接参数

`10-spike4-cairo-smoke.sh:1083-1098`：

```bash
gcc -static -O1 -Wall -Wextra -Wno-unused-parameter \
    $(pkg-config --cflags pangocairo glib-2.0) \
    -I/usr/local/include \
    -include pango/pango.h \
    "$T/smoke_main.c" "$REPO/lib/vendor/lgishim_pango.c" \
    "$REPO/lib/vendor/lgishim_cairo.c" \
    -L/usr/local/lib -llua \
    -Wl,--start-group \
    -l:libpangocairo-1.0.a -l:libpangoft2-1.0.a -l:libpangoxft-1.0.a \
    -l:libpango-1.0.a -lcairo -lfreetype -l:libpng16.a \
    -lbrotlidec -lbrotlicommon -lharfbuzz -lgraphite2 -lfontconfig \
    -lfribidi -l:libpixman-1.a -luuid \
    -lgmodule-2.0 -lgio-2.0 -lgobject-2.0 -lglib-2.0 \
    -lexpat -lbz2 -lz -lpcre -lnettle -lffi -lintl -lm \
    -Wl,--end-group \
    -o "$T/smoke"
```

三个源文件合编为一个静态二进制：

| 源文件 | 作用 |
|--------|------|
| `smoke_main.c` | 最小 Lua 宿主（~40 行） |
| `lgishim_pango.c` | Pango + PangoCairo 绑定（Spike 2 产出） |
| `lgishim_cairo.c` | Cairo 绑定（Spike 4 产出） |

`-Wl,--start-group/--end-group` 包裹整个库表——这是 Spike 1 的关键发现：静态库单向扫描会漏符号，group 让 `ld` 反复扫描至收敛（`2026-09-23-record1.md` §8.3）。

### 2、`-include pango/pango.h` 为什么是必须的

`lgishim_cairo.c` 用到 `PangoLayout` 类型（`show_layout` / `update_layout` 桥接），但该文件自身 `#include` 的是 `<cairo.h>` / `<pango/pangocairo.h>`。在本容器的 Pango 1.48 头文件里，`pangocairo.h` 对 `PangoLayout` 是前向声明（`typedef struct _PangoLayout PangoLayout;`），而 `lgishim_cairo.c` 内联 `check_layout` 函数需要完整的 `PangoLayout` 类型来比较 metatable。

`-include pango/pango.h` 让 `PangoLayout` 的完整定义在**所有编译单元之前**被展开，绕过前向声明顺序问题（`10-spike4-cairo-smoke.sh:1081-1082` 注释说明）。

---

## 三、冒烟的断言哲学

### 1、cairo 的延迟报错陷阱

cairo 的核心设计是将错误**延迟**到下一次读取状态时才暴露：`cairo_move_to(NULL, 0, 0)` 不会立即崩，只是把 `cr->status` 设为 `CAIRO_STATUS_NULL_POINTER`，后续 `cairo_stroke()` 看到错误状态就什么都不画。所以：

> **"不崩"不算通过。** 空实现 / stub / 错误参数都能"不崩"。

脚本里 §9 每轮结束后显式检查 `dc:get_status() == "SUCCESS"`（`:544`），这是防御延迟错误的第一道防线。

### 2、真实往返：唯一的真实性保证

能区分"真 cairo"和"空实现"的办法只有一个：**让 cairo 记住状态，再读出来比较。**

| 如果 shim 是…… | `cr:move_to(3,4)` | `cr:get_current_point()` |
|-----------------|-------------------|--------------------------|
| 真 cairo | `cairo_move_to(cr, 3, 4)` | `cairo_get_current_point(cr, &x, &y)` → `(3.0, 4.0)` |
| stub 空函数 | 什么都不做 | 返回 `(0, 0)` 或垃圾值 |
| 伪造（Lua 侧记状态） | 在 Lua table 里写 `self._x=3` | 读 `self._x` 返回 3 | 看起来也是对的——**但 `scale()` 和 `clip()` 的矩阵/区域就没有账面了** |

第三种是最危险的：看起来能跑，一碰到 `wibox/drawable.lua` 的真实 clip + scale 组合就静默画错。

### 3、断言矩阵（28 个 section 全覆盖）

> **⚠ 2026-09-24 校定：** 下表 §1-§20 是 Part 1-3 时期定型的清单；§21-§28 是 Part 4-9 追加段（详见 §七 规模演进 与 `record2.md` 顶部对照表）。

以下按脚本 section 列出**每一段断言什么、为什么、怎么验证**。

#### §module：加载器与模块完整性

| 断言 | 检查点（`:125-133`） | 验证什么 |
|------|---------------------|----------|
| `lgishim.lua` 选了 shim | `lgi.shim == true` | `pcall(require, "lgishim_pango")` 成功 → 探测回退机制正确触发 |
| `lgi.Cairo == lgi.cairo` | 大小写别名 | lgi 风格的小写别名 |
| `Pango.SCALE == 1024` | 回归守卫 | Pango shim 没被 cairo shim 污染 |

#### §1：枚举常量是字符串、不是整数

| 断言 | 示例 | 验证什么 |
|------|------|----------|
| 类型为 string | `type(Op.SOURCE) == "string"` | `install_enum` 走 `lua_pushstring`，不是 `lua_pushinteger` |
| 值等于键名 | `Op.OVER == "OVER"` | `enum_from` 的映射表正确 |
| 九类枚举抽样 | Operator / Status / Extend / Format / Content / FillRule / PatternType / Filter | 枚举的整体一致性 |

**为什么必须是字符串：** `lib/wibox/drawable.lua:166` 写 `assert(cr.status == "SUCCESS", ...)`——如果枚举是整数，这个 `assert` 永远不触发（整数 `0` ≠ 字符串 `"SUCCESS"`），但 status 错误也被静默吞掉。

#### §2：14 张枚举表全部存在且非空

遍历 14 张枚举表（Operator / Format / Content / Status / Antialias / LineCap / LineJoin / FillRule / Path / Extend / FontSlant / FontWeight / PatternType / Filter），逐表检查 `type(t) == "table"` 且成员数 `> 0`，累计 `>= 80` 个成员。

#### §3：真实 cairo 枚举名存在（Part 1 bug-fix 面）

| 类 | 验证名 | 反证 |
|----|--------|------|
| Operator | `SATURATE` / `HARD_LIGHT` / `SOFT_LIGHT` / `DEST_IN` / `DEST_OUT` / `OVERLAY` | Agent 曾编造 `SUBTRACT` / `HARDLIGHT` / `SOFTLIGHT`（§4 验证它们已死） |
| Content | `ALPHA` | Agent 曾编造 `NONE`（Content 无此成员） |
| Extend | `NONE` / `PAD` / `REFLECT` | — |
| FontWeight | `BOLD` / `NORMAL` | cairo 1.16 只有这两项 |
| PatternType | `LINEAR` / `RADIAL` | — |
| Filter | `FAST` / `GAUSSIAN` / `BEST` | — |

#### §4：伪造名确实已死

6 个 Agent 伪造的枚举名逐一检查返回 `nil`：`Operator.SUBTRACT` / `Operator.HARDLIGHT` / `Operator.SOFTLIGHT` / `Operator.OUT_OVER` / `Operator.IN_OVER` / `Content.NONE`。

#### §5：Matrix 构造 + 读写 + 运算

| 操作 | 断言 | 验证什么 |
|------|------|----------|
| `Matrix.new()` | `xx==1, yy==1, xy==0, yx==0, x0==0, y0==0` | 默认单位阵 |
| `m:init(2,0,0,3,10,20)` | `xx==2, yy==3, x0==10, y0==20` | 显式初始化 |
| `s:multiply(ident)` | 结果各字段不变（单位阵 operand） | multiply 返回的是用户数据且字段可读 |
| `scale(2):invert()` | `xx == 0.5` | 逆矩阵计算真实发生 |
| `translate(5,5):invert()` | `x0 == -5, y0 == -5` | 平移逆矩阵对称 |

#### §6：Surface / ImageSurface / RecordingSurface 构造

| 操作 | 断言 | 备注 |
|------|------|------|
| `ImageSurface.new(ARGB32, 100, 50)` | `width==100, height==50, content==COLOR_ALPHA` | 基本构造 |
| `Surface.new(img)` | 返回同一个 userdata（`==` 比较） | `gears/surface.lua:72` 的包装路径 |
| `Surface.new(nil)` | 返回 `nil` | 空值保护 |
| `Surface.create_similar(img, COLOR_ALPHA, 40, 30)` | `width==40, height==30` | 派生 surface |
| `RecordingSurface.new(COLOR_ALPHA, {0,0,10,10})` | `content==COLOR_ALPHA` | 记录 surface |
| `ImageSurface.create_for_data(pixels, ARGB32, 10, 10, 40)` | `width==10, height==10`，可 flush | user-data hook 持有 Lua string 副本 |
| `img:flush()` / `img:show_page()` | 不掉 | 协议方法可调用 |

#### §7：Pattern 构造 + getter/setter 往返

| 操作 | 断言 | 验证什么 |
|------|------|----------|
| `Pattern.CreateRgba(0.5, 0.25, 0.125, 1)` | `get_rgba()` → `(0.5, 0.25, 0.125, 1)` | RGBA 通道精确往返 |
| `solid.type` | `== PatternType.SOLID` | 类型属性 |
| `set_filter(BILINEAR)` | `solid.filter == "BILINEAR"` | filter 往返 |
| `set_extend(REPEAT)` | `solid.extend == "REPEAT"` | extend 往返 |
| `CreateLinear(0,0,100,0)` | `type == LINEAR` | 线性渐变 |
| `add_color_stop_rgba(0, 1,0,0,1)` | `get_color_stop_rgba(0)` → offset=0, r=1, a=1 | 颜色停止点往返 |
| `CreateRadial(...)` | `type == RADIAL` | 径向渐变 |
| `CreateForSurface(img)` | `type == SURFACE` | 面片 pattern |
| `set_matrix(pm)` | `pattern.matrix.x0 == 7, .y0 == 9` | pattern 级矩阵往返 |

#### §8：Context 状态机往返

| 操作 | 断言 | 验证什么 |
|------|------|----------|
| `Context.new(img)` | userdata, `get_status() == "SUCCESS"` | 基本构造 |
| `set_operator(MULTIPLY)` | `cr.operator == "MULTIPLY"` | **operator 往返——这是"真 cairo vs stub"的标杆断言** |
| `scale(2, 3)` | `cr.matrix.xx == 2, .yy == 3` | matrix 真实算术 |
| `translate(10, 20)` | `matrix.x0 == 10, .y0 == 20` | 平移叠加 |
| `save() / translate(100,100) / restore()` | `matrix.x0 == 0`（恢复成功） | save/restore 真实栈 |
| `move_to(3, 4)` | `get_current_point() == (3, 4)` | **坐标往返** |
| `rel_move_to(1, 2)` | `current_point == 4`（x 方向累计） | 相对移动 |
| `rectangle(10,20,30,40) + clip()` | `clip_extents()` 返回 `x0==10, y0==20, x1==40, y1==50` | **clip 往返** |
| restore 后 | `clip_extents()` 回到全 surface 范围 | clip 被 restore 清除 |
| `set_source_rgb` / `set_source_rgba` | `get_source()` 返回 Pattern userdata | source 链 |
| `set_line_width(5)` | `cr.line_width == 5` | 线宽往返 |
| `set_hairline()` | `cr.hairline == true` | 细线模式 |
| `set_line_width(5)` 后 | `cr.hairline == false`（hairline 自动失效） | 互斥状态 |
| `set_dash({2,4,6}, 3, 1.5)` | `get_dash()` → 表 `{2,4,6}`, count=3, offset=1.5 | dash 模式往返 |
| `set_antialias(NONE)` | `cr.antialias == "NONE"` | 抗锯齿往返 |
| `set_fill_rule(EVEN_ODD)` | `cr.fill_rule == "EVEN_ODD"` | 填充规则往返 |
| `set_line_cap(ROUND)` | `cr.line_cap == "ROUND"` | 线帽往返 |
| `set_line_join(ROUND)` | `cr.line_join == "ROUND"` | 线连接往返 |
| `set_tolerance(1.5)` | `cr.tolerance == 1.5` | 容差往返 |

#### §9：Context 绘图——延迟错误检查

19 个绘制方法逐一调用（move_to / line_to / stroke / curve_to / arc / arc_negative / rectangle / fill / fill_rectangle / stroke_rectangle / circle / close_path / path_close / fill_preserve / stroke_preserve / paint / paint_with_alpha / mask / mask_surface / push_group / pop_group_to_source / push_group_with_content），**终点断言 `dc:get_status() == "SUCCESS"`**（`:544`）。

这是防御 cairo 延迟错误的核心机制——如果前面任何一笔产生了 `CAIRO_STATUS_INVALID_MATRIX` 或 `CAIRO_STATUS_NULL_POINTER`，最终 status 就不是 `SUCCESS`。

#### §10：Context toy font API

| 操作 | 断言 | 验证什么 |
|------|------|----------|
| `select_font_face("sans", NORMAL, BOLD)` | 不掉 | 字体选择 |
| `set_font_size(24)` | `font_size` 是 function（只有 setter，无 getter） | cairo 1.16 无 `cairo_get_font_size` |
| `text_extents("hello")` | **返回 table，`width > 0`, `height > 0`, `x_advance > 0`** | **text 度量往返——stub 会返回零或 nil** |
| `font_extents()` | `height > 0`, `ascent > 0`, `max_x_advance > 0` | 字体度量 |
| `glyph_extents(0,0,104)` | `width > 0` | 字形度量 |
| `show_text("hello")` | 不掉，status == SUCCESS | 文本绘制 |

#### §11：Rectangle 构造 + `Format.stride_for_width`

| 操作 | 断言 | 验证什么 |
|------|------|----------|
| `cairo.Rectangle{...}` | 返回同一个 table，字段不变 | 构造函数恒等（lgi 语义） |
| `Format.stride_for_width(ARGB32, 100)` | `== 400` | stride 计算（4 字节 × 100） |
| `Format.stride_for_width(RGB24, 100)` | `== 400` | RGB24 也是 4 字节/像素（cairo 对齐） |
| `Format.stride_for_width(A1, 100)` | `== 16` | 位图 stride |

#### §12：`is_type_of` 在真实对象上

| 被检对象 | `:is_type_of()` | 期望 |
|----------|:---:|:----:|
| nil | Context | false |
| 42 | Context | false |
| {} | Context | false |
| 真实 Context | Context | **true** |
| ImageSurface | Context | false |
| Pattern | Context | false |
| ImageSurface | Surface | **true** |
| RecordingSurface | Surface | **true**（派生类） |
| create_similar 结果 | Surface | **true** |
| solid pattern | Pattern | **true** |
| linear gradient | Pattern | **true** |
| radial gradient | Pattern | **true** |
| surface pattern | Pattern | **true** |
| get_source 结果 | Pattern | **true** |
| ImageSurface | Pattern | false |
| Context | Pattern | false |
| Matrix userdata | Matrix | **true** |
| Context | Matrix | false |

#### §13：跨模块 cls 碰撞防御

cairo 和 pango 的 `cls` 编号**四档全撞**：

| cls | cairo | pango |
|----:|-------|-------|
| 1 | Context | AttrList |
| 2 | Surface | Layout |
| 3 | Pattern | FontDesc |
| 4 | FontOptions | Color |

断言每对碰撞都返回 false：`cairo.FontOptions:is_type_of(Pango.Color()) == false` 等 4 组。这是 Part 1 最隐蔽的 bug——Agent 原实现用 `w->cls + strcmp` 判定，跨模块碰撞会静默返回 true（`record2.md` 篇二 §2 的 bug #4）。

#### §14：GC 安全

200 个周期内每轮新建 Surface + Context + Pattern + Matrix，然后强制 `collectgarbage("collect")`。不掉 = userdata 的 `__gc` 元方法无 double-free / use-after-free / 空指针。

#### §15：Pango 桥接

`cr:update_layout(layout)` 后：
- context status == SUCCESS
- `layout:get_pixel_extents()` 的 ink width/height > 0（字体状态真实写入 layout）

`cr:show_layout(layout)` 后：
- context status == SUCCESS
- 渲染后 layout 的 logical extents 非零

**注意：** Pango Context 必须通过 `PangoCairo.font_map_get_default():create_context()` 创建（`:1057`），不能用 `Pango.Context.new()`——容器 Pango 1.48 上后者得不到 font backend，会死循环吐 glib critical（`record2.md` 篇四 §1）。

#### §16：Surface 扩展方法

| 方法 | 断言 |
|------|------|
| `surface:get_width()` | `== 100` |
| `surface:get_height()` | `== 50` |
| `surface:get_content()` | `== COLOR_ALPHA` |
| `surface:get_data()` | 返回非 nil |
| `surface:get_type()` | 返回 string |
| `surface:get_description()` | 返回 string |
| `surface:mark_dirty()` | 不掉 |
| `surface:mark_dirty_rectangle(0,0,10,10)` | 不掉 |
| `surface:get_device_offset()` | `(0, 0)` |
| `surface:get_font_options()` | 返回 FontOptions userdata |
| `cr:push_group_to_image()` | 返回 Surface userdata（不是 Pattern） |

#### §17：Pattern 扩展方法

| 方法 | 断言 |
|------|------|
| `pattern:get_type()` | `== SURFACE`（对 surface pattern） |
| `pattern:get_target()` | 返回 userdata |
| `pattern:get_target_format()` | 返回 string |
| `pattern:get_opaque()` | `== true`（对 alpha=1 的 solid） |
| `gradient:get_color_stop_count()` | `(status, count) == ("SUCCESS", 3)` |
| `gradient:get_color_stop_rgba(0)` | `("SUCCESS", 0.0, 1.0, 0.0, 0.0, 1.0)` |
| stopless gradient `get_color_stop_count()` | `("SUCCESS", 0)` |
| `clone_stops()` 回归项 | 逐字复刻 `background.lua:33-38`，复制后 3 stops 全保留 |

status 优先返回形状（`(status, count)` 而不是 `(count)`）是对齐 lgi 的关键——`gears/color.lua:325,334` 和 `wibox/container/background.lua:33,36` 的 destructuring 第一变量就是 status（`record2.md` 篇四 §3.4）。

#### §18：Matrix 静态构造器

| 操作 | 断言 |
|------|------|
| `Matrix.identity()` | `xx==1, yy==1, xy==0, x0==0` |
| `Matrix.translate(5, 10)` | `xx==1, yy==1, x0==5, y0==10` |
| `Matrix.scale(3, 4)` | `xx==3, yy==4, x0==0, y0==0` |

三种都返回新 userdata（静态构造，非原地修改）。

#### §19：Region 新类

| 操作 | 断言 |
|------|------|
| `Region.new()` | 返回 userdata |
| `contains_point(5, 5)` | 返回 boolean |

#### §20：真实 awesome 调用路径（详见第四章）

---

#### §21：Context/Pattern 属性 getter（Part 4）

`--index` 元方法读入真实 cairo 状态。断言：`cr.font_size`、`cr.line_width`、`cr.line_cap`、`cr.line_join`、`cr.hairline`、`cr.matrix`、`cr.source`、`cr.operator`、`cr.operand`、`cr.ink_extents`、`cr.paint_extents`、`pattern.extend`、`pattern.opacity`、`pattern.filter`、`pattern.matrix` 全走真实 getter。

#### §22：属性 setter `__newindex`（Part 4）

反向——`cr.line_width = 7`、`pattern.opacity = 0.5` 等 setter 真的调了 `cairo_*_set_*`。断言：写后 `--index` 读回同一值。

#### §23：`lib/` 精确属性访问形状回归守卫（Part 4）

逐字复刻 `lib/gears/color.lua`、`lib/wibox/drawable.lua`、`lib/wibox/widget/textbox.lua` 的属性访问组合（如 `local _, _, _, a = pattern:get_rgba()` 的 4 值解构），防止 shim 签名改坏 lib/。

#### §24：Pango Layout 测量（Part 7）

`Layout:get_extents` / `get_size` / `get_baseline` / `get_line_count` 四个 API 交付。断言：`text = "hello cairo"` 渲染后 `logical.width > 0`、`ink.height > 0`、`baseline >= 0`、`line_count == 1`。

#### §25：GLib Part 5（main loop、路径、Variant、KeyFile）

GLib shim 首次交付时的段：`GLib.MainLoop` 构造 + `new_default` + `new_context` + `quit`；`GLib.get_tmp_dir()` / `GLib.path_get_dirname()` / `GLib.filename_from_uri()` 等路径函数；`GLib.Variant.new()` / `GLib.Variant.parse()` 与 KeyFile 四函数（`new`、`load_from_file`、`get_string`、`save_to_file`）。

#### §26：GdkPixbuf Part 8

`GdkPixbuf.Pixbuf.new_from_file(path)` + `new_from_bytes` + `new_from_native` + `Pixbuf:scale_simple` + `Pixbuf:get_pixels` + `Pixbuf:get_width/height/pixel_width/n_channels`。

#### §27：GObject Part 9（Closure）

`GObject.Closure.new(callback)` + `Closure:connect_object` + `Closure:connect_object_swap` + `Closure:connect_object_full` 及 `Closure:emit` 触发的往返。

#### §28：Gio Part 6（File、streams、D-Bus）

Gio shim 首次交付时的段：`Gio.File.new_for_path()` + `Gio.File.new_for_uri()` + `Gio.File.new_tmp()` + `Gio.File:get_uri()`；`Gio.FileInputStream.new_sync` / `Gio.FileOutputStream.new_replace`；D-Bus surface 的 `Gio.bus_get_sync(Gio.BusType.SESSION)`（会话不可用时允许报 null，但接口形状必须存在）。

#### §20：真实 awesome 调用路径（详见第四章）

> **⚠ 2026-09-24 校定：** §20 的 5 条路径行号已更新到当前脚本位置（详见 §四）。

## 四、§20「真实 awesome 调用路径逐字复刻」法

### 1、方法

不复现合成的 `move_to + line_to + stroke` 组合调用。直接从 `lib/` 源码逐行搬运真实代码路径，变量名和调用序列与源文件完全一致。**§20 的 5 段代码块从 5 个 lib/ 文件逐字移植，只省略了无关分支**。

### 2、5 条路径

| 路径 | 来源 | 行号 | 验证什么 |
|------|------|------|----------|
| 脏区域裁剪 | `lib/wibox/drawable.lua:110-119` | 脚本 `:1035-1050` | `Region.create()` + `union_rectangle()` + `get_rectangle(i)` 返回普通 table（`.x/.y/.width/.height` 索引） |
| fit_in_bounding | `lib/awful/placement.lua:829-842` | `:1052-1073` | `Region.create_rectangle()` + `intersect()` + `get_rectangle(0)` 的宽高在裁剪前后正确 |
| create_similar | `lib/gears/surface.lua:167` | `:1075-1084` | `surface:create_similar(content, w, h)` + `.content` 往返 |
| push/pop_group | `lib/wibox/container/background.lua:313,335` | `:1086-1105` | `push_group()` → draw → `pop_group()` 返回 Pattern（不是 Context），可作 mask 用 |

> **⚠ 2026-09-24 校定：** 上表「脚本」列的行号已从旧快照（`:981-1048`）更新到当前 `10-spike4-cairo-smoke.sh` 位置。§20 段本身在 Part 1-3 定型后没有新增路径，只是被后续段（§21-§28）挤到更下方。
| textbox draw() | `lib/wibox/widget/textbox.lua:40-54,513` | `:1055-1072` | 从 `PangoCairo.font_map_get_default():create_context()` 获取 context → `update_layout` → `show_layout` → `get_pixel_extents` 全链路 |

### 3、价值

- **不是"覆盖更多 API"**——这 5 段用的 API §1–§19 已经逐个测过。
- **而是覆盖"API 之间的合同"**——比如 `Region:get_rectangle()` 返回的是普通 table 还是 userdata？`drawable.lua` 直接 `.x/.y/.width/.height` 索引，如果不是 table 就炸。又比如 `pop_group()` 返回的是 Pattern 还是 Context？`background.lua` 直接拿它调用 `cr:set_source()`——如果是 Context，静默画错。
- **可复现回归**：上游 `lib/` 文件不改动，这些路径就是 shim 的合同。shim 改了返回形状，§20 第一个炸。

### 4、适用边界

§20 是验收测试，不是规范测试。它依赖于 `lib/` 的当前版本——如果上游重构了 `drawable.lua` 的 clip 路径，§20 的逐字复刻需要同步更新。**§1–§19 的原子 API 测试是稳定的**——无论 `lib/` 怎么改，`set_operator() → cr.operator` 的合同不变。§20 是额外的一层安全网，不是替代品。

---

## 五、测试自身的断言纠错（5 条）

冒烟脚本编写过程中发现 5 处**测试断言错误**（不是 shim bug），每一条都是对 cairo/lgi 语义的认知修正。

| # | 问题 | 说明 | 修正 | 来源 |
|---|------|------|------|------|
| 1 | `clip_extents` 被 surface 边界钳制 | 100×50 surface，clip 矩形 `y` 到 60 → 实际 `y1=50`（被表面高度钳制），不是 60。cairo 内部将 clip 与 surface 区域做交集 | `approx("clip_extents y1 clamped to the surface height", y1, 50)`（`:416`） | `record2.md` 篇三 §5 |
| 2 | `.dashes` 属性多返回值只留第一个 | `get_dash()` 返回三元组 `(table, count, offset)`，但 `__index` 元方法对多返回值只取第一个 → `cr.dashes` 拿到的是 setter function，不是 dashes 表 | 改用 `cr:get_dash()` 显式取三元组 | `record2.md` 篇三 §5 |
| 3 | `font_size` 无 getter | 本容器 cairo 1.16.0 没有 `cairo_get_font_size()`，`__index` 先查 getter 表（空）、再查 setter 表 → 返回 function（setter 本身）。这**正是 lgi 的行为** | 断言 `type(fc.font_size) == "function"`（`:560-562`） | `record2.md` 篇三 §5 + 所有权探针表 |
| 4 | Matrix `multiply` 是 cairo 约定 | `a:multiply(b)` 执行 `cairo_matrix_multiply(&result, &a, &b)`——即 `result = a × b`。cairo 的矩阵乘法是行列式约定（`xx' = a.xx*b.xx + a.xy*b.yx`），**不是**逐元素乘 | 测试用单位阵 operand：`s:multiply(ident)` → 结果各字段 = `s` 的字段。不测试非单位阵的乘积语义 | `record2.md` 篇三 §5 |
| 5 | `scale(2)` 的 invert | `Matrix:scale(2, 2):invert()` → `xx == 0.5`（逆缩放，不是 2） | 断言 `approx("invert of scale(2) xx", inv.xx, 0.5)`（`:236`） | `record2.md` 篇三 §5 |

外加一条**环境事实**（不成其为"纠错"但影响断言设计）：

| # | 事实 | 说明 | 影响 |
|---|------|------|------|
| 6 | `pop_group` 的 `type` 是 SURFACE | 本容器 cairo 的 `cairo_pattern_type_t` 枚举止于 `RASTER_SOURCE`，没有 `CAIRO_PATTERN_TYPE_PATTERN` 成员。`pop_group()` 返回的 group pattern 的 `get_type()` 报告 `SURFACE` | 断言改为"能映射到真实 enum 名、不落到 UNKNOWN"（`:1043-1045`），不断言具体值 | `record2.md` 篇四 §4 |

---

## 六、实操要点

> **⚠ 2026-09-24 校定：** 本节小节号后括号里的 `:NNN` 是**旧脚本快照**的行号（Part 1-3 时期，脚本 1120 行）。当前脚本已扩到 2283 行，行号整体下移。方法学完全没变，用 `grep -n "<snippet>" docs/sam/scripts/10-spike4-cairo-smoke.sh` 定位当前行号即可。

### 1、`io.stdout:setvbuf("no")`（旧 `:89`，现 `:135`）

stdout 在管道里默认全缓冲。**脚本崩了最后几行输出会被缓冲吞掉**，看不到最后一条 `ok   xxx` 是哪项、从而无法定位崩溃点。无缓冲让每条 `print` 即时刷出。

### 2、`safe()` 包装器（旧 `:117-121`，现 `:163-167`）

```lua
local function safe(name, fn)
    local ok, err = pcall(fn)
    check(ok, name, err)
    return ok
end
```

每个 Part 2+ 方法调用都经过 `safe()`。这样签名不匹配（如传参数量错、类型错）报的是 `FAIL <method_name> -- <error_message>`，而不是未捕获异常让脚本直接退出（连前面测了多少项都不知道）。

### 3、`LUA_PATH`（旧 `:1116`，现脚本尾部）

```bash
LUA_PATH="$REPO/lib/?.lua;$REPO/lib/?/init.lua"
```

静态宿主不继承系统的 LUA_PATH。不给这个，`require("vendor.lgishim")` 直接失败。

### 4、probe-compile 比从 `build.ninja` awk 提参可靠

容器 `sh` 的 awk 对单引号正则转义会炸（`"Unmatched ( or \("`）。`09-spike4-cairo-probe-compile.sh` 直接把 build.ninja 里的 FLAGS/INCLUDES 硬编码成 shell 变量（`:25-26`），一行搞定编译验证（`record2.md` 篇二 §5.3）。

### 5、链接后 `nm` 验证符号真实链入（旧 `:1109-1112`，现脚本尾部）

```bash
nm "$T/smoke" | grep -c " cairo_set_operator"
nm "$T/smoke" | grep -c " cairo_pattern_create_rgba"
nm "$T/smoke" | grep -c " cairo_create"
nm "$T/smoke" | grep -c " cairo_image_surface_create_for_data"
```

四个关键 cairo 符号的 `nm` 计数——**零计数 = shim 调了 stub / 静态链接没把 `.a` 链进去**。这是在"跑冒烟"之前的廉价检查：符号都没链进二进制，冒烟全绿才叫见鬼。

### 6、`LD_LIBRARY_PATH=` 清空（旧 `:1116`，现脚本尾部）

```bash
LD_LIBRARY_PATH= "$T/smoke" "$T/part12_smoke.lua"
```

防止宿主环境的 `LD_LIBRARY_PATH` 意外让 `smoke` 捡到动态 `.so`，破坏"全静态"的验证前提。

---

## 七、规模演进

> **⚠ 2026-09-24 校定：** 下表 Part 4-9 行是**新增**，Part 1-3 三行是原快照保留。

| 阶段 | 脚本名 | Section 数 | 检查数 | 时期 | 新增内容 |
|------|--------|:--------:|:------:|------|------|
| Part 1 | `08-spike4-cairo-part1-smoke.sh` | 7 + 6b | **108** | 02:00 | 枚举 + 元方法 + 类表 + stub 构造器 + 跨模块碰撞防御 |
| Part 1+2 | `10-spike4-cairo-smoke.sh`（git mv） | 14 | **306** | 04:18 | Matrix / Surface / Pattern 构造 + Context state 往返 + drawing 延迟错误检查 + font API + Rectangle + is_type_of 真实对象 + GC 安全 |
| Part 1+2+2c+3 | `10-spike4-cairo-smoke.sh` | **20** | **424** | 06:00 | Pango 桥接 + Surface 15 方法 + Pattern 12 + Matrix 3 静态构造器 + Region 新类 + §20 真实路径 |
| Part 4（property 层） | 同上 | **23** | **~581** | 08:30 | §21 Context/Pattern getter + §22 setter + §23 lib/ 精确访问形状回归守卫 |
| Part 5（GLib） | 同上 | 24 | — | 11:00 | GLib main loop / 路径 / Variant / KeyFile（并入 §25） |
| Part 7（Pango Layout） | 同上 | 25 | — | 12:00 | `Layout:get_extents` / `get_size` / `get_baseline` / `get_line_count`（§24） |
| Part 8（GdkPixbuf） | 同上 | 26 | — | 13:00 | `Pixbuf.new_from_file` / `_bytes` / `_native` / `scale_simple`（§26） |
| Part 9（GObject） | 同上 | 27 | — | 13:30 | `Closure.new` / `connect_object` / `emit`（§27） |
| Part 6（Gio）+ 接线护栏 | 同上 | **28** | **829** | 14:00 | §28 Gio File/streams/D-Bus；脚本头部 `luaa.c` ↔ `lib/vendor/lgishim.lua` 双向接线校验（wiring guard，防 `{}` 静默替换） |

每次扩展都是**叠加**——Part 2 没删 Part 1 的检查（stub 构造器替换为真实实现后，原先的 nil 检查自然消失），Part 2c+3 在 Part 1+2 的 14 个 section 后追加 6 个新 section；Part 4-9 在同一模式下继续追加。最终 **829 项检查 = 前 424 项回归守卫 + 405 项新覆盖率**。Part 6 追加的两道**接线护栏**（脚本 `:43-74`、`:176-185`）不属于新增 section，但**是 Part 6 Gio 空表事故的直接后果**，见 `record2.md` 篇八。

---

## 八、冒烟确认的 shim 事实

以下是**被冒烟验证过，不是"看起来能跑"**的结论：

| 事实 | 验证手段 | 如果假的会怎样 |
|------|----------|----------------|
| `set_operator("MULTIPLY")` 真的调了 `cairo_set_operator` | §8 `cr.operator` 读取到 `"MULTIPLY"` | serializer 返回错误字符串，或 stub 返回默认值 |
| `scale(2,3)` 真的调了 `cairo_scale` | §8 `cr.matrix.xx == 2` | 读回 1.0（默认单位阵） |
| `move_to(3,4)` 真的调了 `cairo_move_to` | §8 `get_current_point() == (3,4)` | 读回 `(0,0)` |
| `clip()` 真的调了 `cairo_clip` | §8 `clip_extents()` 返回被裁剪的矩形 | 返回全 surface 范围 |
| `text_extents("hello")` 真的调了 `cairo_text_extents` | §10 `ext.width > 0` | 返回零宽度的全零结构 |
| `show_layout(layout)` 真的通过 `pango_cairo_show_layout` 画了文本 | §20 `logical.width > 0, ink.width > 0` | extents 全零 |
| `get_color_stop_count()` 返回 `(status, count)` 形状 | §17 status-first destructuring | `gears/color.lua:325` 的 `local err, count = ...` 拿到 nil err |
| `Region:get_rectangle(i)` 返回 table | §20 `.x/.y/.width/.height` 索引成功 | drawable.lua 裁剪路径 null-deref |
| `pop_group()` 返回 Pattern | §20 `Pattern:is_type_of(mask) == true` | background.lua 把 Context 当 mask 用 |
| GC 200 轮不掉 | §14 | double-free / use-after-free / refcount 多减触发 cairo 断言 abort |

---

## 九、产出文件

| 文件 | 行数 | 作用 |
|------|-----:|------|
| `lib/vendor/lgishim_cairo.c` | **2379** | Cairo shim 全量（Part 1+2+2c+3+4） |
| `docs/sam/scripts/09-spike4-cairo-probe-compile.sh` | 33 | 隔离编译验证 |
| `docs/sam/scripts/10-spike4-cairo-smoke.sh` | **2283** | 本冒烟框架（28 section / 829 检查） |
| `lib/vendor/lgishim_pango.c` | **996** | Pango shim（`check_layout` 导出 + Context 修复 + Part 7 `Layout` 四 API） |
| `lib/vendor/lgishim_glib.c` | ~990 | GLib shim（Part 5 交付） |
| `lib/vendor/lgishim_pixbuf.c` | ~150 | GdkPixbuf shim（Part 8 交付） |
| `lib/vendor/lgishim_gobject.c` | ~130 | GObject shim（Part 9 交付） |
| `lib/vendor/lgishim_gio.c` | ~2900 | Gio shim（Part 6 交付，最大单体） |

**与 Spike 2 冒烟的关系：** Spike 2 的 `07-spike2-shim-smoke.sh`（230 行，42 项）是第一个冒烟，开创了「最小 Lua 宿主 + `package.preload` 注册 + 静态链接 + `nm` 验证」模式。Spike 4 的 10 脚本（**2283 行，829 项**）把这个模式的检查深度从"API 可调用"升级到"真实往返验证"——42 项 vs 829 项，**约 20 倍覆盖率密度**，并追加了两道接线护栏防 `{}` 静默替换。

---

## 十、本次更新说明（2026-09-24 校定）

1. **头部**：状态 `424/424` → **`829/829`**；脚本 `1120 行` → **`2283 行`**；日期区间延伸至 `14:00`；结论加 GLib/GdkPixbuf/Gio/GObject 覆盖说明。
2. **§三 断言矩阵**：标题 `20 个 section` → **`28 个 section`**，并就地补入 §21-§28 六段（Part 4 属性层、Part 5 GLib、Part 7 Pango Layout、Part 8 GdkPixbuf、Part 9 GObject、Part 6 Gio）。
3. **§四 §20 5 条路径**：脚本列行号从旧快照 `:981-1048` → **`:1035-1105`**（当前脚本实际位置）。
4. **§六 实操要点**：加一段整体说明——六小节的 `:NNN` 行号是**旧脚本快照**，方法学没变；每小节标题里补上当前行号。
5. **§七 规模演进**：新增 Part 4-9 六行；Part 6 尾部说明两道**接线护栏**（wiring guard）不是新 section 但**是 Part 6 Gio 空表事故的直接后果**。
6. **§九 产出文件**：`lgishim_cairo.c` `~2372` → **`2379`**、脚本 `1120` → **`2283`**、`lgishim_pango.c` `~950` → **`996`**；新增 GLib / GdkPixbuf / GObject / Gio 四 shim 行。
7. **底部「与 Spike 2 冒烟的关系」**：`1120 行 / 424 项` → **`2283 行 / 829 项`**，覆盖倍数 `10x` → **约 `20x`**；追加接线护栏说明。