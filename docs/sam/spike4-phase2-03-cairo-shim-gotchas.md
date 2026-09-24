# Cairo Shim 施工踩坑全记录（Spike 4）

> **本文覆盖 Part 1-4。逐 Part 的执行记录见 `2026-09-23-record2.md` 顶部对照表。**

> 范围：`lib/vendor/lgishim_cairo.c`（2379 行）+ `lib/vendor/lgishim_pango.c`（996 行）跨模块交互。
> 所有结论均可追溯到源码注释或冒烟脚本（`docs/sam/scripts/10-spike4-cairo-smoke.sh`，**829 项全绿**）。
> 构建产物 **9,597,256 bytes** 全静态链接，ninja 0 warning。

---

## 一、Lua C API 栈陷阱

### 1、`lua_pushlstring(L, s, -1)` = 分配 16 EiB

**症状：** 模块加载时 Lua 直接崩：
```
memory allocation error: block too big
```

**根因：** 第三个参数是 `size_t`（无符号），`-1` 变成 `SIZE_MAX`（≈ 16 EiB），`luaM_malloc_` 直接报 fatal。这是 LuaJIT / Lua 5.1 时代的"用 strlen"惯用法，Lua 5.3 里不成立。

**修法：** 改用 `lua_pushstring(L, s)`——它内部调 `strlen`。

**源码位置：** `lgishim_cairo.c:572`（`make_class_table` 内注释明确注明"NOT lua_pushlstring(..., -1)"）、`lgishim_cairo.c:647`（`class_setup` 内同样注释）。

Pango shim 没踩这个坑是因为它没有 `is_type_of`，这条路径当时从未被走过。

### 2、`class_call` 的 `lua_call` nargs 语义

**症状：** `cairo.Rectangle{x=1, y=2, width=10, height=10}` 报 `attempt to call a table value`；`cairo.Matrix.new()` 这种 0 参构造器反而正常。

**根因：** 原始代码写 `lua_call(L, lua_gettop(L)-2, 1)`。`lua_call(L, nargs, nresults)` 调用的是栈上 `top - nargs` 位置的函数——即栈底第二个值（第一个实参），而不是类表的 `new` 方法。0 参构造器恰好栈布局碰对，1+ 参时必错。

**修法：** 显式取 `new` 再调，确保被调函数紧贴在所有实参之上（标准 `[func, arg1, arg2, ...]` 布局）：

```c
lua_getfield(L, 1, "new");      /* [self, args..., new] */
lua_insert(L, 2);               /* [self, new, args...]   */
lua_remove(L, 1);               /* [new, args...]         */
lua_call(L, lua_gettop(L)-1, 1); /* new(args...)           */
```

**源码位置：** `lgishim_cairo.c:501-517`（`class_call` 函数，注释完整描述了根因和修法）。

**注意：** Pango shim 的 `class_call`（`lgishim_pango.c:829-835`）仍用旧写法 `lua_call(L, lua_gettop(L)-2, 1)`，但 pango 的 class 都是 `new()` 无参构造器，恰好没触发。

### 3、`m_index` 属性查找时的栈纪律

**症状：** 访问属性（`cr.matrix` / `cr.font_size`）时 `attempt to index a nil value`。

**根因：** 新增"属性 setter 查找"路径（表 `p`）后，代码尝试 `lua_getfield(L, -1, "p")` 时，栈上 `-1` 槽位仍是方法表而非类信息表——因为没在查 `p` 之前弹出 `m` 表和 value。栈上多出两层，`-1` 指向错误对象。

**修法：** 查 `p` 之前先 `lua_pop(L, 2)`（弹出方法表 + 对应 value），再走属性解析。

**源码位置：** `lgishim_cairo.c:416-429`（`m_index` 中查 `p` 表前先 `lua_pop(L, 2)`）。

---

## 二、跨模块身份冲突（代价最高的两个坑）

### 4、`cls` 编号跨模块碰撞 → 必须用 metatable 身份

**症状：** `cairo.FontOptions:is_type_of(Pango.Color())` 返回 **true**——静默误判，比 crash 危险得多。

**根因：** cairo 和 pango 各自定义 `enum { ... }` 的 cls 编号，**四个索引全部撞车**：

| cls | cairo | pango |
|----:|-------|-------|
| 1 | C_CTX | C_ATTRLIST |
| 2 | C_SURFACE | C_LAYOUT |
| 3 | C_PATTERN | C_FONTDESC |
| 4 | C_FONT_OPTIONS | C_COLOR |

`is_type_of` 原用 `strcmp(CLS_NAMES[other->cls], expected)` 判定——cairo 的 CLS_NAMES 数组里查 pango 的 cls 编号，当然撞。lib/ 里 `cairo.Surface:is_type_of(image)`（`imagebox.lua:427`）会把 `Pango.Layout` 误判成 Surface。

**修法：** `is_type_of` 的 upvalue 捕获**本类 metatable 表本身**（`luaL_getmetatable` 取回），用 `lua_rawequal` 比对 userdata 的 metatable。跨模块的 metatable 是不同的表，身份比较天然隔离。

**源码位置：** `lgishim_cairo.c:519-546`（`m_is_type_of`，注释解释为什么不靠 `w->cls`）。

**附加陷阱：** lib/ 用**冒号调用** `is_type_of`：`cairo.Surface:is_type_of(image)` = `is_type_of(cairo.Surface, image)`，slot 1 是类表（不是 userdata），slot 2 才是被测对象。写成 `getud(L, 1)` 永远返回 false。正确做法是 `lua_getmetatable(L, 2)`。

### 5、metatable **名字**也跨模块碰撞（#4 的延伸）

**症状：** `panctx.set_resolution == nil`——`Pango.Context` 对象查不到自己的 setter。

**根因：** `luaL_newmetatable(name)` 的行为是：名字已注册就**直接返回已有表**，不新建。Pango shim 和 Cairo shim 的 `CLS_NAMES` 都写了 `"lgishim.Context"`，但指向两个**不同的 C 类型**（`PangoContext*` vs `cairo_t*`）。

`lgishim.lua` 的加载顺序是 pango → pangocairo → cairo，所以 cairo 的 `class_setup` 调 `luaL_newmetatable("lgishim.Context")` 时拿到的是 pango 那张 metatable，然后把自己的 `__index`/`__newindex`/`__gc`/`finish` **覆盖上去**，还把 cairo 的 Context 方法表装在同一张表上。一个 `pango_context_t` 走到 `:set_resolution()`，命中的是 cairo 的 `m_index`，按 cairo 的 `C_CTX=1` 走完全错误的分支。

**两个维度都得切**，只做一个都不行：

| 维度 | 修改 | 原因 |
|------|------|------|
| `CLS_NAMES` 加前缀 | `"lgishim.Context"` → `"lgishim_pango.Context"` | 切断 `luaL_newmetatable` 的名字碰撞 |
| `REG_CLASSES` 改名 | `"lgishim_classes"` → `"lgishim_pango_classes"` | `m_index` 是 `REG_CLASSES[w->cls]` 按模块内序号取类信息，pango `C_CONTEXT=6` 与 cairo `C_REGION=6` 数值撞车 |

第 2 条不能省：只改名不切数组，`w->cls` 查到的仍然是另一个模块的类信息表，方法照样取错。

**源码位置：**
- `lgishim_pango.c:60-69`（`CLS_NAMES` 全部带 `lgishim_pango.` 前缀）
- `lgishim_pango.c:71-82`（`REG_CLASSES` 改为 `"lgishim_pango_classes"`，注释完整解释两个维度）
- `lgishim_cairo.c:106-116`（cairo 侧 `CLS_NAMES` 保持 `"lgishim."`，`REG_CLASSES` 保持 `"lgishim_classes"`）

**诊断探针套路：** 打印 `debug.getregistry().lgishim_classes[i].m.xxx` 和 `debug.getmetatable(u)`，能直接看出"类信息在哪张数组、metatable 归谁"。

---

## 三、属性表 / 方法表顺序陷阱

### 6、属性表优先于方法表，且 getter 会被调用

**症状：** `region:num_rectangles()` 报 `attempt to call a number value`。

**根因：** `m_index` 的查找顺序是 **getter 表（`g`）→ 方法表（`m`）→ setter 表（`p`）→ metatable**。getter 命中后会 `lua_call` 返回**结果值**（一个整数），后续 `region:num_rectangles()` 变成调一个整数。

lib/ 用法是方法形式：`drawable.lua:114` 调 `region:num_rectangles()`，`drawable.lua:112` 调 `region:is_empty()`。

**修法：** 凡是 lib/ 用**冒号调**的名字，**绝不能进属性表 `g`**，只能留在方法表 `m`。

**源码位置：** `lgishim_cairo.c:2253-2263`（`REGION_PG` 注释明确解释"num_rectangles 和 is_empty 只留方法表"）。

| 名称 | 在 `g`（属性表）？ | 在 `m`（方法表）？ | 原因 |
|------|:---:|:---:|------|
| `num_rectangles` | ✗ | ✓ | `drawable.lua:114` 冒号调用 |
| `is_empty` | ✗ | ✓ | `drawable.lua:112` 冒号调用 |
| `empty` | ✓ | ✗ | 属性读（`region.empty`） |

---

## 四、枚举相关

### 7、枚举常量必须是字符串，不是整数

**症状：** `cr:set_operator(cairo.Operator.SOURCE)` 报 `string expected, got number`。

**根因：** lib/ 的枚举用法是**字符串字面量比较**：

- `lib/wibox/drawable.lua:166`：`assert(cr.status == "SUCCESS", ...)`
- `lib/wibox/container/background.lua:240`：`if status == "SUCCESS" then`
- `lib/gears/color.lua:151`：`pattern:set_extend(cairo.Extend.REPEAT)`

所以 `install_enum` 必须 `lua_pushstring`（不是 `lua_pushinteger`），配 `enum_from` 的 `luaL_optstring`——**两端必须一致**。

**源码位置：** `lgishim_cairo.c:173-178`（注释写明字符串语义）、`lgishim_cairo.c:366-376`（`install_enum` 用 `lua_pushstring`）。

### 8、Agent 编造了 6 个不存在的 cairo 枚举名

**症状：** 编译报错 `CAIRO_OPERATOR_SUBTRACT undeclared`、`CAIRO_CONTENT_NONE undeclared` 等。

**根因：** Agent 凭记忆写的枚举名，对照真头文件 `/usr/include/cairo/cairo.h`（cairo 1.16.0）后发现：

| 错误名（Agent 编造） | 真名 / 处理 |
|----------------------|------------|
| `CAIRO_OPERATOR_SUBTRACT` | `CAIRO_OPERATOR_SATURATE`（值 13） |
| `CAIRO_OPERATOR_HARDLIGHT` | `CAIRO_OPERATOR_HARD_LIGHT`（值 21） |
| `CAIRO_OPERATOR_SOFTLIGHT` | `CAIRO_OPERATOR_SOFT_LIGHT`（值 22） |
| `CAIRO_OPERATOR_OUT_OVER` | 不存在，删 |
| `CAIRO_OPERATOR_IN_OVER` | 不存在，删 |
| `CAIRO_CONTENT_NONE` | 不存在（`Content` 只有 `COLOR`/`ALPHA`/`COLOR_ALPHA`） |

另外 **`EXTEND` 整张表缺失**（lib/ 用 `cairo.Extend.REPEAT`/`PAD`），`FONTWEIGHT` 也缺失。

**源码位置：** `lgishim_cairo.c:220-243`（`OPERATOR` 枚举，含 `SATURATE`/`HARD_LIGHT`/`SOFT_LIGHT`）、`lgishim_cairo.c:257-260`（`EXTEND`）。

### 9、`cairo.Rectangle` 是构造函数不是枚举

**症状：** 如果 `Rectangle` 做成枚举表，`cairo.Rectangle{x,y,w,h}` 直接报错（类表遮蔽构造函数）。

**根因：** lib/ 用法是**构造函数**：`cairo.Rectangle{x,y,width,height}`（`theme_assets.lua:203` 等 4 处）、`cairo.RectangleInt{...}`（6 处）。它们返回纯 Lua table，不是 userdata，不需要 metatable。

**修法：** `Rectangle` / `RectangleInt` 用 `make_call_table` 做成纯 `__call` 表，`new` 方法原样返回输入 table。

**源码位置：** `lgishim_cairo.c:310-313`（注释"Deliberately NOT installed as enum"）、`lgishim_cairo.c:2077-2095`（`rectangle_new` + `RECT_M`）、`lgishim_cairo.c:2347-2352`（`luaopen_lgishim_cairo` 中用 `make_call_table` 安装）。

---

## 五、所有权与内存安全

### 10、所有权事实一览

以下结论通过 `/_ext/tmp/logs/cairo_probe.c` 逐项 fork 独立进程验证：

| API | 所有权 | destroy 回调 | 验证方式 |
|-----|--------|:-----------:|---------|
| `cairo_get_source()` | **借用**（不增 ref） | `d_noop` | fork 后 `get_source` 再 `destroy`，不 abort |
| `cairo_get_target()` | **借用** | `d_noop` | 同上 |
| `cairo_pop_group()` | **调用方拥有 ref** | `d_pattern` | 不调 `destroy` 则 valgrind 报 leaked |
| `cairo_set_matrix(cr, NULL)` | — | — | SIGSEGV（直接解引用 NULL） |
| `cairo_get_dash(cr, dashes, *offset)` | — | — | 此版本只有 2 个出参（dashes + offset） |
| `cairo_get_font_size()` | — | — | 此版本不存在（cairo 1.16.0 无此 API） |

#### 10.1 `get_source` / `get_target` 用错销毁回调 → GC 时 abort

**症状：** `Assertion failed: CAIRO_REFERENCE_COUNT_HAS_REFERENCE`。

**根因：** `cairo_get_source()` 和 `cairo_get_target()` 返回**借用指针**（不增 refcount）。shim 原用 `d_pattern` / `d_surface` 作为 destroy 回调——Lua GC 时对借用指针调了 `cairo_pattern_destroy()` / `cairo_surface_destroy()`，refcount 被多减一次触发断言。

**修法：** 两处都改用 `d_noop`（空销毁回调）。

**源码位置：** `lgishim_cairo.c:1486-1489`（`ctx_get_source` 用 `d_noop`，注释写"BORROWED pointer"）、`lgishim_cairo.c:1578-1584`（`ctx_get_target` 同理）。

#### 10.2 `set_matrix(NULL)` → SIGSEGV

**症状：** `cr:reset_matrix()` 段错误。

**根因：** 原实现调 `cairo_set_matrix(cr, NULL)`。此容器 cairo 1.16.0 的 `cairo_set_matrix` 对 NULL 参数直接解引用（内部无条件 `cairo_matrix_multiply(&tmp, &cr->gstate->ctm, matrix)`）。

**修法：** 传显式单位阵。

**源码位置：** `lgishim_cairo.c:1435-1444`（`ctx_reset_matrix`，注释"NOT cairo_set_matrix(cr, NULL)"）。

#### 10.3 Matrix 按指针返回的 use-after-free

**症状：** `Matrix:invert()` 返回的矩阵里 `xx` 读到 `4.94e-324`（垃圾值）；同样影响 `matrix_mul`、`ctx_get_matrix`、`pattern_get_matrix`。

**根因：** 四个 getter 函数内部声明了栈局部 `cairo_matrix_t m`，取 `cairo_matrix_t *p = &m`，把指针包进 userdata 返回。Lua GC 触发 `free()` 时释放的是**栈地址**——该栈帧早已归还，栈内存被后续调用覆盖。

**修法：** `push_matrix()` 做堆拷贝：

```c
static int push_matrix(lua_State *L, const cairo_matrix_t *src) {
    cairo_matrix_t *m = lua_newuserdata(L, sizeof(cairo_matrix_t));
    *m = *src;
    /* ... metatable ... */
}
```

**源码位置：** `lgishim_cairo.c:718-733`（`push_matrix`，注释"Every 'get a matrix' path must hand the wrapper a HEAP allocation"）。

---

## 六、容器环境特殊问题

### 11、本容器 cairo 没有 `CAIRO_PATTERN_TYPE_PATTERN` 成员

**症状：** `pop_group()` 返回的 pattern `:type` 是 `"SURFACE"` 而非 `"PATTERN"`。

**根因：** 编译/探针确认此容器的 `cairo_pattern_type_t` 枚举止于 `RASTER_SOURCE`（无 `PATTERN`/`IMAGE`）。这不是 egrep 漏了——头文件里确实没有。

**修法：** 冒烟断言改为"能映射到真实枚举名、不落到 UNKNOWN"，不断言具体值。上游 `lib/` 也没人检查 mask 的 type。

**源码位置：** `lgishim_cairo.c:324-331`（`PATTERN_TYPE` 枚举，止于 `RASTER_SOURCE`）。

### 12、`Pango.Context.new()` 死循环（Pango 1.48 特有问题）

**症状：** 调用后无限重试 + 海量 glib qdata critical（实测刷出 999,419 行 stderr）。

**根因：** 直接调 `pango_context_new()` 得到的 context 没有 font map、也没有 font backend。任何 fontset 加载走 `pango_context_load_fontset()` → 已废弃的 `pango_font_map_load_fontset(NULL, ...)` → 只报 `assertion 'fontmap != NULL' failed` 然后返回 NULL → layout 一路重试。

**修法：** 挂到默认 font map：
```c
pango_font_map_create_context(
    (PangoFontMap *)pango_cairo_font_map_get_default())
```
即 `pango_cairo_create_layout()` 的做法。修好后 "Hello" 给 ink=37x12、零 critical。

**注意：** `pango_font_map_get_default` 在此构建不存在（undefined reference），必须走 `pango_cairo_font_map_get_default`。

**源码位置：** `lgishim_pango.c:594-608`（`cx_new`，注释完整描述根因）。

### 13、`check_layout` 的栈索引错误

**症状：** `show_layout` / `update_layout` 全线报 `expected a Pango.Layout`，即使传入正确对象。

**根因：** metatable 校验把 metatable 拿在 `-2`，但那一格是 userdata 本身：

```
luaL_getmetatable(...)  → [.., mt1]
lua_pushvalue(idx)      → [.., mt1, ud]
lua_getmetatable(-1)    → [.., mt1, ud, udmt]   ← -1=udmt, -2=ud, -3=mt1
```

`lua_rawequal(L, -1, -2)` 比的是 udmt 和 ud，永远不等 → 恒返 NULL。

**修法：** 改成 `lua_rawequal(L, -1, -3)`。

**源码位置：** `lgishim_pango.c:155-171`（`lgishim_pango_check_layout`，栈注释已写对位置）。

---

## 七、工程纪律

### 14、返回值形状由 lib/ 决定，**不许改上游 lib/**

**现象：** 并发 agent 为了让 `get_color_stop_count` / `get_color_stop_rgba` 过测试，**改了上游 `lib/`**：

- `lib/gears/color.lua:325,334`
- `lib/wibox/container/background.lua:33,36`

**原则：** 本项目前提是上游 `lib/` 保持原样，只换 `require("lgi")` → `require("vendor.lgishim")`。shim 侧必须按 lgi 的 **status 优先**形状返回：

- `get_color_stop_count()` → `(status, count)`
- `get_color_stop_rgba(i)` → `(status, off, r, g, b, a)`

**检查手段：** `git diff origin/master -- lib/` 应该只剩 require 替换，任何 API 形状改动都是越界。冒烟里加 `clone_stops()` 逐字回归项防再漂。

**源码位置：** `lgishim_cairo.c:1220-1253`（`pattern_get_color_stop_rgba` 和 `pattern_get_color_stop_count` 的返回形状，注释"lgi puts the cairo status first"）。

### 15、磁盘事故（0 字节截断 + 恢复流程）

**现象：** 一次 edit 写入失败把 `lib/vendor/lgishim_pango.c` 截成 **0 字节**。`/dev/vda2` 已 100%（49G/50G），`dd` 写 100 字节即 ENOSPC。

**根因：** Docker overlay backing store（`/opt/docker-data`，容器命名空间里 `/opt` 是空的）占 ~41G。容器跑着时 `df` 常年 Available=0，删自己的文件不会让 Available 变化（docker 立刻吃掉释放的空间）。

**恢复：** `docker stop builder-awe` 释放 writable layer（实测 +15GB），然后 `git checkout HEAD -- lib/vendor/lgishim_pango.c`。

**教训：**
1. edit 写入失败后必须立刻 `wc -c <file>` 检查长度
2. 失败后用 `git checkout HEAD -- <file>` 恢复（需要磁盘空间）
3. 长期跑 build 容器要监控磁盘，`builder-awe-disk-watch` 类 watchdog 不可省

---

## 八、快速排查清单

| # | 症状关键词 | 第一怀疑 | 定位 |
|:--|-----------|---------|------|
| 1 | `block too big` | `lua_pushlstring(..., -1)` | 全文搜 `pushlstring` |
| 2 | `attempt to call a table value` | `class_call` 的 `lua_call` nargs | 搜 `lua_call.*gettop` |
| 3 | `is_type_of` 静默误判 | `w->cls` 跨模块碰撞 | 打印 metatable 身份比对 |
| 4 | `xxx == nil`（跨模块查不到方法） | metatable 名字碰撞 | 打印 `debug.getregistry()` |
| 5 | `attempt to call a number value` | 方法名进了属性表 `g` | 检查 `_PG` 数组 |
| 6 | `string expected, got number` | `install_enum` 用了 `lua_pushinteger` | 搜 `install_enum` |
| 7 | `CAIRO_XXX undeclared` | 枚举名伪造 | 对照 `/usr/include/cairo/cairo.h` |
| 8 | GC 时 `REFERENCE_COUNT` abort | 对借用指针用了 `d_*` destroy | 搜 `cairo_get_source` / `get_target` |
| 9 | 矩阵读回垃圾值 | 栈局部指针包进 userdata | 搜 `push_matrix` |
| 10 | `set_matrix(NULL)` → SIGSEGV | cairo 1.16 不防 NULL | 搜 `reset_matrix` |
| 11 | 无限 critical / 死循环 | `pango_context_new` 无 font map | 搜 `pango_font_map_create_context` |
| 12 | 文件 0 字节 | 磁盘满 | `df -h` + `docker stop` |

---

## 九、本次更新说明（2026-09-24 校定）

1. **头部行 6-7**：冒烟 **`424 项全绿`** → **`829 项全绿`**；产物 `9,539,912 bytes` → **`9,597,256 bytes`**；`lgishim_cairo.c` `~2379 行` → **`2379 行`**（去掉 tilde，已定稿）；新增 `lgishim_pango.c` `996 行`。
2. **内容不变**：Part 5-9 交付的是 GLib / Gio / GdkPixbuf / GObject 四个 shim（见 `05-glib-gio-scope`、`06-x11-pixbuf-scope`、`record2.md` 篇八/九），Cairo / Pango shim 从 Part 4 之后未再动。本文一～八章的 12 条踩坑仍逐字适用。