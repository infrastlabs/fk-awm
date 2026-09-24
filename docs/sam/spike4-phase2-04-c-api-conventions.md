# lgishim C API 编写约定（Spike 4）

> **本文源自 Part 1-4，Part 5-9 的补充约定见本文「本次更新说明」。逐 Part 的执行记录见 `2026-09-23-record2.md` 顶部对照表。**

> 范围：`lib/vendor/lgishim_cairo.c`（~2379 行）+ `lib/vendor/lgishim_pango.c`（~938 行）+ `lib/vendor/lgishim.lua`（40 行）。
> 本约定是 **Phase 2 Part 1–4 实测沉淀，不是理论规范**。每一条都带有源码位置或 record2.md 节号，违反均可自查。
> 姊妹篇 `spike4-phase2-03-cairo-shim-gotchas.md` 管"踩坑清单"（已发生的每个 bug 的症状→根因→修法），本约定管"正向规则"（写新类／新模块要遵守什么）。

---

## 一、文档定位与适用范围

本约定回答的是：**「我要在 lgishim 体系里新增一个 C 类（Renderer / Pixbuf / XWindow …），每一步该怎么写才不会踩已知的坑？」**

与姊妹篇的分工边界：
- **`spike4-phase2-03-cairo-shim-gotchas.md`**：每个 bug 的完整病历（症状 → 根因 → 修法 → 源码位置），读它用来**排错**。
- **本约定**：每个环节的正确写法 + 违反时的典型现象，读它用来**写对**。

本约定不重复姊妹篇里已详述的具体 bug 修法；只提炼为可遵循的规则。

**本约定是 Phase 2 Part 1–4 实测沉淀，不是理论规范。** 所有规则均来自 `lgishim_cairo.c` / `lgishim_pango.c` 的实际代码和 `record2.md` 的执行记录。

---

## 二、Lua 栈纪律

### 1、取参一律用 `luaL_opt*` 族

**约定：** 参数从栈上 2 号位开始取（slot 1 是 self / userdata）。数值用 `luaL_checknumber(L, 2)`，字符串用 `luaL_checkstring(L, 2)`，布尔用 `lua_toboolean(L, 2)`。**绝不直接 `lua_tonumber` 后不校验**。

**反例/教训：** 取参不校验时 nil 被隐式转 0 或空串，后续 C API 收到脏值但不会立即崩，只在某些 corner case 才暴露。
**源码依据：** `lgishim_cairo.c:1491-1492`（`set_line_width` 用 `luaL_checknumber`）、`lgishim_cairo.c:1467-1472`（`set_source_rgb` 取 3 个 double）。

### 2、C 回调返回前必须 `lua_settop(L, 0)`

**约定：** 所有**不返回值**的 C 回调（setter、void 方法）必须以 `return 0` 结尾，且函数体中间不要残留栈值。`return 0` 等价于 `lua_settop(L, 0)` 后不 push 返回值——如果调用过程 push 了临时值又没清理，`return 0` 够用，但中间 `lua_call` 后栈上会多出 nresults 个值，必须提前 `lua_pop`。

**反例/教训：** `m_index`（`lgishim_cairo.c:380-441`）在四次查找（g → m → p → metatable）里每次失败后用 `lua_pop(L, 2)` 精确清理，才能在末尾安全 `return 0`。漏一次 pop 就会把取错的值当作索引结果返回给 Lua。
**源码依据：** `lgishim_cairo.c:406` / `416` / `429`（三次 `lua_pop`）。

### 3、别用 `lua_pushlstring(L, s, -1)`

**约定：** 任何需要 push 字符串的地方，**只许用 `lua_pushstring(L, s)`**。第三个参数 `-1` 作为 `size_t` 会变成 `SIZE_MAX`（≈ 16 EiB），`luaM_malloc_` 直接 fatal：`"memory allocation error: block too big"`。

**反例/教训：** Part 1 的 `make_class_table` 和 `class_setup` 初版用了 `lua_pushlstring(..., -1)`，模块加载即崩。修法：改为 `lua_pushstring`。
**源码依据：** `lgishim_cairo.c:572`（注释明确写"NOT lua_pushlstring(..., -1)"）、`lgishim_cairo.c:647`（同样注释），`record2.md §2.2 #3`。

### 4、`luaL_argcheck` 校验 + 多返回值返回约定

**约定：** 非法参数（NULL 指针、越界索引、类型不匹配）用 `luaL_argcheck` 或 `luaL_error` 报错，**不要**只 return nil 让调用方猜为什么 nil。栈索引一律从 1 起——在 `m_index` / `m_newindex` 等元方法中，slot 1 是 userdata（self），slot 2 是 key/value。GLib C 回调（timer、idle …）返回前必须 `lua_settop(L, 0)`，因为回调上下文与用户代码共享同一个 `lua_State`。

**多返回值：** 按 lgi 既有形状返回——不是"方便实现什么就返回什么"。例如 `get_color_stop_count` 必须返回 `(status, count)` 二值而非单值 `count`（`record2.md §4.3.4`），background.lua 的 `clone_stops()` 会按 `status, count = ...` 解包。

**源码依据：** `lgishim_cairo.c:1244-1253`（`get_color_stop_count` 先 push status 再 push count）、`lgishim_cairo.c:140-147`（`chk` 里的 `luaL_error`）、`record1.md §10.8 #4`（GLib 回调的 `lua_settop(L, 0)` 约束）。

### 5、`lua_pushstring(L, NULL)` 在 Lua 5.3 下断言

**约定：** `lua_pushstring(L, s)` 当 `s == NULL` 时 **不会** push `nil`（很多人以为 Lua 5.3 会这样），而是在 `lua_pushvfstring` 内部的 `luaL_addgsub` 路径上触发断言 / 报 `bad argument #1 to 'pushstring'`。C 属性 getter 如果可能返回 NULL 字符串（如 GLib 的 `get_name` / `get_display_name` 在未设定时返回 NULL），必须**先判空**，再把 NULL 映射为 `lua_pushnil` 或默认字符串。

**反例/教训：** `record2.md §八 #2`：GLib / Gio 属性 getter 未判空，首次碰到返回 NULL 的字符串属性就断言崩。
**源码依据：** `record2.md §八` 尾部（`lua_pushstring(L, NULL)` 断言项）。

---

## 三、userdata / wrap 约定

### 1、wrap struct 统一格式

**约定：** 所有出自 lgishim 的 C 对象都用同一结构：

```c
typedef struct {
    void *p;
    void (*destroy)(void *);
    int cls;
} wrap;
```

三个字段缺一不可：`p` 指向真实 C 对象、`destroy` 是 GC 时调用的析构函数（可为 `d_noop`）、`cls` 让 `m_index` 能定位到本类的 clsinfo 表。

**源码依据：** `lgishim_cairo.c:118-122`（cairo 侧）、`lgishim_pango.c:84-88`（pango 侧）。

### 2、destroy 回调必须与真实销毁 API 匹配

**约定：** 五句话口诀：
- Cairo：`cairo_xxx_destroy()` → `d_ctx` / `d_surface` / `d_pattern` / `d_font_options` / `d_region`。
- Pango GObject（Layout / FontMap / Context / AttrList）：`g_object_unref()` → `d_unref`。
- Pango GBoxed（FontDescription / FontMetrics）：它们**不是 GObject**，必须用自己的 free/unref，不能传 `g_object_unref`。
- **借用指针**（`cairo_get_source` / `cairo_get_target` / `pango_context_get_font_description`）：用 `d_noop`，不可调 destroy。
- 堆分配的小结构（Matrix）：用 `malloc`，destroy 是 `free`（`d_matrix`）。

**源码依据：** `lgishim_cairo.c:126-132`（七个 destroy 函数 + d_noop）、`lgishim_pango.c:92-130`（pango 侧 destroy）、`record2.md §3.2 Bug 4–5`。

### 3、`getud` / `chk` / `push_obj` 三件套

**约定：** 每模块必须有这三个 helper：
- **`getud(L, idx)`**：`lua_touserdata(L, idx)` 的薄封装，返回 `wrap *`。
- **`chk(L, idx)`**：调 `getud`，NULL 则 `luaL_error` 报类型错，返回 `w->p`（裸 C 指针）。
- **`push_obj(L, ptr, destroy, cls)`**：`lua_newuserdata` → 填三个字段 → `luaL_setmetatable(L, CLS_NAMES[cls])` → return 1。**对象必须在 `luaL_setmetatable` 之前创建**（该宏展开含有 `lua_setmetatable(L, -2)`，已消费栈顶）。

**反例/教训：** 在 `push_obj` 的 `luaL_setmetatable` 之后再追加一个 `lua_setmetatable(L, -2)`——这会把 userdata 本身装成别人的 metatable，导致对象方法全部失效（Spike 2 Bug 2，`record1.md §9.4 #2`）。
**源码依据：** `lgishim_cairo.c:134-166`（三件套 + push_obj 注释解释为什么对象要在 metatable 之前创建）。

### 4、`_native` 跨绑定 unwrap

**约定：** `m_native` 必须返回 `lightuserdata`（`lua_pushlightuserdata(L, w->p)`），不是 userdata、不是 table、不是整数。lib/ 侧 10 处 `._native` 调用（`surface.lua` / `wibox/init.lua` 等）期望拿到的是一个可直接塞给跨模块 C API 的裸指针。

### 5、类型化 checker（`chk_surface` / `chk_pattern` 等）

**约定：** 除了通用 `chk`（只检查 `w->p != NULL`），还要为关键类型写带类型校验的 checker：`chk_surface`（确保 `w->cls == C_SURFACE`）、`chk_pattern`（确保 `w->cls == C_PATTERN`）、`chk_matrix`、`chk_region`。跨模块桥接函数（如 `show_layout` 需要 Layout 对象）则用 `lgishim_pango_check_layout()` 做 metatable 身份校验（不靠 `w->cls`，原因同 §四·3）。

**源码依据：** `lgishim_cairo.c:670-692`（`chk_surface` / `chk_pattern` / `chk_matrix` / `chk_region` 四个函数）、`lgishim_pango.c:155-171`（`lgishim_pango_check_layout` 用 metatable 身份校验）。

---

## 四、metatable 三张表的分工

### 1、三张表与其查找顺序

**约定：** 每个类的 clsinfo 表（存在 `REG_CLASSES[cls]`）包含三张子表：

| 子表 key | 用途 | 查找时机 |
|----------|------|---------|
| `"g"` | 属性 **g**etter（`cr.matrix` / `cr.font_size`） | **最先**，命中后 `lua_call(getter, obj)` |
| `"m"` | **m**ethods（`cr:move_to()` / `cr:fill()`） | getter 未命中 |
| `"p"` | 属性 **p**setter（`pattern.set_filter()` / `pattern.set_extend()`） | 方法表也未命中 |

查找结束后还有一轮 fallback：直接查 metatable 自己的字段（`_native` / `finish`）。
**源码依据：** `lgishim_cairo.c:380-441`（`m_index` 完整四段查找）、`lgishim_cairo.c:629-642`（`class_setup` 里的 clsinfo 表构造）。

### 2、属性表遮蔽方法表——关键优先级规则

**约定：** 凡是 lib/ 用**冒号调用**的名字（如 `region:num_rectangles()`、`region:is_empty()`），**绝不能进属性表 `g`**，只能留在方法表 `m`。因为 getter 命中后 `lua_call` 返回结果值（一个整数/表），冒号调用就变成调这个结果值——`attempt to call a number value`。

**反例/教训：** `record2.md §4.3.3`：`num_rectangles` 和 `is_empty` 原放 `REGION_PG`（属性 getter 表），导致 `wibox/drawable.lua:112-114` 冒号调用崩掉。
**源码依据：** `lgishim_cairo.c:418-421`（注释："Placed after m, so an explicit method in m always wins"）、`lgishim_cairo.c:2255` 附近（REGION_PG 注释说"num_rectangles / is_empty 只留 m"）。

### 3、`m_is_type_of` 用 `lua_rawequal` 比对 metatable 身份

**约定：** `is_type_of` 的 upvalue 必须是**本类的 metatable 表本身**（在 `make_class_table` 里通过 `luaL_getmetatable` 取回，用 `lua_pushcclosure` 捕获）。判定逻辑：对 slot 2 的 userdata 取 metatable → `lua_rawequal` 与 upvalue 比对。**绝不用 `w->cls` + `strcmp` 判定**。

**反例/教训：** `record2.md §2.2 #4`：cairo 和 pango 的 cls 编号四档全撞（`C_CTX=1` vs `C_ATTRLIST=1` …），`strcmp(CLS_NAMES[w->cls], ...)` 在跨模块场景下误判。另注意：lib/ 用**冒号**调 `is_type_of`（`cairo.Surface:is_type_of(image)`），slot 1 是类表不是 userdata，所以 `lua_getmetatable(L, 2)` 比 `getud(L, 1)` 正确。
**源码依据：** `lgishim_cairo.c:533-546`（`m_is_type_of`，注释解释为什么不用 `w->cls`）、`lgishim_cairo.c:568-575`（`make_class_table` 里捕获 metatable upvalue）。

### 4、`__gc` 与 `release` 的幂等保护

**约定：** 每个模块的 `release(wrap *w)` 必须在调 `destroy` 之前把 `w->p` 置 NULL，`m_gc` 只调 `release`、`m_finish` 也只调 `release`。这样 `finish()` 后 GC 再次触发时，`release` 看到 `w->p == NULL` 直接跳过——避免 double-free。

**反例/教训：** oocairo 无此保护；lib/ 侧有 `finish()` 后手动释放 + 自动 GC 的双重路径（`spike3-cairo-semantics.md` §1）。不做幂等保护则 GC 二次释放 abort。
**源码依据：** `lgishim_cairo.c:476-484`（`release` 先设 NULL 再调 destroy）、`lgishim_cairo.c:486-498`（`m_finish` 和 `m_gc` 都调 `release`）。

---

## 五、枚举常量约定

### 1、一律字符串，`install_enum` push string

**约定：** 枚举值**永远不是整数**。`install_enum` 用 `lua_pushstring`（不是 `lua_pushinteger`），使 `cairo.Operator.SOURCE == "SOURCE"`。这来自 lib/ 侧的真实用法：`assert(cr.status == "SUCCESS", ...)`（`lib/wibox/drawable.lua:166`）——直接字符串字面量比较。

**反例/教训：** Part 1 Agent 交付的版本 `install_enum` 用 `lua_pushinteger` 但 `enum_from` 用 `luaL_optstring`——两端不一致，`cr:set_operator(...)` 直接 "string expected, got number"（`record2.md §2.2 #2`）。
**源码依据：** `lgishim_cairo.c:365-376`（`install_enum` 用 `lua_pushstring`）、`lgishim_cairo.c:173-176`（注释解释 lib/ 的 `cr.status == "SUCCESS"`）。

### 2、`enum_from` / `enum_to` 双端字符串

**约定：** `enum_from(L, idx, table, n, defv)` 从 `luaL_optstring` 映射到 C int（setter 方向）。`enum_to(L, table, n, v)` 从 C int 映射回 `lua_pushstring`（getter 方向）。两端都是字符串语义，**中间转换才走 C int**。
**源码依据：** `lgishim_cairo.c:180-203`（`enum_from` / `enum_to` 实现）。

### 3、枚举名必须对照真头文件逐一核实，不许伪造

**约定：** 每个枚举表的名字必须与容器内 `/usr/include/cairo/cairo.h`（或对应头文件）里的 `CAIRO_XXX_YYY` 定义**完全一致**（去掉前缀、全大写）。**禁止凭记忆或猜测写枚举名**——Agent 写的 `SUBTRACT` / `HARDLIGHT` / `SOFTLIGHT` / `OUT_OVER` / `IN_OVER` / `CONTENT_NONE` 在真 cairo 里全不存在。

**反例/教训：** `record2.md §2.2 #1`：6 个伪造枚举名，`SATURATE` / `HARD_LIGHT` / `SOFT_LIGHT` / `ALPHA` 才是真名。
**源码依据：** `record2.md §2.4`（容器内 `/usr/include/cairo/cairo.h` 实测值表）。

### 4、枚举表不含构造函数（`cairo.Rectangle` 不是枚举）

**约定：** 有些类名看起来像枚举表（`cairo.Rectangle`、`cairo.RectangleInt`）但 lib/ 实际用作**构造函数**（`cairo.Rectangle{x, y, width, height}`，`wibox/drawable.lua:246` 等 4 处调用）。它们**不能进枚举表**——会被枚举表遮蔽。正确的做法是用 `make_call_table`（不建 metatable 的"裸表类"），返回普通 Lua table 而非 userdata。

**源码依据：** `lgishim_cairo.c:2085-2090`（`rectangle_new` 返回普通 table）、`lgishim_cairo.c:584-593`（`make_call_table` 无 metatable，不做类型判定）、`record2.md §2.2`（"cairo.Rectangle 刻意不装成枚举表"）。

### 5、Format 表混合函数与常量

**约定：** 有些表既需要枚举常量又需要挂函数。`cairo.Format` 需要 7 个字符串常量（`ARGB32`、`RGB24` …）**外加**一个静态函数 `Format.stride_for_width`（`lib/naughty/dbus.lua:94` 调用）。用 `install_enum` 建表后再 `lua_pushcfunction` / `lua_setfield` 追加函数。

**源码依据：** `lgishim_cairo.c:2295-2307`（`format_stride_for_width` 实现）、`lgishim_cairo.c:2329` 附近（`luaopen_lgishim_cairo` 里先 `install_enum` 再 `lua_setfield` 加 `stride_for_width`）。

---

## 六、所有权与生命周期

### 1、cairo 对象引用计数

**约定：** 每个 cairo API 的返回值归类为三档之一：**拥有所有权**（必须 destroy）、**借用**（不增 refcount，给 `d_noop`）、**堆拷贝**（用 `malloc` 分配，给 `free`）。判定不准就写探针 fork 独立进程验证（`record2.md §3.3` 的方法）。

**验证结论表（cairo 1.16.0）：**

| API | 所有权 | destroy 回调 |
|-----|--------|-------------|
| `cairo_get_source()` | 借用 | `d_noop` |
| `cairo_get_target()` | 借用 | `d_noop` |
| `cairo_pop_group()` | 拥有 | `d_pattern` |
| `cairo_get_font_options(cr, fo)` | 自己创建 fo 传进去 | `d_font_options` |

**反例/教训：** `record2.md §3.2 Bug 5`：`ctx_get_source` 和 `ctx_get_target` 误用 `d_pattern` / `d_surface`，GC 时对借用指针调了 destroy，refcount 多减一次触发 abort：`Assertion failed: CAIRO_REFERENCE_COUNT_HAS_REFERENCE`。

**源码依据：** `lgishim_cairo.c:1481-1489`（`ctx_get_source` 完整注释解释为何用 `d_noop`）、`lgishim_cairo.c:1578-1584`（`ctx_get_target` 同上）、`lgishim_cairo.c:1659-1662`（`ctx_pop_group` 用 `d_pattern`）。

### 2、Matrix 按指针返回的 use-after-free

**约定：** 任何返回对象（Matrix、Rectangle、Region …）需要**在堆上分配**，**绝不返回指向栈局部变量的指针**。用 `malloc` 做堆拷贝：`cairo_matrix_t *copy = malloc(sizeof(*copy)); *copy = *src;`，然后 `push_obj` 带 `d_matrix`（内部调 `free`）。

**反例/教训：** `record2.md §3.2 Bug 3`：四个 getter（`matrix_invert` / `matrix_mul` / `ctx_get_matrix` / `pattern_get_matrix`）声明了栈局部 `cairo_matrix_t m`，取 `&m` 包进 userdata。GC 释放时该栈帧早已归还，读到随机值（`xx = 4.94e-324`）。
**源码依据：** `lgishim_cairo.c:724-733`（`push_matrix`——堆拷贝助手）、`lgishim_cairo.c:1427-1433`（`ctx_get_matrix` 改用 `push_matrix`）。

### 3、Pango / cairo 所有权边界

**约定：** Pango 对象家族里，`FontDescription` 和 `FontMetrics` 是 GBoxed（独立 refcount），不是 GObject。写 destroy 前先在 GLib 文档里确认类型：GObject 走 `g_object_unref`，GBoxed 走各自的 `_free` / `_unref`。

**反例/教训：** Spike 2 Bug 4（`record1.md §9.4 #4`）：PangoFontDescription 传了 `d_unref`（内部 `g_object_unref`），GLib 报 `GLib-GObject-CRITICAL: g_object_unref: assertion 'G_IS_OBJECT (object)' failed`。
**源码依据：** `lgishim_pango.c:110-125`（`d_fontdesc` 调 `pango_font_description_free`，`d_metrics` 带 `#if` 分支 `pango_font_metrics_unref` / `_free`）。

### 4、禁止传 NULL 给不可空的 C API

**约定：** 某些 C API 的参数在头文件里声明为指针（暗示可为 NULL），但实际实现不做 NULL 检查、直接解引用会 SIGSEGV。`cairo_set_matrix(cr, NULL)` 就是典型——`cairo_matrix_multiply(&tmp, &cr->gstate->ctm, matrix)` 无条件解引用 `matrix`。此类 API 必须传显式值（`cairo_matrix_init_identity(&id)`），不能图省事传 NULL。

**反例/教训：** `record2.md §3.2 Bug 4`：`ctx_reset_matrix` 调 `cairo_set_matrix(cr, NULL)` 直接 SIGSEGV。
**源码依据：** `lgishim_cairo.c:1435-1444`（`ctx_reset_matrix` 传显式单位阵 + 注释说明 NULL 会崩）。

---

## 七、冒烟断言约定

### 1、最小 Lua 宿主 + 打印 `lua_tostring(L, -1)`

**约定：** 冒烟脚本必须是一个**最小 C 宿主**（静态链接 shim 的两个 `.c` + `-llua -lm`），不依赖 CMake/ninja/X11/Pango 初始化。`luaL_dofile` 失败时错误在栈上——宿主**必须 `fprintf(stderr, "%s\n", lua_tostring(L, -1))` 打印**，否则只看到 `exit=1`、连哪一行报错都不知道。

**反例/教训：** `record2.md §2.5 #5`：前两次调试 `luaL_dofile` 失败但无输出，直到加了 `lua_tostring` 才定位到 `block too big`。
**源码依据：** `docs/sam/scripts/10-spike4-cairo-smoke.sh`（冒烟宿主 C 源码内嵌于脚本）。

### 2、断言写法与回归守卫

**约定：** 断言**不验"非 nil"而验真实往返值**。例如 `set_operator("OVER")` → `cr.operator == "OVER"`（不验 `cr.operator ~= nil`）、`scale(2, 3)` → `cr.matrix.xx == 2`。每段加一行 `print("==", description, "==", n_checks, "items")`，最后一段是 `results: N checks, 0 failures`。

**回归守卫段（§20）：** 必须有一段**"真实 awesome 调用路径逐字复刻"**——不是跑完整的 awesome，而是在冒烟脚本里逐句复刻 `drawable.lua` 的脏区域裁剪、`placement.lua` 的 `fit_in_bounding`、`textbox.lua` 的 `draw()` 等关键路径。这是防止"shim 单元测试全绿但真实代码路径跑崩"的唯一方式。

**源码依据：** `record2.md §3.4`（306 项冒烟，14 section）、`record2.md §4.5`（424 项冒烟，新增 §15–§20）。

### 3、点号调方法会崩，一律冒号

**约定：** 冒烟里调用方法**一律用冒号**（`cr:move_to(3,4)` 不是 `cr.move_to(cr,3,4)`），因为 lib/ 真实代码也用冒号。点号调方法不会自动传 self，方法体内 `chk(L, 1)` 拿到的是第一个实参而非 userdata，直接 `luaL_error`。
**源码依据：** `record2.md §3.4`（冒烟矩阵里全部冒号调用）、`lgishim_cairo.c:141-146`（`chk` 第一行就校验 userdata，点号调用时 slot 1 不是 userdata 直接炸）。

### 4、probe-compile 独立脚本 + LUA_PATH

**约定：** 每次改 shim C 代码后，在跑完整 `ninja awesome` 之前，先跑一个**独立 probe-compile 脚本**——它硬编码 `build.ninja` 里的 FLAGS/INCLUDES，单文件编译 shim，立即发现语法错误和缺函数。**不要**从 `build.ninja` 里 `awk` 提参：容器 `sh` 的 awk 正则引号转义会炸。冒烟跑 Lua 时必须设 `LUA_PATH`：`LUA_PATH="$REPO/lib/?.lua;$REPO/lib/?/init.lua"`，否则 `require("vendor.lgishim")` 找不到文件。

**源码依据：** `record2.md §2.5 #3-4`（两个实操修正）、`docs/sam/scripts/09-spike4-cairo-probe-compile.sh`（probe-compile 脚本模板）。

---

## 八、命名与文件组织

### 1、`cls` 编号命名空间

**约定：** 每个模块有自己独立的 `enum { C_XXX = 1, ... }`，编号从 1 开始。**两个模块的编号可以重复**（cairo 的 `C_SURFACE=2` 和 pango 的 `C_LAYOUT=2`），因为 `m_index` 只访问自己模块的 `REG_CLASSES` 数组。编号只在模块内用于索引，身份判定交给 metatable 比对（见 §四·3）。

**源码依据：** `lgishim_cairo.c:96-104`（cairo `enum` 含 `C_CTX=1` … `C_REGION=6`）、`lgishim_pango.c:49-58`（pango `enum` 含 `C_ATTRLIST=1` … `C_METRICS=7`）。

### 2、`CLS_NAMES` 跨模块命名空间

**约定：** `CLS_NAMES` 的 metatable 名字**必须带模块前缀**以切断跨模块碰撞（cairo 用 `"lgishim.XXX"`，pango 用 `"lgishim_pango.XXX"`）。同时 `REG_CLASSES` 也要用不同 key（`"lgishim_classes"` vs `"lgishim_pango_classes"`），因为 `w->cls` 索引的是各自模块内的序号，两个模块序号撞车时必须走不同数组。

**反例/教训：** `record2.md §4.3.1`：两模块的 `CLS_NAMES` 都写了 `"lgishim.Context"`，`luaL_newmetatable` 按名复用，cairo 覆盖了 pango 的 metatable。只改名不切 `REG_CLASSES` 也不行：pango `C_CONTEXT=6` 与 cairo `C_REGION=6` 数值撞车。
**源码依据：** `lgishim_pango.c:60-82`（CLS_NAMES 全带 `lgishim_pango.` 前缀 + REG_CLASSES 改 `lgishim_pango_classes` + 注释完整解释两个维度）。

### 3、新类扩展步骤

**约定：** 新加一个 C 类的标准流程，按顺序：

1. 在模块 enum 里加 `C_XXX = N`，并在 `CLS_NAMES[N]` 注册 metatable 名。
2. 写 destroy 回调（确认是 GObject / GBoxed / cairo_xxx_destroy / d_noop 哪一档，见 §六·1）。
3. 写三件套照搬（`getud` / `chk` 不动，`push_obj` 传新 cls）。
4. 调 `class_setup`：传三个 `luaL_Reg` 数组（`m` 方法表 / `pg` 属性 getter / `p` 属性 setter）。**方法名不能进 `pg`**（见 §四·2）。
5. `class_setup` 后 `lua_setfield(L, -2, "ClassName")` 把类表挂到模块上。
6. 在 `luaopen_*` 函数末尾 **`return 1`** 把模块表留在栈顶（stack 契约）：`lua_createtable` 建模块表后就绪入栈，后续 `lua_setfield(L, -2, "ClassName")` / `lua_setfield(L, -2, "Method")` 会弹出刚 push 的值并写入索引，所以循环里每对 `push + setfield` 后栈高不变；`class_setup` 内部的 `lua_newtable` / `lua_setmetatable` / `lua_setfield` 同样成对消费。最后 `lua_getfield(L, -1, "ClassName")` 之类如果写了，必须紧接 `lua_setfield(L, -2, "ClassName")` 消费——**任何不弹回的中间 push 都会在末尾把模块表的引用泄漏到栈上，`require` 拿到的返回值就变成多值而不是单表**。

**反例/教训：** Gio shim 初版 `luaopen_lgishim_gio` 里 `lua_setfield(L, -2, "BusType")` 之后多了一个未消费的 `lua_createtable`，模块表变成 `(table, table)` 两值返回；`lgishim.lua` 的 `ok, Gio = pcall(require, ...)` 拿到 `(true, table1)`，把第一张空表包为 `Gio`，后续所有 `Gio.File.xxx` 都是 nil。修法：严格成对，末尾 `return 1` 前 `assert(lua_gettop(L) == 1, "luaopen stack unbalanced")`。
**源码依据：** `lgishim_gio.c:2321`（`luaopen_lgishim_gio` 完整实现）、`lgishim_glib.c:1501`（同类模式）、`record2.md 篇八`（Gio 空表事故的接线护栏修复）。

**源码依据：** `lgishim_cairo.c:599-650`（`class_setup` 完整流程，含 clsinfo 三张子表构造 + `make_class_table` 调用）。

### 4、C 段落注释风格

**约定：** 每个"区块"用一行 `/* ---- 区块描述 */`（45 个 `-`）。关键决策写在注释里（不依赖外部文档），让读 C 代码的人不跳文件就能理解。例如 `ctx_reset_matrix` 的注释："NOT cairo_set_matrix(cr, NULL): this cairo copies the matrix straight off the pointer…"。

**源码依据：** `lgishim_cairo.c:124`（`/* --------------------------------------------------------------- helpers */`）、`lgishim_cairo.c:1435-1437`（`ctx_reset_matrix` 的决策注释）。

### 5、`ninja` 不认新文件要先 `cmake .`

**约定：** 在 `CMakeLists.txt` 添加新的 `.c` 源文件后，**必须先 `cmake .` 重扫**，否则 `build.ninja` 里 0 处引用新文件，ninja 当成没事发生过直接 exit 0。

**反例/教训：** `record2.md §2.5 #1`：加完 `lgishim_cairo.c` 直接 `ninja`，看似成功但产物里没有 cairo 的代码。
**源码依据：** `record2.md §2.5`（构建与冒烟的实操修正第 1 条）。

### 6、`lgishim.lua` 加载器模式

**约定：** 新模块的加载采用探测回退模式（`lgishim.lua:17-38`）：先 `pcall(require, "lgishim_xxx")` 试探静态绑定模块，成功则组包返回；失败则 `require("lgi")` 透传。模块加载顺序由 luaa.c 的 `package.preload` 注册决定（pango → pangocairo → cairo），此后加载的模块不能覆盖先加载模块的 metatable（见 §八·2）。

每个 shim C 模块需要在 `luaa.c` 里注册 `luaopen_*` 入口，并在 `CMakeLists.txt` 的库表里追加 `.c` 文件。

> **⚠ 2026-09-24 校定（推翻了原文「空表就是安全的」叙述）：**
>
> 原文说「空模块（GLib / Gio / Rsvg / GObject）不能留 nil——必须设为空表 `{}`」。这条在 Part 1-5 阶段是**对的**，因为当时 GLib / Gio / Rsvg 在 `lib/` 里没有真实调用方，`{}` 只是「静默 nil 访问」而不会被触发。
>
> **但 Part 6 交付Gio 时就炸了**：Gio shim 编译进了 luaa.c 但忘了写 `package.preload`，被 `lgishim.lua` 的 `pcall` 回退成了 `{}`。所有 `Gio.File.xxx` 静默返回 nil，冒烟全绿（因为冒烟只检查自己的 preload，不检查 luaa.c 的），上线才崩。这就是 Part 6 的「空表静默成功」事故，见 `record2.md 篇八`。
>
> **新约定：**
> 1. **有真实 lib/ 调用方的模块**（Gio / GdkPixbuf / GLib / GObject / Cairo / Pango / PangoCairo）——必须要么交付完整 shim、要么留 nil 让它 loud-fail。**不能留 `{}`**，因为 `{}` 会静默把 `M.XXX.Handle.new()` 变成 `M.XXX.Handle` 返回 nil、再 `nil.new()` 崩——崩得早但崩得远，定位困难。更糟糕的是 `{}` 让「存在性检查」（`if M.XXX then`）静默通过。
> 2. **当前无 lib/ 调用方的模块**（如 Rsvg，`lib/naughty/naughty.c` 才用）——保留 `{}` 尚可接受，但**必须在模块顶部注释里说明「暂无调用方，一旦有人调就直接 loud-fail 而非 nil fallback」**，否则后来人以为 `{}` 是可维护的默认值。
> 3. **Rsvg 的特殊状态：** `lgishim.lua:35` 的 `Rsvg = {}` 是**待交付**，不是已交付的空表。一旦开始实现 rsvg 绑定，就要把 `{}` 改成 `ok and Rsvg or nil`。
>
> **两道接线护栏**（Part 6 事故的直接后果）：
> 1. 冒烟脚本 `:43-74`——`luaa.c` ↔ `lib/vendor/lgishim.lua` 双向 grep：每个 `luaopen_lgishim_*` extern 必须有对应 `package.preload` 条目，每个 preload 条目必须被 `lib/vendor/lgishim.lua` 里 require。
> 2. 冒烟 Lua `:176-185`——`check(next(lgi[name]) ~= nil)` 确保每个 shim 表非空。
> 详见 `spike4-phase2-02-smoke-harness.md §七`。

**源码依据：** `lgishim.lua:17-38`（探测回退逻辑）、`lgishim.lua:28-31`（原空表约定）、`lgishim.lua:35`（Rsvg 待交付的空表）、`spike4-phase2-02-smoke-harness.md:43-74` / `:176-185`（两道接线护栏）、`record2.md 篇八`（Gio 空表事故与护栏新增）。

### 7、`AWESOME_IGNORE_LGI=1`

**约定：** 完整 `ninja awesome` 构建时，cmake 的 `lgi-check` 会先跑并失败（容器无 lgi runtime）。必须 `AWESOME_IGNORE_LGI=1 ninja awesome` 绕过。这个环境变量由 `build-utils/lgi-check.c:57` 读取。

**源码依据：** `record2.md §2.5 #2`、`record2.md §3.6`（构建命令均为 `AWESOME_IGNORE_LGI=1 ninja awesome`）。

---

## 九、本次更新说明（2026-09-24 校定）

1. **§二 新增 §2.5**：`lua_pushstring(L, NULL)` 在 Lua 5.3 下会断言（不是 push nil）。GLib / Gio 属性 getter 必须**先判空再 push**，否则碰到返回 NULL 的字符串属性就崩。
2. **§八·3 步骤 6 展开**：把「`return 1` 把模块表留在栈顶」扩展为完整的 **`luaopen_*` 栈契约**——`lua_createtable` / `lua_setfield` 必须成对消费，末尾断言 `lua_gettop(L) == 1`。补一条 Gio shim 初版的多返回值 bug 教训。
3. **§八·6 推翻原文「空表就是安全的」叙述**：Part 6 交付 Gio 时，`{}` 让「冒烟只查自己 preload」的假设彻底失灵——`Gio.File.xxx` 静默返回 nil、冒烟全绿、上线才崩。新约定分成三档：
   - **有真实 lib/ 调用方的模块**（Gio / GdkPixbuf / GLib / GObject / Cairo / Pango / PangoCairo）：**不能留 `{}`**，要么交付完整 shim、要么留 nil 让它 loud-fail。
   - **当前无 lib/ 调用方的模块**（如 Rsvg）：保留 `{}` 尚可接受，但顶部注释必须声明「暂无调用方」。
   - **Rsvg 的特殊状态**：`lgishim.lua:35` 的 `Rsvg = {}` 是**待交付**，不是已交付。
4. **§八·6 新增两道接线护栏说明**（`record2.md 篇八` Gio 空表事故的直接后果）：冒烟脚本头部 `:43-74` 的 `luaa.c` ↔ `lgishim.lua` 双向 grep，冒烟 Lua `:176-185` 的 `next(lgi[name]) ~= nil` 非空检查。详见 `spike4-phase2-02-smoke-harness.md §七`。