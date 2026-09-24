# Phase 2 范围界定与起步决策 · Spike 4

> **本文覆盖 Phase 2 全局（Part 1-4 划分 + Part 5-13 范围与人天）。逐 Part 的执行记录见 `2026-09-23-record2.md` 顶部对照表。**

> 采集时间：2026-09-24 09:15 CST
> 前置：Spike 3 D.5「调整 R6」决策通过 → Phase 2 起步
> 配套：`2026-09-23-record2.md`（Phase 2 执行记录）· `spike3-r6-decision.md`（R6 总决策）· `spike3-cairo-semantics.md`（D.3 cairo 语义）· `spike3-oopango-coverage.md`（oocairo 覆盖率数据）· `2026-09-23-impl-plan.md`（阶段计划）

---

## 一、原始决策 vs 当前决策

### 1、Spike 3 D.3 的原始判断：复用 oocairo

Spike 3 D.3 对 Cairo shim 的结论是「拿 oocairo 当底层，lib/ 侧套一层 LGI 语义」，核心依据：

| 数据点 | 值 | 来源 |
|--------|---|------|
| oocairo 方法覆盖 | 47/56 = ~84% | `spike3-cairo-semantics.md` §5 |
| 未覆盖需 shim 补齐 | 9 类（构造函数改名、enum→string、属性赋值、`._native`、`is_type_of`、`cr.status`、Pango 桥接、`cairo.Surface(obj,true)` 通用转换、Rsvg） | 同上 §4.2 |
| 估算人天 | 2.5-3 | 同上 §5.1 |

D.3 的计算逻辑：oocairo 覆盖 84% 方法，剩下的 9 类缺口在 shim 层补齐（约 250-400 行），总成本 2.5-3 人天——落在 b16「Cairo 2-4 人天」区间下沿。

Spike 3 D.5 综合决策采纳此数字，写入了 Phase 2 施工清单的「Cairo 层 2.5-3 人天」档（`spike3-r6-decision.md` §3）。

### 2、Phase 2 起步决策：不复用，写自己的（2026-09-24 01:20）

Phase 2 启动后的第一个决策就是推翻 D.3。原因五条：

| # | 原因 | 详情 |
|---|------|------|
| 1 | **容器无 oocairo** | `pkg-config oocairo` 未找到，Alpine 3.15 仓库无此包。要么 autotools 源码构建一个 12+ 年老项目，要么放弃——两条路都不省事 |
| 2 | **命名空间不兼容** | oocairo 模块名 `Ctx`/`Surface`，非 LGI 惯例的 `Cairo.Context`/`Cairo.Surface`。lib/ 侧 35 处 `cairo.Context(...)` 静态构造调用均依赖 `Cairo.*` 命名空间，套 oocairo 需要再加一层别名层 |
| 3 | **无 `._native` / `is_type_of` / 幂等 `__gc`** | lib/ 硬需求：10 处 `._native` + 9 处 `is_type_of`（`spike3-lgi-api-inventory.md`）。oocairo 对象是纯 Lua table 包装，这三条它一条都给不了 |
| 4 | **对象模型冲突** | oocairo 是纯 Lua 包装（非 full userdata + destroy callback），与 `lgishim_pango.c` 的 wrap struct 模式（`typedef struct { void *p; void (*destroy)(void*); int cls; } wrap;`）不兼容。混用引入两套对象模型，`m_index`/`m_gc`/`push_obj` 全要写两份 |
| 5 | **复用净收益接近 0** | oocairo 提供的 `cr:move_to(x, y)` 等纯 C API 包装，自己写 5 行，比包装 oocairo 的 `Ctx.MoveTo(cr, x, y)`（函数式调用，cr 还得先转 full userdata）还简单 |

**结论：写 `lib/vendor/lgishim_cairo.c` 独立 shim**，沿用 `lgishim_pango.c` 的 wrap struct + metatable 模式，静态链接 `-lcairo`（已在 `CMakeLists.txt:141` 的 `AWESOME_REQUIRED_LDFLAGS`）。

---

## 二、关键洞察：复用不到什么东西

这是本文档最有价值的部分——它解释了为什么「84% 覆盖率」是虚假的安全感。

### 1、需求 × oocairo 对照

| 需求 | oocairo 提供？ | 自己写成本 |
|------|:---:|:---|
| wrap struct + destroy callback | ✗ | 30 行 |
| `._native` / `:finish()` / `__gc` 幂等 / `is_type_of` | ✗ | 60 行 |
| `cr.status` 返回字符串 | ✗（返回整数） | 10 行 |
| enum 常量表（12 张，字符串语义） | ✗ | 100 行 |
| `__newindex` 拦截 5 属性赋值 | ✗ | 40 行 |
| `cr:move_to` / `line_to` 等 56 方法 | ✓ | 每方法 5 行 = 280 行 |
| `cr:set_source(pattern)` 跨对象 | ✓ | 已含上表 |
| `cr:show_layout(Pango Layout)` | ✗ | 20 行 |
| **合计** | | **~540 行** |

自己写 540 行 C 比包装 oocairo 多写约 300 行，但换来：

| 收益 | 说明 |
|------|------|
| **静态链接干净** | 不用 build oocairo，不引 `liboocairo.so` |
| **与 lgishim_pango 同手法** | wrap struct + metatable 模式完全复用；`getud`/`chk`/`push_obj`/`class_setup` 等 helper 直接复制 |
| **无额外依赖** | CMake 里 `-lcairo` 已通过 pangocairo 的 Cflags 拉进头路径，不用新增 `pkg_check_modules(cairo)` |
| **规避已修 6 个 bug** | Spike 2 修过的 `is_type_of` 语义、`attributes=nil`、`metrics` 单位、`font` 单位、`finish` 前后值——写法已踩过一遍坑，Cairo shim 直接照搬正确姿势 |
| **命名对齐 LGI 惯例** | `Cairo.Context` / `Cairo.Surface` / `Cairo.Pattern`，lib/ 侧 35 处 `cairo.Context(...)` 调用零改动 |

### 2、净结论

oocairo 的 84%（47/56）数量覆盖率里，真正「复用不写」的核心是 47 个 `cr:xxx()` 方法，每个 5 行 = 235 行；剩下 5 个特殊方法 + 属性 getter/setter + shim 元方法（`._native`/`:finish()`/`is_type_of`/`__newindex` 拦截/enum 常量表/`cr.status`）共 265 行，**oocairo 一条都给不了**。

「复用」在净代码量上只是 235 行 vs 280 行的差（省 45 行），但在依赖、静态链接、对象模型一致性上全是净亏损。

---

## 三、Phase 2 范围：六层实测调用面

Phase 2 覆盖六层 shim：Cairo · Pango · GLib · Gio · GdkPixbuf · Rsvg。以下为 grep 实测（`lib/` 全量 171 个 `.lua` 文件，排除注释行）。

### 1、Cairo 层

已在 Spike 3 D.3 完成详测：19 个静态标识、56 方法、9 处 `is_type_of`、10 处 `._native`、~150 处 enum 常量引用。详见 `spike3-cairo-semantics.md`。

### 2、Pango 层

Spike 2 骨架（`lgishim_pango.c` 887 行）已覆盖约 30 API；剩余 ~20 API 在 Phase 2 补齐。详见 `spike2-pango-shim.md`。

### 3、GLib 层

| API | 类型 | 出现次数 | 集中文件 |
|-----|------|:---:|------|
| `GLib.Variant(...)` | 构造函数 | 8 | `naughty/dbus.lua` |
| `GLib.build_filenamev` | 函数 | 5 | `menubar/icon_theme.lua` |
| `GLib.get_system_data_dirs` | 函数 | 2 | `menubar/icon_theme.lua` |
| `GLib.String(cmd)` / `GLib.String.hash` | 类+方法 | 2 | `awful/spawn.lua` |
| `GLib.get_home_dir` | 函数 | 1 | `menubar/icon_theme.lua` |
| `GLib.get_user_data_dir` | 函数 | 1 | `menubar/icon_theme.lua` |
| `GLib.PRIORITY_DEFAULT` | 常量 | 1 | `awful/spawn.lua` |
| `GLib.Timer()` | 构造函数 | 1 | `wibox/container/scroll.lua` |
| `glib.timeout_add(...)` | 函数 | 1 | `gears/timer.lua` |
| `glib.source_remove(...)` | 函数 | 1 | `gears/timer.lua` |
| `GLib.Error` | 类型 | 0¹ | `wibox/widget/textbox.lua:162`（仅注释） |
| `GLib.Bytes` | 类型 | 0¹ | `awful/spawn.lua:536`（仅注释） |

> ¹ `GLib.Error` 和 `GLib.Bytes` 在 lib/ 源码中仅出现于注释（Lua 注释行、`--`），非实际 API 调用。但 lgi 运行时这两个类型可能由 Pango markup 解析或 Gio stream 读取产生，shim 需预留类型桩位。

**GLib 调用面总结：** 集中在 4 个文件——`gears/timer.lua`（主循环）、`naughty/dbus.lua`（Variant）、`menubar/icon_theme.lua`（路径拼接）、`awful/spawn.lua`（String hash + PRIORITY_DEFAULT）。与 Spike 3 D.5 的判断「GLib 绑定面只有 2 函数 + 常量」相比，实测多了 **Variant（8 处）、build_filenamev（5 处）、String（2 处）、Timer（1 处）**，人天需上调。

### 4、Gio 层

| API | 类型 | 出现次数 | 集中文件 |
|-----|------|:---:|------|
| `Gio.File.new_for_path` | 类+方法 | 9 | `gears/filesystem.lua`（8）+ `beautiful/init.lua`（1） |
| `Gio.FileQueryInfoFlags` | 枚举 | 4 | `gears/filesystem.lua` |
| `Gio.DataInputStream.new` | 构造函数 | 3 | `awful/spawn.lua` |
| `Gio.MemoryInputStream.new_from_data` | 构造函数 | 2 | `awful/spawn.lua` |
| `Gio.UnixInputStream.new` | 构造函数 | 2 | `awful/spawn.lua` |
| `Gio.BusType` | 枚举 | 2 | `naughty/dbus.lua` |
| `Gio.DBusProxy.new_for_bus` | 构造函数 | 1 | `naughty/dbus.lua` |
| `Gio.DBusProxyFlags` | 枚举 | 1 | `naughty/dbus.lua` |
| `Gio.DBusCallFlags` | 枚举 | 1 | `naughty/dbus.lua` |
| `Gio.DBusArgInfo` | 类型 | 1 | `naughty/dbus.lua` |
| `Gio.DBusMethodInfo` | 类型 | 1 | `naughty/dbus.lua` |
| `Gio.DBusSignalInfo` | 类型 | 1 | `naughty/dbus.lua` |
| `Gio.DBusInterfaceInfo` | 类型 | 1 | `naughty/dbus.lua` |
| `Gio.bus_own_name` | 函数 | 1 | `naughty/dbus.lua` |
| `Gio.BusNameOwnerFlags` | 枚举 | 1 | `naughty/dbus.lua` |
| `Gio.IOErrorEnum` | 枚举 | 1 | `gears/filesystem.lua` |
| `Gio.InputStream` | 类型 | 0² | `awful/spawn.lua:501`（仅 ldoc `@tparam`） |
| `Gio.DBusConnection` | 类型 | 0² | `naughty/dbus.lua:38`（仅注释） |

> ² `Gio.InputStream` 和 `Gio.DBusConnection` 在源码中仅出现于注释/文档，非直接 API 调用。但 `Gio.DataInputStream.new(input_stream)` 隐式依赖 InputStream 类型系统。

**Gio 调用面总结：** 集中在 3 个文件——`naughty/dbus.lua`（DBus 全栈，~11 个 API）、`gears/filesystem.lua`（Gio.File 文件操作，9 处 File + 4 处 FileQueryInfoFlags + 1 处 IOErrorEnum）、`awful/spawn.lua`（UnixInputStream + DataInputStream + MemoryInputStream，7 处）。**DBus 调用面比 Spike 3 预计的大得多**——不仅是 `gio.Async` 可砍的问题，而是 `naughty/dbus.lua` 完整实现了一个 D-Bus 通知守护进程，依赖 `Gio.DBus*` 的 11 个 API。

### 5、GdkPixbuf 层

未做专项 grep。b16 一·3.6 列举的 `lib/gears/surface.lua`（`pixbuf_to_surface`）和 `lib/wibox/widget/imagebox.lua`（PNG 加载）在 Phase 2 范围内，人天 0-2。

### 6、Rsvg 层（决策 B：只 PNG，人天 0）

| API | 出现次数 | 文件 |
|-----|:---:|------|
| `Rsvg.Handle.new_from_data` | 1 | `wibox/widget/imagebox.lua:87` |
| `Rsvg.Handle.new_from_file` | 1 | `wibox/widget/imagebox.lua:89` |
| `Rsvg.Handle:is_type_of` | 2 | `wibox/widget/imagebox.lua:408,423` |

Spike 3 D.4 决策 B（只 PNG），Rsvg 人天 0。但 `lgishim.lua:29` 的 `Rsvg = {}` 空表仍需保留——`imagebox.lua:408,423` 用 `Rsvg and Rsvg.Handle:is_type_of(image)` 判空，空表使短路求值返回 `nil`（不报错），后续走 PNG 降级路径。

> **⚠ 2026-09-24 校定：上面这句推理是错的。** `{}` 是 Lua **真值**，`a and b` 不会短路——右侧 `Rsvg.Handle` 照常被求值，返回 nil，下一行 `nil:is_type_of(...)` 直接抛 `attempt to index a nil value`。带 `pcall` 的调用点只是把报错变成静默失败。Rsvg 空表**不**是安全降级，它是当前唯一未处理的阻塞项。见 `spike4-phase2-07-lib-lgi-coverage-matrix.md` §F1。

### 7、GObject 层

| API | 出现次数 | 文件 |
|-----|:---:|------|
| `GObject.Closure` | 4 | `naughty/dbus.lua`（434, 541, 542） |

GObject.Closure 是 D-Bus 信号回调的关键——没有它，`bus_own_name` 的三个回调（`on_bus_acquire`/`on_name_acquired`/`on_name_lost`）和 `DBusInterfaceInfo` 的 `method_call` 回调都无法注册。开工时 `lgishim.lua` 的 `GObject = {}` 空表意味着 Notifications D-Bus 守护进程**完全不可用**。

> **✅ 2026-09-24 已解决（Part 9，见 `2026-09-23-record2.md` 篇八）：** `GObject.Closure` + `closure_marshal` 已落地，`lgishim.lua` 改为 `GObject = ok6 and GObject or {}`。

---

## 四、施工拆解

### 1、Cairo 层：`lgishim_cairo.c` 分 Part 1-4 串行

| Part | 内容 | 方法数 | 类型 |
|------|------|:---:|------|
| **Part 1** | 基础层：wrap struct + destroy + 核心函数（`getud`/`chk`/`push_obj`/`m_index`/`m_newindex`/`m_native`/`m_finish`/`m_gc`/`m_is_type_of`）+ 12 张 enum 常量表（字符串语义）+ `class_setup` + 模块入口 | — | 元方法 + 枚举 |
| **Part 2** | Ctx 方法：drawing 19（`move_to`/`line_to`/`curve_to`/`rectangle`/`fill`/`stroke`/...）+ state 31（`save`/`restore`/`translate`/`scale`/`rotate`/...）+ text/font 方法 + 构造器替换（Part 1 的 5 个 stub → 真实实现） | **61** | 方法体 |
| **Part 3** | Surface 15 + Pattern 12 + Matrix 3 + Region（新类 C_REGION=6，含 `create_rectangle`/`intersect`/`union`/`subtract`/`xor`/`contains_point`/`get_rectangle`/`is_empty`/`num_rectangles` 等 ~20 方法） | **50+** | 方法体 + 新类 |
| **Part 4** | 属性 getter/setter：18 属性（`cr.operator`/`cr.source`/`cr.fill_rule`/`cr.antialias`/`cr.hairline`/`cr.tolerance`/`cr.line_width`/`cr.line_cap`/`cr.line_join`/`cr.miter_limit`/`cr.dashes`/`cr.matrix`/`cr.target`/`cr.font_face`/`cr.font_size`/`cr.font_slant`/`cr.font_weight`/`cr.font_matrix`）+ `__newindex` 拦截 5 属性赋值（`pattern.extend`/`pattern.filter`/`pattern.matrix`/`pattern.content`/`cr.operator`）+ `cr.status` 字符串 getter | — | 属性层 |

**每部分单独跑一次冒烟**（`docs/sam/scripts/10-spike4-cairo-smoke.sh`），Part 1 冒烟验证枚举+元方法+类表（108 项），Part 2 追加 Ctx 方法往返（306 项），Part 3 追加 Surface/Pattern/Matrix/Region + 真实 awesome 调用路径（424 项）。

### 2、依赖顺序

```
Part 1（基础层）
  ↓
Pango 桥接补丁：lgishim_pango.c 暴露 check_layout() + Pango.Context.new() 修复
  ↓
Part 2（Ctx 方法 + show_layout/update_layout 桥接 + FontOptions.new）
  ↓
Part 3（Surface / Pattern / Matrix / Region）
  ↓
Part 4（属性 getter/setter + __newindex 拦截）
```

Part 2 里的 `show_layout` / `update_layout` 需要 `lgishim_pango.c` 暴露 `lgishim_pango_check_layout()` 做跨模块 Layout 识别（metatable 身份校验，非 cls 编号——因为 `C_LAYOUT=2` 与 cairo 的 `C_SURFACE=2` 数值撞车）。

### 3、Phase 2 九项施工清单

| # | 类别 | 内容 | 人天 | 状态 |
|---|------|------|:---:|------|
| 1 | Cairo | `lgishim_cairo.c` Part 1-4 | 2.5-3 | **✅ 全部完成**（Part 4 属性层零 C 改动） |
| 2 | Pango | 4 个未实现 layout API | 2-3 | 未开始 |
| 3 | Pango | Pango 1.50 迁移（4 个 `#if` 分支） | 0.5 | 未开始 |
| 4 | Cairo | show_layout / update_layout 桥接 | 1 | **已完成**（Part 2c） |
| 5 | Cairo | is_type_of 元方法 | 0.5 | **已完成**（Part 1） |
| 6 | Cairo | enum 常量代理表 | 0.3 | **已完成**（Part 1） |
| 7 | Cairo | __newindex 属性拦截 | 0.5 | **Part 4 内** |
| 8 | Cairo | cr.status 字符串 getter | 0.2 | **Part 4 内** |
| 9 | X11/XRandR | shim 全量 | 5-8 | 未开始 |

**总人天：39-53**（与 `spike3-r6-decision.md` §3 一致）。

---

## 五、人天估算与可信度

| 子项 | 人天 | 可信度 | 依据 |
|------|:---:|:---:|------|
| Cairo 层 | 2.5-3 | ✅ 已实测 | Part 1-3 已完成，Part 4 剩余量小（~18 属性 getter/setter + 5 __newindex 拦截），实测施工速度验证了估算 |
| Pango 层（剩余） | 2-3 | ⚠️ 推测 | 4 个 layout API + Pango 1.50 迁移，未开始 |
| GLib 层 | 3-6 | ⚠️ 推测→上调 | Spike 3 原估 3-5，实测调用面多了 Variant×8 + build_filenamev×5 + String×2 + Timer×1。人天区间上调至 4-6 |
| Gio 层 | 3-8 | ⚠️ 推测→上调 | Spike 3 原估 3-5，实测调用面多了完整 DBus 子系统和 DataInputStream/MemoryInputStream。砍 menubar 档下，人天区间上调至 5-8 |
| GdkPixbuf 层 | 0-2 | ⚠️ 推测 | 无新实测 |
| Rsvg（可选） | 0 | ✅ 已实测 | 决策 B，PNG-only |
| X11/XRandR | 1-2 | ⚠️ 推测 | Spike 1 §6 证无 typelib；参考 b16 一·3.3 |
| 静态链接收口 | 8-12 | ✅ 已实测 | Spike 1 + Spike 2 两轮 dry-run |
| 测试回归 | 4-6 | ⚠️ 推测 | Spike 0 基线 568 pass |
| **合计** | **39-53** | — | 与 Spike 3 D.5 一致 |

**可信度分级：**

| 等级 | 项 |
|:---:|-----|
| ✅ 已实测 | Cairo、Rsvg、静态链接 |
| ⚠️ 推测 | Pango 剩余、GLib、Gio、GdkPixbuf、X11/XRandR、测试回归 |

**GLib / Gio 上调风险提示：** Spike 3 D.5 的 GLib 3-5 和 Gio 3-5 是基于「glib 只有 2 函数 + 常量」的保守估计。Phase 2 起步后的实测 grep 暴露了更大的调用面（GLib 多了 Variant 和文件路径 API，Gio 多了完整 D-Bus 栈），原估可能偏乐观。但 Cairo 层实测施工速度（Part 1-3 约 2 人天完成）提供了正面校准。

---

## 六、计划 vs 实际

### 1、Part 1 的计划偏差

| 项 | 计划（`2026-09-23-record2.md` §1.4） | 实际 | 偏差 |
|----|------|------|:---:|
| 行数 | 250-300 | 545 | +82% |
| 人天 | 1.5 | ~2（含复验 + 冒烟脚本） | +33% |
| 枚举常量 | 19 静态标识+~150 字符串常量 | 12 张枚举表，字符串语义 | — |

**偏差根因：Agent 交付需复验。**

Part 1 是 Agent 交验的，四类问题全在复验时暴露（`2026-09-23-record2.md` §2.2）：

| # | 问题 | 严重度 |
|---|------|:---:|
| 1 | 枚举名伪造（`SUBTRACT`/`HARDLIGHT`/`SOFTLIGHT`/`OUT_OVER`/`IN_OVER`/`CONTENT_NONE` 在真 cairo 里不存在） | 编译失败 |
| 2 | `install_enum` 与 `enum_from` 语义不一致（push integer vs opt string → 运行时 "string expected, got number"） | 运行时必炸 |
| 3 | `lua_pushlstring(L, s, -1)` → `SIZE_MAX` → "block too big" 段错误 | 加载即崩 |
| 4 | `is_type_of` 用 `w->cls` + `strcmp` 判定 → cairo/pango cls 编号四档全撞，跨模块类混淆 | 静默误判 |

**教训：Agent 交付需复验，下轮估人天时按 1.5x 计。** Part 1 的额外 0.5 人天全部花在复验四类错误 + 建冒烟脚本上。这个开销不是一次性的——后续各 Part 均应预留复验 buffer。

### 2、Part 2 的计划偏差

Part 2 未单独给计划行数（`record2.md` §1.4 只列了 Part 1），实际产出约 1182 行新增（构造器替换 + 61 方法 + 5 个 C 层 bug 修复 + `push_matrix`/`d_noop` 辅助函数）。

5 个 C 层 bug 是在冒烟首跑时连续触发的：`class_call` 索引用错、`m_index` 栈纪律、Matrix 按指针返回的 use-after-free、`ctx_reset_matrix` 传 NULL、`ctx_get_source`/`ctx_get_target` 用错销毁回调。每个都是「写法看起来对、跑起来就炸」的类型，进一步印证「复验是硬成本」。

### 3、Part 3 的计划偏差

Part 3 计划未单独列行数，实际产出约 645 行新增（Surface 15 + Pattern 12 + Matrix 3 + Region ~20 + `set_resolution` 修复 + Pango.Context 修复）。集成阶段抓出 3 个新问题：

- **跨模块 metatable 名碰撞**（`lgishim.Context` 被 pango 和 cairo 共享，`luaL_newmetatable` 返回已有表导致方法表被覆盖）：pango 的 `CLS_NAMES` 全加 `lgishim_pango.` 前缀 + `REG_CLASSES` 切分命名空间
- **`check_layout` 栈索引错**（metatable 在 `-3` 而非 `-2`，恒返 NULL）
- **`REGION_PG` 属性遮蔽方法**（`num_rectangles` 和 `is_empty` 放属性表里导致 `region:num_rectangles()` 变成"调整数"）

---

## 七、计划缺口：shim 与 impl-plan 的差距

### 1、`lgishim.lua` 空表的真缺口

开工时（本文采集时刻）`lib/vendor/lgishim.lua` 的四张空表：

```lua
GLib = {},
Gio = {},
Rsvg = {},
GObject = {},
```

impl-plan（`2026-09-23-impl-plan.md` 第三章）Phase 2 范围列了 GLib / Gio / GdkPixbuf 三层。**空表意味着这些模块当时完全不工作**——代码路径走到即 `attempt to index a nil value`（比返回 nil 更安全：大声炸，不静默失败）。

> **⚠ 2026-09-24 校定（两条）：**
> 1. **四张里只剩 `Rsvg` 一张。** `GLib`（Part 5）、`Gio`（Part 6）、`GObject`（Part 9）已接入 `luaa.c` 的 `package.preload`，`lgishim.lua` 相应改为 `okN and M or {}`。见 `2026-09-23-record2.md` 篇八/篇九。
> 2. **「空表比返回 nil 更安全」只在直接索引的意义上成立**（`{}.x` 得 nil 不抛错；`nil.x` 抛 `attempt to index a nil value`）。但 `{}` 是 Lua **真值**，所以 `if x and x.y` **不会短路**，右侧照样求值——任何写成 `if Rsvg and Rsvg.Handle ...` 的判空守卫都形同虚设。

### 2、实测调用面 vs impl-plan 预期

| 层 | impl-plan 预期 | 实测调用面 | 缺口 |
|----|------|------|------|
| GLib | `glib.timeout_add` + `glib.source_remove`（2 函数） | + Variant×8 + build_filenamev×5 + String×2 + Timer×1 + PRIORITY_DEFAULT×1 + get_home_dir×1 + get_user_data_dir×1 + get_system_data_dirs×2 | **多 ~8 个 API** |
| Gio | `Gio.UnixInputStream`（1 类） | + File×9 + DBus 全栈~11 API + DataInputStream×3 + MemoryInputStream×2 + FileQueryInfoFlags×4 + IOErrorEnum + BusType×2 | **多 ~30 个调用点** |
| GObject | 未列（impl-plan 无此项） | Closure×4（DBus 信号回调必需品） | **整层未规划** |
| Rsvg | 决策 B 砍掉 | Handle×4（`imagebox.lua:87,89` 构造 + `:408,423` 判型） | **⚠ 空表并不安全**，见上方校定与 `spike4-phase2-07-lib-lgi-coverage-matrix.md` §F1 |

**关键发现：DBus 不是「可砍」。**

Spike 3 D.2（timer 分块）论证了 `gio.Async` 可替代，但 `naughty/dbus.lua` 不是 Async——它是完整的 **D-Bus 通知守护进程客户端**，依赖 `Gio.DBusProxy`/`Gio.DBusInterfaceInfo`/`Gio.DBusMethodInfo`/`Gio.DBusSignalInfo`/`Gio.DBusArgInfo`/`Gio.bus_own_name`/`Gio.BusType`/`Gio.DBusCallFlags`/`GObject.Closure`。砍 menubar 不会减少 D-Bus 调用面；砍 D-Bus 意味着 **Notifications 规范（org.freedesktop.Notifications）完全丢失**。

### 3、缺口的人天影响

| 缺口 | 原估人天 | 建议修正 | 理由 |
|------|:---:|:---:|------|
| GLib 调用面扩大 | 3-5 | **4-6** | + Variant（8 处，需 GVariant 格式化）+ build_filenamev（5 处，纯字符串操作）+ String（2 处）+ Timer（1 处） |
| Gio 调用面扩大 | 3-5 | **5-8** | + Gio.File（9 处）+ DBus 全栈（~11 API）+ DataInput/MemoryInputStream（5 处）。DBus 是最重的部分 |
| GObject 整层未规划 | 0 | **0.5-1** | Closure 是 GObject 的基础设施，需要 C 包装 `g_closure_new_object` + 回调→Lua 栈转换 |

合计人天区间：原 39-53 → 建议 **41-58**（上调 2-5 人天，主要来自 GLib+Gio+GObject 调用面修正）。

---

## 八、当前状态快照（2026-09-24 09:15）

### 1、Cairo shim

| 项 | 值 |
|----|-----|
| 文件 | `lib/vendor/lgishim_cairo.c` |
| 行数 | **2379**（`2026-09-23-record2.md` §4.2 记录 2372，+7 行为后续微调） |
| Part 1-3 | **完成** |
| Part 4 | **✅ 完成**（2026-09-24，零 C 改动——getter 表已随 Part 1-3 落盘，见 `2026-09-23-record2.md` 篇五） |

### 2、Pango shim

| 项 | 值 |
|----|-----|
| 文件 | `lib/vendor/lgishim_pango.c` |
| 行数 | 887（Spike 2 基线） |
| 新增导出 | `lgishim_pango_check_layout()`（跨模块 Layout 识别） |
| 修复 | `Pango.Context.new()` 挂到 `pango_cairo_font_map_get_default()`（原直接调 `pango_context_new()` 没有 font map，layout 死循环吐 critical） |
| CLS_NAMES | 加 `lgishim_pango.` 前缀（解决与 cairo 的 metatable 名碰撞） |

### 3、构建

| 项 | 值 |
|----|-----|
| 编译 | `AWESOME_IGNORE_LGI=1 ninja awesome` → `[6/6] Linking`，**0 warning** |
| 产物 | **9,539,912 bytes**（`file` 报告 `statically linked, stripped`） |
| `ldd` | `not a dynamic executable` |
| `readelf -d` | 无 dynamic section |

### 4、冒烟

| 项 | 值 |
|----|-----|
| 脚本 | `docs/sam/scripts/10-spike4-cairo-smoke.sh`（1120 行） |
| 检查项 | **424 项，全部通过**（0 failures） |
| 覆盖 | Part 1 枚举+元方法 → Part 2 Ctx 方法往返 → Part 3 Surface/Pattern/Matrix/Region → §15 Pango 桥接 → §20 真实 awesome 调用路径（`drawable.lua` 脏区域裁剪、`placement.lua` fit_in_bounding、`gears/surface.lua` create_similar、`background.lua` push/pop_group、`textbox.lua` draw()） |

### 5、shim 层状态矩阵

| 模块 | lgishim.lua 状态 | 实际可用性 |
|------|:---:|------|
| Pango | ✅ `lgishim_pango` | 可用（Spike 2 基线 + Part 2c 修复） |
| PangoCairo | ✅ `lgishim_pangocairo` | 可用（Spike 2 骨架） |
| Cairo | ✅ `lgishim_cairo` | ✅ Part 1-4 全部可用（Part 4 属性层零 C 改动） |
| GLib | ✅ `lgishim_glib`（1568 行 C） | ✅ Part 5 已落地（2026-09-24） |
| Gio | ✅ `lgishim_gio`（2399 行 C） | ✅ Part 6 已落地（2026-09-24） |
| GdkPixbuf | ✅ `lgishim_pixbuf`（387 行 C） | ✅ Part 8 已落地（2026-09-24） |
| GObject | ✅ `lgishim_gobject`（667 行 C） | ✅ Part 9 已落地（2026-09-24） |
| Rsvg | ❌ `{}` 空表 | ⚠ **决策 B 砍掉，但空表并不安全**——见 §七.1 校定 |

> **✅ 2026-09-24 校定：** 本表是 Phase 2 起步时的快照。Cairo Part 4、GLib、Gio、GdkPixbuf、GObject 均已交付。冒烟 **829 checks / 0 failures**，静态产物 **9,597,256 bytes**。

### 6、lib/ 修改状态

| 文件 | 修改 | 状态 |
|------|------|------|
| 34 个 .lua 文件 | `require("lgi")` → `require("vendor.lgishim")` | 稳定 |
| `lib/gears/color.lua:325,334` | 曾因 Pattern 返回值形状被 Agent 修改 | **已 revert** |
| `lib/wibox/container/background.lua:33,36` | 同上 | **已 revert** |

---

## 附 A：grep 验证记录

以下数字为本文档独立 grep 验证（非照抄来源文档）：

```bash
# Gio 调用面
grep -rn 'Gio\.File[^Q]' lib/ --include='*.lua' | grep -v '^\s*--' | wc -l    # 9
grep -rn 'GLib\.Variant' lib/ --include='*.lua' | grep -v '^\s*--' | wc -l     # 8
grep -rn 'Gio\.UnixInputStream' lib/ --include='*.lua' | grep -v '^\s*--' | wc -l  # 2
grep -rn 'glib\.timeout_add\|glib\.source_remove' lib/ --include='*.lua' | grep -v '^\s*--' | wc -l  # 2
grep -rn 'GObject\.' lib/ --include='*.lua' | grep -v '^\s*--' | wc -l          # 4
grep -rn 'Rsvg\.' lib/ --include='*.lua' | grep -v '^\s*--' | wc -l             # 4
```

**注意：** 来源文档声称「`Gio.DBusConnection`/`Gio.InputStream` 各 1」「`GLib.Error`/`GLib.Bytes` 各 1」——grep 实测这四处仅出现在 Lua 注释行（`--`），非实际 API 调用。本文档已标注为 0 并注明注释来源，同时列出 grep 发现的额外 API（Gio.DBus* 全栈、GLib 文件路径函数等）。

## 附 B：oocairo 覆盖率数据来源

| 数据 | 值 | 来源 |
|------|-----|------|
| oocairo 方法覆盖 | 47/56 = ~84% | `spike3-cairo-semantics.md` §5 |
| oopango 数量覆盖率 | 28/54 ≈ 52% | `spike3-oopango-coverage.md` §2 |
| oopango 语义覆盖率 | 17/54 ≈ 31% | 同上 |
| oopango shim 特性覆盖 | 1/8 = 12.5% | 同上 §3 |

---

## 附 C：本次更新说明（2026-09-24 校定）

1. **§三·6 Rsvg 层**：删除「Rsvg 空表比 nil 更安全」判断。`lgishim.lua:35` 的 `Rsvg = {}` 是**待交付状态**，不是已交付安全网；Part 6 Gio 的空表事故证明「空表静默成功」在有真实调用方时是致命的。详见 `spike4-phase2-04-c-api-conventions.md §八·6`。
2. **§七·1 `lgishim.lua` 空表缺口**：四张空表从「全空」→「GLib / Gio / GObject 已交付，只剩 Rsvg 一个真缺口」；`{}` 是否安全按调用方分档讨论。
3. **§七·3 缺口人天**：Part 4 状态从「待做」→「完成（零 C 改动）」——part 4 property 层复用 Cairo / Pango 已有 getter 集合，人天 0。
4. **§八·4 冒烟状态**：`424 checks` → **`829 checks`**（Part 4-9 追加 6 段 + 两道接线护栏）；静态产物 `9,539,912 bytes` → **`9,597,256 bytes`**。
5. **§八·5 shim 层状态矩阵**：全绿；Cairo / Pango / GLib / Gio / GdkPixbuf / GObject 六 shim 均交付，Rsvg 保留 `{}` 待交付。
6. **状态快照时间**：`09:15` → **`14:00`**（新增 Part 4-9 时间）。
7. **顶部标注**：加一行「本文覆盖 Phase 2 起步决策与范围。逐 Part 的执行记录见 `2026-09-23-record2.md` 顶部对照表」。