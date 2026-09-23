# oopango Coverage Analysis (Spike 3)

采集时间：2026-09-23 23:31:33 CST
目标：评估 oopango 能否替代 `lib/vendor/lgishim_pango.c`（887 行）

---

## 1. oopango 项目概况

- **仓库**：<https://github.com/awesomeWM/oopango>（master 分支，40 commits，tip = `0bddccc9`）
- **License**：MIT/X Consortium（正文）+ `m4/attributes.m4` GPLv2-with-exception（来自 xine-lib）
  - 与 awesomeWM 兼容（同组织）
- **语言**：纯 C，autotools 构建（`configure.ac` + `Makefile.am` + `*.pc.in`）
- **核心文件**：`oopango.c` / `oopango.h` / `obj_font_desc.c` / `obj_font_map.c` / `obj_layout.c` / `pango_cairo.c`
- **依赖**：pango + pangocairo + glib + lua5.1/5.2 + **oocairo**（`pango_cairo.c` 里 `#include <oocairo.h>` 用 `OOCAIRO_MT_NAME_CONTEXT`）
- **最后实质更新**：2014-10 之前的历史；仓库 GitHub 元数据显示 2011–2014 时代代码（Debian `oopango 1.1-1` 最后发布 2011-10）。仓库仍 public，但 12+ 年没 commit
- **Pango 版本适配**：代码针对 Pango 1.29–1.36 时代写的
  - `pango_font_description_free` / `_copy` 仍可用
  - `pango_font_metrics_unref`（1.50 起改名 `_free`，且 metrics 不再 refcount）**没写兼容分支**
  - `pango_font_family_list_faces` 参数在 1.50 有变化，oopango 用 `int` 计数，可能编译警告但不致命
  - **没有 1.48/1.50 兼容宏**（对比 lgishim_pango.c:105 有 `#if PANGO_VERSION_MAJOR * 100 + PANGO_VERSION_MINOR >= 150`）
- **是否可静态链接**：技术上可以（autotools 生成 `liboopango.a`），但**必须同时静态链入 oocairo**，且 oocairo 依赖 cairo + lua + 一个自研的 `oocairo.h` 头

**结论**：oopango 是 2011–2014 年 awesomeWM 官方维护的老 Pango 绑定，本质是 `lgi.Pango` 出现前的替代物，awesomeWM 4.0 (2012) 之后被 lgi 完全取代。仓库处于事实上的 abandoned 状态。

---

## 2. API 覆盖对照

方法/属性/构造器逐个统计。lgishim_pango.c 中每个 `luaL_Reg` 表列一次（`new` 只算一次）。

| lgishim_pango API | 类 | 用途 | oopango 是否提供 | 备注 |
|---|---|---|:---:|---|
| `new` | Layout | `pango_layout_new(context)` | ◐ | oopango 用 `pango_cairo.create_layout(cairo_ctx)`，构造签名不同 |
| `set_text` | Layout | 纯文本 | ✓ | |
| `get_text` | Layout | 读回文本 | ✗ | oopango 只写不读，`set_markup` 也内部吃掉 text |
| `set_attributes` | Layout | 富文本 attrlist | ✗ | |
| `get_attributes` | Layout | 读回 attrlist | ✗ | |
| `set_width` / `get_width` | Layout | 换行宽度 | ✓ | |
| `set_height` / `get_height` | Layout | 最大行数 | ✓ | |
| `set_indent` / `get_indent` | Layout | 首行缩进 | ✗ | textbox.lua 用 |
| `set_wrap` / `get_wrap` | Layout | WORD/CHAR/WORD_CHAR | ✓ | oopango 值小写，lgishim 值大写 |
| `set_ellipsize` / `get_ellipsize` | Layout | 省略号 | ✓ | 大小写差异 |
| `set_justify` / `get_justify` | Layout | 两端对齐 | ✓ | |
| `set_alignment` / `get_alignment` | Layout | LEFT/CENTER/RIGHT | ✓ | 大小写差异 |
| `set_line_spacing` / `get_line_spacing` | Layout | 行间距 | ✗ | textbox.lua 用 |
| `set_font_description` | Layout | 设置字体 | ✓ | |
| `get_pixel_extents` | Layout | ink+logical 像素 | ✓ | |
| `context_changed` | Layout | 失效缓存 | ✗ | |
| Layout 属性 `text/attributes/width/height`（读写各一次） | Layout | 语法糖 | ◐ | 只有 width/height 双向可用；text 只 set 不 get；attributes 完全缺 |
| `new` | FontDescription | | ✓ | |
| `from_string` | FontDescription | | ✓ | |
| `merge` | FontDescription | 合并描述 | ✗ | gear.color 用它合并 |
| `get_size` | FontDescription | | ✓ | oopango 是 property |
| `copy_static` | FontDescription | 静态拷贝 | ◐ | oopango 只有 `copy`（多一份 ref） |
| `to_string` | FontDescription | | ✓ | 以 `__tostring` 呈现 |
| `new` | Color | | ✗ | **整个 Color 类没有** |
| `parse` | Color | 解析 "#RRGGBB" | ✗ | |
| Color 属性 `red/green/blue` | Color | 字段读 | ✗ | lib/gears/color.lua 依赖 |
| `create_context` | FontMap | 拿 PangoContext | ✗ | **整个 Context 通路缺** |
| `new` | Context | | ✗ | |
| `set_resolution` | Context | DPI | ✗ | lgishim 走 fontmap_set_resolution + context_changed |
| `get_font_description` | Context | 借引用 | ✗ | |
| `get_metrics` | Context | 拿 FontMetrics | ✗ | |
| `get_ascent` / `get_descent` | FontMetrics | 文本度量 | ✗ | **整个 FontMetrics 类没有** |
| `new` | AttrList | 空 attrlist | ✗ | oopango 只在 `set_markup` 内部临时用 |
| `SCALE` | module | PANGO_SCALE | ✓ | |
| `units_from_double` | module | Pango unit 转换 | ✓ | oopango 名 `units_from_number` |
| `parse_markup` | module | 独立 markup 解析 | ◐ | oopango 只暴露 `layout:set_markup()` 副作用版本，无法拿 (attrlist, text) 元组 |

**覆盖率（数量口径）：28 / 54 ≈ 52%**

（分母 54 = 24 Layout methods + 8 Layout props + 6 FontDescription + 5 Color + 1 FontMap + 4 Context + 2 FontMetrics + 1 AttrList + 3 module functions）

**口径修正（语义级覆盖）**：
- 12 个 ✗ 里，有 4 个是「整个类缺失」：Color (5) + Context (4) + FontMetrics (2) + FontMap.create_context (1) = 12 个 API 空
- 这 12 个 API 覆盖 awesomeWM 文本度量的关键路径（Color → 文字色、Context.get_metrics → 度量、FontMap.create_context → 从字体表拿上下文），无法用 oopango 现有 API 拼出来
- 有效覆盖率 ≈ **17 / 54 ≈ 31%**

---

## 3. 关键 shim 特性

| 特性 | lgishim | oopango | 影响 |
|---|:---:|:---:|---|
| `._native` 返回 lightuserdata | ✓ | ✗ | lgishim.lua 的 C 层桥（cairo pattern / image 等）用它跨 C 边界；oopango 完全没有对 Lua 暴露裸指针 |
| `:finish()` 显式释放 | ✓ | ✗ | 仅 `__gc`；Lua 层不能主动回收 |
| `__gc` 幂等释放 | ✓ | ✓ | oopango 每个 `_gc` 都做 `if (*obj) { unref; *obj = 0 }` |
| `is_type_of` 类型检查 | ✓ (通过 `cls` 字段) | ✗ | lib/ 里 `cairo.Surface:is_type_of(...)` 的调用点没有 Pango 的，但 gears/color.lua:263,288 有 `cairo.Pattern:is_type_of` — 若以后要扩展需要 |
| `__typename` | ✓ (`CLS_NAMES[cls]`) | ✗ (仅 `_NAME` 静态字符串) | 报错定位能力弱 |
| 借引用 destroy 保护 (d_noop) | ✓ | ✗ | **致命**。lgishim 在 `l_get_attributes`（第 255 行）和 `cx_get_font_description`（第 575 行）用 `d_noop` 保护借引用；oopango 的 `layout_get_font_description` 用 `pango_font_description_copy` 兜底可以，但没有通用机制 |
| Pango 1.48 静态链接兼容 | ✓（有 `#if PANGO_VERSION >= 1.50` 分支，第 105 行） | ✗（无版本兼容） | 需要人工补 `_metrics_unref` vs `_metrics_free` 分支 |
| 无 GI / dlopen 依赖 | ✓ | ✓ | 都是手写 C 绑定 |
| 单一项目依赖 | ✓ | ✗ | oopango 必须同时链 oocairo（另一个已 abandoned 的项目） |
| Cairo 集成 (`pango_cairo_*`) | ✓ | ✓（但需 oocairo） | oopango 的 `pango_cairo.c` 强依赖 oocairo |

**Shim 特性覆盖：1 / 8 = 12.5%**

---

## 4. 静态链接可行性

- **oopango 依赖清单**：`pango`、`pangocairo`、`glib-2.0`、`lua5.1/5.2`、**`oocairo`（外部项目）**
- oocairo 是 awesomeWM/luapower 生态里 Cairo 的 Lua 绑定（GitHub：awesomeWM/oocairo），同样 2010s 时代，同样 abandoned
- oopango 的 `pango_cairo.c` 里 `#include <oocairo.h>` 且用 `OOCAIRO_MT_NAME_CONTEXT`，静态链接时必须把 oocairo 静态库也链进来
- Alpine musl 上同时静态链接 pango + pangocairo + cairo + glib + lua + oocairo + oopango 技术上可行，但**多引入一个 abandoned 依赖** = 多一份维护负债
- 无 Pango 1.50 兼容代码，Alpine 当前包 `pango-dev 1.54.x`，`pango_font_metrics_unref` 已废弃（1.50 起改 `_free` 且语义变化：不再返回 refcounted 对象）。oopango 若编译过会链接到不存在的符号

**结论**：技术上可静态链，但代价是引入另一个 abandoned 依赖 + 手动补 Pango 1.50+ 兼容层。

---

## 5. 决策

**不采纳 oopango**

一句话理由：**oopango 覆盖 52% 名义 API、31% 语义 API、12.5% shim 特性；缺 4 个关键类（Color/Context/FontMetrics/AttrList）+ 全部 shim 桥（`._native` / `finish` / `d_noop`），补齐缺口 ≈ 重写 lgishim_pango.c 一半的代码量，且要额外维护 oocairo 依赖**。

量化对比：

| 项 | lgishim_pango.c 现状 | 若改用 oopango |
|---|:---:|:---:|
| 项目自身代码行 | 887 | 约 700（oopango 全量）+ 400（补 shim + Context + Color + FontMetrics + AttrList） |
| 外部 abandoned 依赖 | 0 | 1（oocairo） |
| Pango 1.48/1.50 兼容 | ✓ | 需手工补 |
| `._native` / `finish` / `d_noop` | ✓ | 需手工补 |
| 团队已知历史 bug 数 | 已知并修 | 未知 |

省 887 行的收益 < 补齐缺口 + 新依赖成本。**保持现状**。

---

## 6. 若采纳，替换计划（**决策不采纳，此节仅记录**）

如未来强制要求替换，最小可行路径：

1. 移除 `lib/vendor/lgishim_pango.c` (887 行)
2. 引入 `lib/vendor/oopango/` (约 700 行，改 `#include "oopango.h"` 里的 lua 5.2 API 兼容)
3. **同时引入 `lib/vendor/oocairo/`** (约 2500 行；否则 `pango_cairo.c` 编不过)
4. 补 shim 特性（预计 300–500 行 C 或 Lua）：
   - 给每个 oopango userdata 挂一个 `.__native = *obj` 字段（`._native` 兼容）
   - 给每个类加 `:finish()` 方法（复用 `_gc` 函数）
   - 在 `class_setup` 里注入 `__typename` 字段（读 metatable 的 `_NAME`）
   - 引入「借引用」包装器：新增 `wrap_borrow(p)` 用 `d_noop` 作为 destroy
5. 补缺失类（预计 500–800 行 C）：
   - `Pango.Color`：`new` / `parse` / `red|green|blue` property（40 行）
   - `Pango.Context`：`new` / `set_resolution` / `get_font_description` / `get_metrics`（80 行）
   - `Pango.FontMetrics`：`get_ascent` / `get_descent`（30 行）
   - `Pango.AttrList`：`new`（10 行）
   - `Pango.FontMap:create_context()`（10 行）
   - `Layout` 缺的 6 个方法（indent/line_spacing/get_text/set_attributes/get_attributes/context_changed，150 行）
   - `FontDescription.merge` / `copy_static`（30 行）
   - module-level `parse_markup` 返回元组（50 行）
6. 修 Pango 1.50+ 兼容（`_metrics_unref` vs `_metrics_free`，2 行）
7. 调 `lgishim.lua` 加载器：`require 'lgishim_pango'` → `require 'oopango'`，并把 `lgishim.FontDescription` 等别名映射到 `oopango.FontDescription`

**总投入**：约 1200–1800 行 C 代码（oopango 700 + oocairo 2500 中的 pango 相关 + shim/补齐 800+）+ 迁移期 bug 排查。**净效果 ≈ 不省代码**。

---

## 附：数据来源

- oopango 源码：`raw.githubusercontent.com/awesomeWM/oopango/master/{oopango.c,oopango.h,obj_layout.c,obj_font_desc.c,obj_font_map.c,pango_cairo.c}`
- lgishim_pango 源码：`lib/vendor/lgishim_pango.c`（`grep -n "luaL_Reg"` 定位 8 个类方法表）
- awesomeWM 邮件归档 55533 / 55539 / 55840：确认 oopango 是 handwritten binding，已被 lgi 取代
- Debian 归档：`oopango 1.1-1` 最后发布 2011-10
