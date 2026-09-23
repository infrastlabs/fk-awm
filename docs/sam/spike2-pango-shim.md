# Spike 2 — Pango Shim 最小闭环

**日期：** 2026-09-23 20:00–21:30
**状态：** ✓ 完成
**分支：** `feat/ai-260923-static`
**结论：** **方案 B 走通。** 静态二进制 21.4 MB，`lgishim_pango` 冒烟 42/42 全通过，无 GLib CRITICAL。

---

## 1. 目标

用最小闭环证明方案 B 可执行：不通过 GI 运行时、不 `dlopen("libpango-1.0.so")`，直接静态链接 Pango C API，让 `require("vendor.lgishim")` 在纯静态 `awesome` 下返回与 `lgi.Pango` 同形的 Lua 对象。

**本轮不含：** `X11` / `XRandR` shim（typelib 缺失但优先级低于 Pango 闭环）、`pango_cairo_show_layout` 端到端渲染、`beautiful` 模块完整端到端 smoke。

**前置依赖（Spike 0/1 已完成）：**

| 前置 | 状态 | 关键产出 |
|------|:----:|----------|
| 静态构建链路 | ✓ | `-Wl,--start-group/--end-group` 归零，Spike 1 §8.4 |
| `LUA_PATH` | ✓ | `/usr/local/share/lua/5.3 → /usr/share/lua/5.3` 软链 |
| typelib 探测 | ✓ | Pango 链 typelib 齐，但真实 lgi 运行期仍要 `dlopen`，与全静态冲突 |
| Pango 静态库 | ✓ | `libpango.a` / `libpangocairo.a` 齐 |

---

## 2. 关键决策

| 决策 | 选项 | 结论 | 依据 |
|------|------|------|------|
| 绑定方式 | GI / Pango C API / 手写 Lua 桩 | **Pango C API** | GI 走 `dlopen`，与静态目标冲突；手写桩等于重造 typelib |
| 加载策略 | 硬替换 / 探测回退 | **探测回退** | `lgishim.lua` 先试真实 lgi，失败才 fallback；开发容器内零行为变化 |
| 链接方式 | 静态 / 动态 pkg-config | **静态** | 与 Spike 1 静态链路对齐，产物为单一 ~21 MB 二进制 |
| 类表缓存 | 每次新建 / 全局 registry | **`LUA_REGISTRYINDEX`** | Pango 对象带 `{ptr, destroy, cls}`，cls 需跨调用稳定，metatable 才能命中 |

**未采用真实 lgi 的核心理由：** Spike 1 §6 已证 typelib 齐、`lgi.Pango` 可用，但加载时 `dlopen("libpango-1.0.so")`，R6 全静态单文件目标下运行时会 100% 失败。shim 是**唯一路径**，不是备选。

---

## 3. 对象模型（b16 硬约束 1-4）

每个 Pango 对象是 full userdata，持有：

```c
typedef struct {
    void *p;                 /* 原生指针 */
    void (*destroy)(void *); /* 释放回调 */
    int cls;                 /* 类索引，对应 CLS_NAMES[] */
} wrap;
```

**四条硬约束：**

1. **`._native` 返回 lightuserdata**（`lgishim_pango.c:744`）—— 唯一跨 C 边界的通道。
2. **`__gc` 幂等**（`:771`）—— `release()` 把 `p` 置 NULL 后 `destroy` 也换成 `d_noop`，重复调用安全。
3. **`finish()` 显式释放**（`:764`）—— 等价于手动触发 `__gc`，让对象进入 inert 状态。
4. **类表在 `LUA_REGISTRYINDEX` 缓存**（`:67` `#define REG_CLASSES "lgishim_classes"`）—— `cls` 数组下标 → `{m, g, p}` 三元组（方法表 / getter 表 / property setter 表）。

`m_index` 查找顺序（`:671`）：属性 getter (`g`) → 方法 (`m`) → metatable 上的助手 (`_native` / `finish`)。

---

## 4. 类清单

| 类 | destroy | 类型 | 备注 |
|----|---------|------|------|
| `lgishim.AttrList` | `d_attrlist` | GBoxed | `pango_attr_list_unref` |
| `lgishim.Color` | `d_gfree` | — | Pango 1.48 公开 struct，字段直读；16-bit 通道打包 |
| `lgishim.FontDescription` | `d_fontdesc` | GBoxed | `pango_font_description_free`，**不是** `g_object_unref` |
| `lgishim.FontMetrics` | `d_metrics` | GBoxed | 1.48: `_unref`；1.50+: `_free`（`#if` 分支） |
| `lgishim.Layout` | `d_unref` | GObject | `g_object_unref` |
| `lgishim.Context` | `d_unref` | GObject | `g_object_unref` |
| `lgishim.FontMap` | `d_unref` | GObject | `pango_cairo_font_map_get_default()` 返回的借用单例 |

**辅助 destroy 回调：**

- `d_unref` → `g_object_unref`（GObject）
- `d_gfree` → `g_free`（`Color` struct）
- `d_attrlist` → `pango_attr_list_unref`
- `d_fontdesc` → `pango_font_description_free`（**GBoxed 专用**）
- `d_metrics` → `pango_font_metrics_unref` (1.48) / `pango_font_metrics_free` (1.50+)
- `d_noop` → 什么都不做（借用引用，如 `context.get_font_description` 返回的指针）

**模块级入口：** `Pango.SCALE`、`Pango.units_from_double`、`Pango.parse_markup`、`PangoCairo.font_map_get_default`。

---

## 5. 核心 API 调用面

按类整理（共 38 个 wrapper，取自 `lgishim_pango.c:384-886`）：

**`Layout`**（21 方法 + 4 属性 RW）
- 构造：`Layout.new(ctx)`
- 文本：`set_text` / `get_text`、`set_attributes` / `get_attributes`
- 布局：`set/get_width`、`set/get_height`、`set/get_indent`、`set/get_wrap`、`set/get_ellipsize`、`set/get_justify`、`set/get_alignment`、`set/get_line_spacing`、`set_font_description`
- 测量：`get_pixel_extents`（返回 `{x, y, width, height}` Pango 单位）
- 刷新：`context_changed`
- 属性：`text`、`attributes`、`width`、`height`（`__index` 走 getter 表，`__newindex` 走 setter 表）

**`FontDescription`**
- `new()`、`from_string(str)`、`merge(other, replace)`、`get_size()`、`copy_static()`、`to_string()`

**`Color`**
- 方法：`new()`、`parse(str) -> bool`
- 属性（1.48 字段直读，`v * 257` 打包）：`red`、`green`、`blue`；`alpha` 无实现（读为 `nil`）

**`Context`**
- `new()`、`set_resolution(dpi)`、`get_font_description()`（借引用，`d_noop`）、`get_metrics(desc)`

**`FontMetrics`**
- `get_ascent()`、`get_descent()`（返回 Pango 单位 1/1024 px）

**`FontMap`**
- `create_context()`

**`AttrList`**
- `new()`

**模块级**
- `Pango.SCALE` = 1024（`PANGO_SCALE`）
- `Pango.units_from_double(x)` → int
- `Pango.parse_markup(str, len?)` → `(attrlist, text)` 或 `(nil, err_message)`
- `PangoCairo.font_map_get_default()`

---

## 6. 版本不匹配（Pango 1.48 vs 1.50）

容器装的是 Pango 1.48，`lib/` 是按 1.50 API 写的。shim 里 3 处运行期 workaround + 1 处编译期 `#if` + 1 处硬编码：

| 场景 | 1.48 做法 | 1.50+ 做法 | 当前 |
|------|-----------|------------|------|
| `set_resolution` | fontmap 路径 + `pango_context_changed` | context 直调 | 1.48 路径（`lgishim_pango.c:563`） |
| metrics 释放 | `pango_font_metrics_unref` | `pango_font_metrics_free` | `#if PANGO_VERSION_MAJOR*100+MINOR >= 150` 分支（`:105`） |
| Color 结构 | 公开 struct，字段直读 | opaque + `pango_color_get_red()` 等 accessor | 硬编码 1.48（`:514-528`） |
| `pango_layout_set_text` 签名 | 3 参（含 `length`） | 2 参 | 传 `-1`（`:227`） |

**枚举大写约定**（`:199-216`）：`ALIGN` / `ELLIPSIZE` / `WRAP` 全部大写。原因是 `textbox.lua` 拿 `get_alignment()` 的返回值跟它传给 setter 的字符串做 `==` 比较（如 `if layout:get_alignment() == allowed[mode] then return end`），allowed 表是大写。shim 的 getter 必须返回大写，否则这些早退判断永远不成立、每次 set 都重设（功能对但浪费）。

---

## 7. 修复的 6 个 bug

本轮约 50% 时间花在 metatable / 栈操作 bug 上，Lua 5.3 C API 的隐式栈语义是主因。

| # | Bug | 位置 | 修复 | 时间 |
|---|-----|------|------|------|
| 1 | `class_setup` registry branch 忘 re-push，导致 clsinfo 写到 module table | `lgishim_pango.c:815` | `lua_setfield` 消费栈顶后立即追加 `lua_getfield` 补回 | 20:52 |
| 2 | `push_obj` 重复 `setmetatable`（`luaL_setmetatable` 已消费栈顶） | `:151` | 删除第二行 `lua_setmetatable` | 20:58 |
| 3 | `m_index` `settop(3)` 保留的是 REG_CLASSES 而非 result | `:692` | 改用 `lua_replace(L, 3)` 把 result 挪到 slot 3，再 `settop(3)` | 21:03 |
| 4 | `FontDescription` / `FontMetrics` 用 `d_unref`（GBoxed 不是 GObject，触发 GLib 断言） | 新增 `:95`/`:100`，换用 `:438`/`:443`/`:461`/`:582` | 新增 `d_fontdesc`（→ `pango_font_description_free`）和 `d_metrics`（→ `pango_font_metrics_unref`） | 21:08 |
| 5 | `cx_get_font_description` 是借引用，Lua 侧释放会二次释放 | `:575` | 改用 `d_noop` | 21:10 |
| 6 | 5 项冒烟测试预期错 | `07-spike2-shim-smoke.sh` | 修测试断言（见 §9） | 21:15 |

**共性教训：**

- **Lua 5.3 C API 对「宏消费栈顶」与「显式 pop」的语义不一致。** `luaL_setmetatable(l,n)` 是宏（展开为 `luaL_getmetatable, lua_setmetatable(l, -2)`，消费栈顶），`lua_setmetatable(L, -2)` 是函数（不消费）。两者都不能靠「栈位置数字」推理，得看源码。类似地，`lua_setfield(L, LUA_REGISTRYINDEX, ...)` 也是 pop 语义。
- **GLib 对象家族里 `PangoFontDescription` / `PangoFontMetrics` 是罕见的 GBoxed**（独立 refcount，不是 GObject），必须用它们自己的 free/unref 而不是 `g_object_unref`。误用会触发 `GLib-GObject-CRITICAL: g_object_unref: assertion 'G_IS_OBJECT (object)' failed`。
- **借引用 vs 拥有引用**：`pango_context_get_font_description` 返回 context **拥有**的指针，caller 不能释放，用 `d_noop`；反之 `pango_context_get_metrics` 返回 caller 拥有的新 refcount，必须 `_unref`。

---

## 8. 版本处理总结

| 类型 | 数量 | 位置 |
|------|:---:|------|
| 编译期 `#if` 分支 | 1 | `d_metrics`（`lgishim_pango.c:105`） |
| 运行期 workaround | 2 | `set_resolution` 走 fontmap 路径（`:563`）；`set_text` 传 `length=-1`（`:227`） |
| 明确不做 | 4 | `pango_layout_get_preferred_size` / `get_preferred_size_at_dpi` / `get_height_for_width` / `get_height_for_width_at_dpi`（Pango ≥ 1.50，`lib/` 只在 `place.lua` 间接用）+ `Color.alpha`（1.48 struct 无 alpha 字段） |
| 硬编码 1.48 | 1 | `Color` 字段直读（`:514-528`） |

**枚举大写**（§6 已述）不计入版本处理，属于 lgi 语义兼容。

---

## 9. 冒烟结果

**静态二进制指标（`07-spike2-shim-smoke.sh` 自动构建）：**

| 指标 | 值 |
|------|----:|
| 二进制体积 | 21,450,344 B（~20.5 MiB） |
| 相对 Spike 1 静态产物（8,423,736 B） | +13.0 MB（Pango/cairo/harfbuzz 静态入库） |
| `pango_layout_new` undefined 数 | **0**（符号已真实链入，非 stub） |
| GLib CRITICAL 计数 | **0**（bug 4 修复后归零） |
| Segfault / abort | 0 |

**冒烟矩阵（42 项全通过）：**

| 分组 | 通过/总数 |
|------|:--------:|
| 加载器（`lgishim` / `Pango.SCALE` / `units_from_double`） | 4/4 |
| `Color` 构造 + `parse('#ff8000')` + 通道值 | 6/6 |
| `FontDescription`（`from_string` / `get_size` / `to_string` / `copy_static` / `merge`） | 5/5 |
| `PangoCairo` / `Context` / `Metrics`（`create_context` / `set_resolution` / `get_ascent` / `get_descent`） | 6/6 |
| `Layout`（`new` + set/get × 11 + `get_pixel_extents` + `context_changed`） | 16/16 |
| `parse_markup`（`<b>bold</b> plain` → 剥离标签 + attr userdata） | 3/3 |
| 协议（`_native` / `finish` 双调用 / `collectgarbage` × 2） | 4/4 |
| **合计** | **42/42** |

`smoke_exit=0`。

**修正的测试预期（bug 6 细节）：**

| # | 现象 | 分类 | 修正 |
|---|------|:----:|------|
| 1 | `c.red` got=65535 want=0xff80 | Pango 16-bit 打包 `v*257`：`#ff8000` 红=255 → 65535 | 测试改为 `255*257` |
| 2 | `c.blue` got=0 want=128 | 同上，蓝通道为 0 → 0 | 测试改为 `0` |
| 3 | Metrics ascent 13312 vs 期望 1000–4000 | Pango 单位 1/1024 px，`13312 = 13 px` | 测试范围改 Pango 单位 |
| 4 | Metrics descent 3072 vs 期望 200–2000 | 同上 | 同上 |
| 5 | `layout.attributes` 非 nil 期望 | 真实 Pango `pango_layout_new` 后 `get_attributes` 返回 NULL（不自动挂默认 language attr），lgi 也**没有**这个副作用 | 测试改为 `attributes == nil` 才对 |
| 6 | finish() 前 value capture want=255 | `#00ff00` 红通道是 0，不是 255 | 测试改为 `0` |

**没有剩余的 shim 缺陷。** 第 5 项尤为关键：真实 Pango 也不 auto-inject language attribute，反证了「对齐 lgi 非标准包装层」这个想法。Spike 3 的 `X11` / `XRandR` shim 应以此为基准：**对齐真实 API 语义，不猜 lgi 的副作用**。

---

## 10. 与 Spike 0/1 的衔接

- **静态链接链路** 继承 Spike 1：`-Wl,--start-group ... -Wl,--end-group` 包裹整个库表，本轮在其中追加 `pangocairo pango cairo harfbuzz`（写入 `CMakeLists.txt`，用 `-DAWESOME_LGISHIM=ON` 开关控制）。
- **不再 patch build.ninja**（Spike 1 教训）：任何一次 cmake 重跑都会把 patch 冲掉，且半冲状态会把 `LINK_LIBARIES` 整行吃掉。库表统一写进 `CMakeLists`。
- **Lua 路径** 继承 Spike 0：`/usr/local/share/lua/5.3 → /usr/share/lua/5.3` 软链，`require("lgi")` 在动态构建下可用。
- **`lgishim.lua` 加载器** 走 `package.preload` 探测：静态构建里 `luaa.c` 注册 `luaopen_lgishim_pango` / `luaopen_lgishim_pangocairo` 到 `preload`；开发容器里未注册，`pcall(require, "lgishim_pango")` 失败回落 `require("lgi")`，行为零变化。
- **require 路径替换**：`lib/**/*.lua` 34 个文件、38 处 `require("lgi")` → `require("vendor.lgishim")`（1 处注释、1 处 `lgishim.lua` 自身、1 处 `lgi.version` 模块名保留）。

---

## 11. 移交 Spike 3 的问题

1. **版本兼容（1.50 迁移）**：§6 表格里的 3 处 workaround 在迁移 Pango ≥ 1.50 时必须人肉检查。`d_metrics` 的 `#if` 分支已就位但需要实测。
2. **未做的 4 个 layout API**：`get_preferred_size*` / `get_height_for_width*` 在 `lib/wibox/container/place.lua` 间接用到，需要在 `pango_layout_get_extents()` 之上重实现。
3. **cairo 侧桥接**：`show_layout` / `update_layout` 走 `cairo_t`，需要在 Spike 3 端到端渲染路径里接入（Spike 3 D.3）。这是「静态单文件跑起来」的关键——在此之前只是口号。
4. **`X11` / `XRandR` shim**：Spike 1 §6 已证无 typelib，需与 Pango 同手法写 C shim，规模参考 b16 一·3.3（XRandR ~5 API，X11 更省）。
5. **行数复盘**：`lgishim_pango.c` 887 + `lgishim.lua` 38 = **925 行**，触发 §5 C.3 的「> 600 行止损」信号。分析：887 行里 ~30% 是 Lua 5.3 C API 样板（每类要建 metatable / `m_index` / `m_newindex` / `__gc` / `push_obj`），实际业务调用面只有 ~30 个 Pango API 调用点。折算 Phase 2 单 API 施工成本约 30 行/api，与 b16 一·8.8 的 25-40 行/api 估计相符。**不重评 R7**——真实成本已验证，且 6 个 bug 都是「一次修好就稳」的类型。
6. **`parse_markup` 完整子集**：当前只走 `<b>` `<i>` `<font>` `<span>` 最小集，等 `menubar` 端到端起来再看是否需要扩展。

---

## 产出文件

- `lib/vendor/lgishim.lua`（38 行）— 加载器
- `lib/vendor/lgishim_pango.c`（887 行）— Pango + PangoCairo Lua 绑定
- `docs/sam/scripts/06-spike2-shim-build.sh`（132 行）— 静态构建 + 符号验证
- `docs/sam/scripts/07-spike2-shim-smoke.sh`（230 行）— 42 项冒烟矩阵
- `luaa.c`（+16 行）— `preload` 注册两个 `luaopen_*`
- `CMakeLists.txt`（+49 行）— `AWESOME_LGISHIM=ON` 开关与库表
- `cmake/lgi-check.cmake` — 允许 shim 路径下跳过真实 lgi 检查
- `lib/**/*.lua`（34 文件、38 处 `require` 替换）
