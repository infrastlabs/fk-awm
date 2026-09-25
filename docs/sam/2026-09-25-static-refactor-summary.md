# fk-awe 静态化改造 · 总结分析与优化（2026-09-25）

> **性质**：会话问答记录（Maki 对话），主题「改造总结分析与优化」。
> 第一章/第二章为问答原文归档；第三章收录后续问答（somewm 查证 / lgi 桥接之辨 /
> 「静态绑定一把梭」之辨）；**第四章是 2026-09-26 的状态收口，当前真值以它为准**。
>
> **配套文件**：`docs/sam/2026-09-23-record1.md`（Phase 1 起步）· `2026-09-23-record2.md`（Phase 2 Part 1-13）
> · `2026-09-25-record3.md`（Phase 2 续三 + 遗留盘点，第五章与 12.x 是黑屏/prefix 定案全文）
> · `spike1~spike4-*.md`（各阶段探索）。
> 本文件不重复技术细节，只做**问与答原文归档**，细节引用上述文件。

---

## 一、Q&A 1：静态化改造总结

**Q：总结下 fk-awe 项目的静态化改造**

**A：**

### 结论先行

- **静态链接无硬门槛**（Spike 1 证明）：502 → 0 个未定义符号，238 个去重符号 100% 由现存 `.a` 提供，无一是 dlopen/dlsym/typelib。
- **最终产物**：`awesome` 9,597,256 B，`ldd` 报 "not a dynamic executable"，UPX `-9 --lzma` 压到 ~3.4 MB。
  （2026-09-26 重编后 9,621,832 B → 3,412,740 B，见第四章）
- **核心架构决策（Spike 2，用户拍板）**：方案 B —— 用 **shim 调真实 Pango/Cairo C API + 静态链接**，否决「返回固定值的稀疏 stub」。

### 三条主线

**1. lgi 替换 → shim 层（lgishim_*）**

lgi typelib 加载是运行期硬门槛之一，被手写 C shim 替换，覆盖：
`lgishim_glib.c` (1,696 行)、`lgishim_pango.c` (1,041)、`lgishim_cairo.c` (2,642)、`lgishim_gio.c` (2,543)、`lgishim_gobject.c` (667)、`lgishim_pixbuf.c` (1,078)。lib/ 里的 `require("lgi").cairo` 改指 `vendor.lgishim`。`AWESOME_IGNORE_LGI=1` 绕过 lgi-check。

**2. 静态链接工程**

- **必须 `-Wl,--start-group / --end-group` 包裹整个库表**（fontconfig ↔ freetype 循环依赖），否则永远剩 ~20 个未定义符号。
- 版本号后缀库必须 `-l:libxxx-1.0.a`（pango/pangocairo/pangoft2/pangoxft/pixman）；`-lz` 能过但真身在 `/lib/libz.a`；`libuuid.a` 由 util-linux-dev 提供，别写 stub。
- **真正静态开关**：`-DCMAKE_EXE_LINKER_FLAGS="-static -Wl,--as-needed -Wl,--strip-all"`（`AWESOME_STATIC_DEPS=ON` 只是拼 `.a` 名，不是静态开关；漏传会静默产出 13MB 动态 PIE）。step 2 强制 `file` 校验 "statically linked"。

**3. 出包流水线（`docs/sam/scripts/11-package-static-release.sh`，7 步）**

configure / build / checkpaths / upx / install / smoke / package，产物 `awm-static-musl-<ver>-<sha7>-<TS>.tar.gz`，包内 346 条目，prefix `/usr/local/static/awm`（2026-09-26 改名；旧名 `awesome-static-musl-*` / prefix `/usr/local/static/awesome` 作废，见第四章）。

### 冒烟口径的两次纠偏（重要）

1. **T2「只看进程活不活」是假通过**：修 pango UAF 前 rc=137 + 日志 89.5MB/8s（卡死刷屏）被误判为稳定；修完后 rc.lua 真跑起来才炸出 2 个真缺陷 → 补了 T2b（日志匹配 traceback/interval must be 直接 fail）。
2. **冒烟全绿 ≠ 真跑起来过**：默认 rc.lua 全流程 09-25 才首次执行；`screen.primary` 为 nil 时 `systray.lua` 整段短路，Xvfb 结构上测不到 `user_to_device_distance` shim 缺口。
3. 修完的缺陷：pango 单例被 `d_unref` 误回收（`pc_font_map_get_default` 改 `d_noop`，一行）；`lgishim_glib.c` 把 interval=0 当错误抛（真 glib 接受）→ shim 语义不一致。

### 当前状态（2026-09-25）

- **P0 全部已关**（spec shim 化、`screen.primary` 短路判据、`GLib.Timer` 三方法、`Surface:write_to_png`）；P2/P3 十项已复核处理；HEAD `8e1c99f6` 已推远端。
  ⚠️ **（2026-09-26 更新）** HEAD 已推进到 `ff0ddca6`，本轮之后的全部工作见第四章。
- **P1 六项遗留**：D-Bus 全栈、32 项 harness + Ubuntu glibc 重跑、Pango ≥1.50 分支、SVG 主题降级、step 5 收尾 chown、theme 路径。
  ⚠️ **（2026-09-26 新增两项）**：`cairo.Context:get_pixels` 未实现（10 个 `spec/surface/crop_surface` 用例仍失败）、`SvgSurface.create` / `ImageSurface.create` 仍为 nil。
- **已知缺陷（未开工，用户令「先不开工」）**：构建前缀烤死 —— `--prefix=/usr/local` 把 gdk-pixbuf `loaders.cache` 路径（图片全解码失败）、dbus socket 路径烤进 rodata；真机黑屏的根因待定（疑 C2：`beautiful.wallpaper` nil 静默 0×0）。
  ⚠️ **（2026-09-26 三条全部有下文）**：
  - 图片解码：`lgishim_pixbuf.c` 已改自解码 PNG/JPEG/XPM（`71e3aa7b`），**不再依赖 gdk-pixbuf loader**，`loaders.cache` 这条已经不成立；
  - dbus socket 路径：已定案为 libdbus 用 `${prefix}/var` 构建烤死，**唯一解是重编 libdbus 加 `--sysdir=/run/dbus`**，改 awesome 的 prefix 救不了它（仍未修，见第四章）；
  - 真机黑屏：**已定案**，是双根因 —— 12.8 的 `root.wallpaper()` 收 full userdata（`_native` 挪进 property getter，`cca0939d`）+ 12.8.12 的编译期前缀烤死。C2 现象成立但成因归错了（不是用户 rc.lua 的问题）。
- librsvg 缺失（无 `.a`），影响 menubar/Surface 图标，按决策不做。
  ⚠️ **（2026-09-26）** `imagebox.lua` 守卫已修（`1c0f142b`，检查 `mod.Handle` 真值而非空表），`M.Rsvg = {}` 现在是**安全空表**、PNG 回退生效。SVG 主题降级从「唯一未处理阻塞项」降为「P1 功能缺口」。

> **⚠️ 一句话总览**：第一/二章的问答写在 09-25 上午，09-25 下午到 09-26 的那一整轮
> （pixbuf 自解码、`_native` 修法、prefix 改名、出包终验）**没有落进这两章**。
> **当前真值以第四章为准**，别按这里的「当前状态」排期。

---

## 二、Q&A 2：vendor 层代码模块、接口与 Lua 桥接

**Q：vendor层的代码模块与代码量情况，各模块主要实现了哪些接口？如何做的lua桥接: 实现静态编译 避免lgi**

**A：**

### 1. 模块与代码量（`lib/vendor/`）

| 模块 | 文件 | 行数 | 替代的 lgi 命名空间 |
| --- | --- | --- | --- |
| GLib | `lgishim_glib.c` | 1,696 | `lgi.GLib` |
| Gio | `lgishim_gio.c` | 2,543 | `lgi.Gio` |
| Cairo | `lgishim_cairo.c` | 2,642 | `lgi.cairo` |
| Pango | `lgishim_pango.c` | 1,041 | `lgi.Pango` |
| GObject | `lgishim_gobject.c` | 667 | `lgi.GObject` |
| GdkPixbuf | `lgishim_pixbuf.c` | 1,078 | `lgi.GdkPixbuf` |
| 分发表 | `lgishim.lua` | 57 | — |
| **合计** | 6 C + 1 Lua | **9,667 + 57** | |

> 表内行数为 2026-09-26 实测（`wc -l`）。09-25 写第一/二章时是 9,036 + 55，
> 涨的部分：pixbuf +536（PNG/JPEG/XPM 自解码）、gio +27（`Gio.Async.start`）、
> pango/glib/cairo 各 +24 左右（`_native` 挪进 property getter）。本文件后文
> 引用的「9,036 行」都是 09-25 快照，用于说明手写成本量级，不代表当前代码量。

PangoCairo 没有单独文件，是 `lgishim_pango.c` 里的第二个入口 `luaopen_lgishim_pangocairo`（`font_map_get_default` 一个函数）。

### 2. 各模块接口面

- **glib**：6 个类 `Variant/KeyFile/DateTime/TimeZone/Timer/String`（Timer 五方法 new/elapsed/stop/continue/start 已接 `lgishim_glib.c:1621`）+ 模块函数 `timeout_add/source_remove/build_filenamev/get_home_dir/getenv/utf8_*` 等 + 枚举表 `Priority/Source/KeyFileFlags` + 平铺整数常量 `PRIORITY_DEFAULT/SOURCE_CONTINUE/SOURCE_REMOVE`（gears/timer.lua 直接读裸整数）。
- **cairo**：6 个类 `Context/Surface/Pattern/FontOptions/Matrix/Region`。Context 是全项目最高频：路径构造（move_to/line_to/curve_to/arc/rectangle）、绘制（fill/stroke/paint/mask）、状态（save/restore/translate/rotate/set_source_*）、文本（show_layout/text_extents）；Surface 含 `write_to_png`（P0）；Pattern 含颜色渐变全套；Region 含布尔运算 8 个。容器 cairo 是 pre-1.14 混血（见文件头 NOTE），所以 `cairo_get_font_size` 是唯一 "not implemented" 桩（`lgishim_cairo.c` 处 grep 命中）。
- **pango**：7 个类 `AttrList/Layout/FontDescription/Color/FontMap/Context/Metrics`。Layout ~20 方法（set/get_text、set/get_markup、wrap、ellipsize、justify、alignment、`get_pixel_extents`、`get_size/baseline/line_count`）+ 模块函数 `units_from_double/parse_markup`；Color 三入口（构造/parse/parse 类方法）；PangoCairo 仅 `font_map_get_default()`。
- **gio**：类最多。文件流 `File/FileInfo/FileEnum/DataInputStream/MemoryInputStream/UnixInputStream`（含 read_line 同步+异步）＋ D-Bus 全家桶 `DBusConnection/DBusProxy/DBusInvocation/DBusVariant/DBusArg/DBusMethod/DBusSignal/DBusInterface`（`bus_get_sync/bus_own_name/register_object/emit_signal`、XML introspection 生成、GVariant 编解码）+ 模块 `bus_get/bus_own_name/async_*`。
- **gobject**：`Closure/Object` 两个类，核心是 `closure_marshal`——把 GClosure 的 GValue 参数栈转成 Lua 值再调 Lua ref，这是所有 GObject 信号回调的通用桥。
- **pixbuf**：`Pixbuf` 类 + `new_from_file` + `get_from_surface`（含手写 surface→pixbuf 像素拷贝回退，因工具链无 `gdk_pixbuf_get_from_surface`）。**`new_from_file` 已改 magic-byte 分发 + 自解码**（`71e3aa7b`）：PNG 走 libpng 简化 API、JPEG 走 libjpeg、XPM 走自写解析器，未知格式回退 `gdk_pixbuf_new_from_file`——不再依赖 gdk-pixbuf loader 插件，这是静态构建里图片能出来的唯一原因。

### 3. Lua 桥接机制（如何绕开 lgi）

三层设计，核心是**把 binding 直接编译进二进制 + package.preload 注册 + 类注册表驱动的动态分发**：

**a) 静态注册，杜绝 dlopen**（`luaa.c:70-76, 1141-1160`）

`luaa.c` 用 `extern` 声明全部 `luaopen_lgishim_*`，在 `luaA_luaopen` 里把每个 C 函数 `lua_pushcfunction` 挂进 `package.preload`。这些符号直接静态链接进 awesome 二进制——没有 `.so`、没有 typelib、没有 gobject-introspection 运行时，这就是"静态编译避免 lgi"的实现点。

**b) 自动降级选择**（`lgishim.lua`）

`pcall(require, "lgishim_pango")`：静态构建下 preload 命中 → 组装 `M = {Pango, PangoCairo, Cairo, GdkPixbuf, GLib, Gio, GObject, Rsvg={}}`，置 `M.shim = true`；普通构建 preload 缺失 → `M = require("lgi")` 走真 lgi，行为保持不变。并补了代码库实际用的小写别名：`M.cairo/M.gdkpixbuf/M.gio/M.Gdk`，`M.GdkPixbuf.pixbuf = M.GdkPixbuf.Pixbuf`（menubar/utils.lua 等找 `pixbuf`）。

**c) 对象模型：wrap userdata + 注册表类表**

每个类一个 `C_*` 枚举，对象包成 `wrap {void *p; void (*destroy)(void*); int cls}` 的 userdata，metatable 装 5 个统一入口：

- `__index`（`m_index`）：按 `w->cls` 从 `LUA_REGISTRYINDEX` 的类注册表取出该类表，依次查 **g**（getter，找到就 `lua_call` 带上 self 执行）/ **m**（方法，返回函数）/ **p**（属性 setter），最后回退 metatable 字段——一个 userdata 一种分发逻辑，全模块复用。
- `__newindex`（`m_newindex`）：只认 `p` 表里的 setter（如 `layout._private.width = X`），否则 `luaL_error`。
- `__gc`/`finish`：调 `wrap->destroy`（`d_unref/d_gfree/d_noop/d_variant`…）按类型释放；`release` 后置 `d_noop` 防双重释放。
- `_native`：返回裸 C 指针 lightuserdata，供跨 shim 传参（如 cairo.c 的 `lgishim_cairo_check_surface` 直接取 pango.c 的 layout）。
- 类表本身 `__call`（`class_call`）→ 转调 `new`，所以 `Pango.Color(...)`、`Pango.Layout.new(ctx)` 语义一致；`is_type_of` 用 metatable 相等比较。

**d) 枚举与常量**：`install_enum` 按名字挂表（`glib.Priority.HIGH`），同时把代码库直接读的裸整数平铺（`glib.PRIORITY_DEFAULT=0`）；`timeout_add` 缺省补 `G_PRIORITY_DEFAULT`、`timeout_cb` 把 nil 返回当 `G_SOURCE_CONTINUE`，保证 timer 即使漏引常量也不崩。

**e) 回调桥**：glib 侧 `cbdata{L, ref}` 存 Lua 函数引用，`timeout_cb` 从 C 回调里重新进 Lua 栈调用；gobject 侧 `closure_marshal` 做 GValue↔Lua 双向转换，`push_gvalue` 支持 bool/int/double/string/object；gio 的 D-Bus 回调（method_call、bus_acquired 等）同一套路。

### 4. 编译接入

`CMakeLists.txt:127-138`：`AWESOME_LGISHIM` option（默认 OFF，静态构建 ON），6 个 `.c` 直接 `list(APPEND AWE_SRCS ...)` 编进 awesome 本体，`pkg_check_modules` 只取 pango/pangocairo 头文件路径。注意两点：这个开关**不是**静态开关（静态靠 `-DCMAKE_EXE_LINKER_FLAGS="-static ..."`），它只是把 binding 链进二进制；以及 pkg prefix 变量名必须带 `_REQUIRED`（`LGISHIM_PANGO_REQUIRED_INCLUDE_DIRS`，commit `5e4ba79b` 修过）。

**一句话总结**：`luaa.c` 静态注册 + `lgishim.lua` 自动降级 + 统一的 wrap/注册表分发，把 lgi 的运行时反射（typelib）替换成编译期写死的 C 绑定，从而静态链接出无动态依赖的 awesome 二进制。

---

## 三、优化内容（等待后续录入）

> **状态：待录入。** 本章预留，后续讨论的优化项在此追加，每项建议含：动机 / 方案 / 影响面 / 是否开工（用户指令）。
>
> **已归档的问答明细见子节 3.1 / 3.2 / 3.3 / 3.4 / 3.5**（somewm 查证 + lgi 桥接机制 + 静态绑定更优方法 + Lua 绑定层之辨），可供后续优化项引用；优化条目本身仍在下方等待录入。

### 3.1 问答：查证 somewm 项目（剔除 lgi 之辨）

**Q：查gh 看somewm项目，可以克隆到本地，详细分析:它是怎么剔除lgi依赖的，他那边的实现与当前这边项目对比有什么异同？有哪些是可以从那边借鉴/优化的？**
**Q（用户澄清）：是有个somewm的项目，面向wayland的**

**A：**

#### somewm 是什么

- **`trip-zip/somewm`**（★391，Lua）：AwesomeWM 的 **Wayland 移植版**（wlroots 合成器，非 X11），2026 年活跃，已移植上游大量 PR（见 `UPSTREAM_PORTS.md`，baseline `fa805ab4`）。
- Lua 层在 `lua/` 目录（不是 lib/），结构沿用 awesome：wibox/gears/awful/beautiful/naughty/menubar 全套。

#### 事实澄清：它没有剔除 lgi（证据）

| 证据 | 位置 |
| --- | --- |
| `lua/` 下 **36 个文件**仍 `require("lgi")`，用法与上游完全一致（`.cairo`/`.GLib`/`.Gio`/`.GdkPixbuf`/裸 `lgi`） | `grep -rln "require(\"lgi" lua/` → 36 |
| nix 包硬依赖 `[lgi ldbus]` | `package.nix:42` |
| meson 里 lgi-check **失败即 error**（硬依赖，无降级开关） | `meson.build:118-129` |
| `lgi_closure_guard.c` 存在的前提就是 lgi 还在（拦截 libffi 闭包） | 文件头注释 |
| `luaa.c:1633` 启动时 `require('lgi.version')` 并检查版本 | — |

它移植了上游的 `lgi-check.c`（shell→C），但定位是**检测版本**，不是绕过。

#### 它真正做的事：围绕 lgi 的"驯服工程"（可借鉴部分）

somewm 作为 Wayland 合成器（自身就是 Wayland server），lgi 的 GTK/GDK 用法会**连自己的 display 死锁**，所以它做了五件事：

1. **GTK 死锁规避**（`luaa.c:3010`）：`package.loaded['lgi.override.Gtk'] = {}` —— 阻止 `gtk_init_check()` 连 `WAYLAND_DISPLAY`（合成器事件循环被 Lua 加载阻塞，双向死锁）。
2. **配置静态检查**（`luaa.c:4873` 附近）：扫描用户 rc.lua，`lgi.require("Gtk")` 警告、`lgi.require('Gdk` **致命**（GDK 连显示服务 = 挂死），还有 xdotool/xprop 等 X11 命令检查。
3. **lgi_closure_guard**（`lgi_closure_guard.c`）：LD_PRELOAD 拦截 `ffi_prep_closure_loc()`，给 lgi 闭包打代际号，热重载时旧闭包变 no-op 防 SEGV。自述"canary, not a safety net"。
4. **GLib source sweep**（`luaa.c:4860`）：状态销毁时清掉该状态遗留的全部 GLib sources，防泄漏后调度。
5. **Lua 5.4+/版本兼容**：继承上游对 `--version` 输出、`GioUnix` 命名空间等的修复。

#### 与 fk-awesomewm 的对比

| 维度 | fk-awesomewm（本 fork） | somewm |
| --- | --- | --- |
| 后端 | X11 | Wayland (wlroots) |
| lgi 策略 | **釜底抽薪**：lgishim C 绑定静态链接，**无 lgi 运行时** | **保留+防御**：lgi 仍是硬依赖，靠 guard/检查驯服 |
| 绑定实现 | 6 个手写 C shim（9,036 行）覆盖 Pango/Cairo/Glib/Gio/GObject/Pixbuf | 无自写绑定（luaopen 注册 0 处），全靠 lgi |
| 二进制 | 全静态、无动态依赖、UPX ~3.4MB | 动态依赖（含 lgi .so + typelib） |
| 运行时风险 | 无 libffi 闭包悬垂/typelib 兼容问题 | 有（闭包 guard 只能当 canary） |
| lgi 降级 | `lgishim.lua` 自动降级：静态 build 用 shim、动态 build 回退真 lgi | 无（lgi-check 失败即 error） |
| 用户配置兼容 | 用户 rc.lua 里 `lgi.Pango` 等经 shim 分发表仍可解析（`M.Pango` 同表） | 完整 lgi 语义，但需防 Gtk/Gdk 死锁 |
| 热重载 | 无此需求（单状态） | 有 hot-reload + closure guard/source sweep |

核心差异一句话：**somewm 解决的是"lgi 在合成器进程里不死锁不崩"，我们解决的是"lgi 根本不存在"**——后者彻底，前者保留生态兼容。

#### 可借鉴 / 优化点（对我们 fork）

1. **配置静态检查器**（最值得借鉴）——借鉴 somewm 的 rc.lua 扫描思路，换成我们的场景：启动时检测用户配置用到但 shim **未覆盖**的 lgi API（如 `user_to_device_distance`、`new_sub_path`），给出明确错误而非静默 nil。somewm 管"危险用法"，我们管"覆盖缺口"，同一机制。
2. **beautiful/gtk.lua 的 shim 下行为**——确认我们 fork 中该文件（上游用 `lgi` 查 GtkStyleContext）在 shim 下走什么路径。我们的 `lgishim.lua` 没有 Gtk 命名空间，用户配置触碰时是报错还是静默，需要像 somewm 一样有明确策略（X11 下无死锁，但语义要定义清楚）。
3. **lgi-check.c 复用**——上游/somewm 的 C 版 lgi-check 已成熟，可直接对齐我们的 `AWESOME_IGNORE_LGI` 检测逻辑。
4. **上游 PR 移植跟踪表**——`UPSTREAM_PORTS.md` 的做法值得复制：我们 fork 相对上游基线偏离很多（lgishim 改造），维护一张"上游最新修复是否已纳入"的表，避免 shim 修复与上游 Lua 层修复互踩。
5. **版本检查保留**——somewm 在 `gears/surface.lua` 保留 lgi ≥0.8.0 检查；我们 fork 的 `surface.lua` 已改成 `vendor.lgishim.version`，确认该检查在我们 shim 下仍然有意义的（防老 typelib 场景）。

### 3.2 问答：somewm 静态化编译查证（结论修正）

**Q：之前查证过，说是他可以直接做静态化编译的。**

**A（结论：somewm 不支持静态化编译）：**

#### 反证：somewm 无静态化能力

| 证据 | 说明 |
| --- | --- |
| PR #299（已合并）"**reject static system wlroots** to prevent stale .a breaking rebuild" | 主动**拒绝**静态 wlroots——反而视静态库为重建隐患 |
| issue #651 "**musl support: dlvsym isn't available in musl**"（open） | 连 musl 都还没支持，遑论全静态 |
| issue #743（open）"Intermittent startup panic: 'attempt to call a number value' **from lgi**" | 至今仍被 lgi 运行时问题困扰 |
| CI 测试要 `apt-get source lua-lgi` 自己编译 lgi 0.9.2（`ci.yml:158-165`） | lgi 是硬依赖，无替代 |
| `meson.build:118-129` lgi-check 失败即 `error()` | 硬门槛，失败就停止配置 |

#### "可以静态化编译"的误解来源（三个陷阱）

1. **`default_library=static`**（`meson.build:189`）：只作用于 **wlroots subproject**（系统找不到 wlroots 时从源码编成静态库），somewm 本体对 wlroots 的依赖是显式的 `static: false`（`meson.build:160,180`）。wlroots 静态 ≠ somewm 静态。
2. **`SOMEWM_IGNORE_LGI=1`**：只跳过 **lgi-check 检测**（`lgi-check.c:63`，失败时退出 0），运行时照样 `require("lgi")`——与我们 fork 的 `AWESOME_IGNORE_LGI` 同款语义。Gentoo overlay 用它是因为 ebuild 构建环境里 lgi 检测会挂，不代表运行时不需要。
3. **"AwesomeWM-compatible" 宣传**：README 强调 Lua API 兼容（"existing configs carry over"），可能被引申为"连 lgi 依赖都能带走"——但它的 36 个 Lua 文件照样 `require("lgi")`。

#### 对照：真正的静态化只有我们这条线

- **fk-awesomewm**：lgishim 6 模块 9,036 行 C 静态绑定 → 无 lgi/typelib/libffi → `-static` 全静态链接（musl）→ UPX ~3.4MB 单二进制。
- **somewm**：保留 lgi，靠 `lgi_closure_guard`（LD_PRELOAD）+ GTK 死锁规避 + GLib source sweep 驯服它——**是"防御 lgi"，不是"剔除 lgi"**。

> 备注：若"somewm 可直接静态化编译"有具体出处（URL/文档/帖子），需另行复核；就仓库代码而言该说法不成立。

### 3.3 问答：lgi 的代理桥接机制与绑定性

**Q：模块与代码量（lib/vendor/），这下面所桥接的这些模块，原来lgi项目，他是如何代理桥接的呢？他与这些模块的具体的接口是绑定的吗？接口的增删改，他这边的项目是否也需要调整？**

**A：**

#### lgi 是怎么"代理桥接"这些模块的

**它不是绑定，是运行时反射。** lgi 只有一套**通用 GI 引擎**，没有任何 per-module 绑定代码：

- **C 核心**（`lgi/core.c`、`gi.c`、`callable.c`、`marshal.c`、`object.c`、`record.c`、`buffer.c`）——通用机制，与具体库无关。
- **Lua 层**（`core.lua`、`namespace.lua`、`class.lua`、`record.lua`、`ffi.lua`…）——同上的通用逻辑。

工作流程（`require("lgi")` → `lgi.Pango`）：

1. 首次访问命名空间 → `namespace.lua` 触发 `core.gi.require('Pango', ...)` → `gi.c:704` 的 `g_irepository_require()` 从 `GI_TYPELIB_PATH` 加载 **`Pango-1.0.typelib`**。
2. **typelib 是库构建时 g-ir-scanner 从 C 头文件 + annotations 自动生成的二进制接口描述**——函数签名、参数、transfer 语义、类层次、信号全在里面。
3. 调用时 `gi.c:630` `g_typelib_symbol()` 在运行时解析函数地址，`callable.c` 用 **libffi** 把 Lua 调用动态 marshal 成 C 调用；对象走 GType，record 走 record.c，enum 用 `ffi.load_enum`。

**override/ 目录（每库一个小 Lua 文件）不是绑定主体，是修补层**：

| override | 行数 | 补什么 |
| --- | --- | --- |
| `cairo.lua` | 923 | cairo 非 GObject 库（无 GType），**大量手工 marshal**——最大的例外 |
| `Gtk.lua` | 660 | 信号/特性 |
| `Pango.lua` | 181 | GIR 里没有的常量（`SCALE_*` 手补）、非 gtype enum 重载、`set_text` 默认 len=-1 |
| `GLib-Timer.lua` | 34 | C 结构包装成 Lua 类型 |
| `GLib-Bytes.lua` | 22 | 同上 |

#### 绑定性：lgi 与具体接口不是编译期绑定

**接口增删改，lgi 绝大多数情况不需要改任何代码**——这是 GObject-Introspection 的设计核心：库更新后重新生成 typelib，lgi 自动拿到新类/新方法/新信号。**只需要改的 4 类例外**：

1. **非 GObject 库**（cairo）：不走自动生成，`cairo.lua` 923 行手写 marshal，cairo API 一变它就要跟着改。
2. **语义/annotation 变化**：库改了 transfer、加了复杂回调/vfunc 参数 → 要更新 override。
3. **GIR 描述不了的**：常量/宏/非 gtype enum（如 Pango `SCALE_XX_SMALL` 是手工补的）→ 库变化时 override 可能不同步。
4. **lgi 自身兼容**：Lua 5.4 的 `ffi.ref` 问题、GIRepository 2.80 迁移（我们 fork 的 `gears/surface.lua` 还保留了 `lgi >= 0.8.0` 版本检查，就是怕 override/行为漂移）。

#### 对我们 lgishim 的对照（直接回答"接口增删改是否要调整"）

**要，而且是硬性人工成本。** lgishim 是**编译期静态绑定**——每个函数在 C 里手写 `luaL_Reg` 注册表、参数转换、语义处理（9,036 行就是这么来的）。对比：

| 维度 | lgi | lgishim（本 fork） |
| --- | --- | --- |
| 绑定方式 | 运行时 typelib 反射 + libffi | 编译期写死 C 注册表 |
| 库接口增删改 | typelib 自动跟随，lgi 零改动 | **必须改 C 代码 + 重新编译 + 重新出包** |
| 覆盖完整性 | 理论 100%（typelib 全量） | 人工枚举，漏一个就是一个运行时 nil（审计的 6 个缺口就是这么来的） |
| 依赖 | GIR + typelib + libffi + 动态库 | 无（静态链接） |
| 维护触发点 | 仅 override 特例 | 每次上游库升级/上游 awesome 新增 lgi 用法 |

一句话：**lgi 是"接口跟着 typelib 走"，lgishim 是"接口跟着我们的 C 代码走"**。lgi 用运行时反射换维护零成本，代价是无法静态化；我们用手写绑定换静态化，代价是接口增删改必须人工同步 shim——这正是 audit 2 里 6 个确定缺口（`user_to_device_distance`、`new_sub_path` 等）和"上游新增 lgi 用法要盯"的根本原因。

### 3.4 问答：静态编译下避免"接口一一对应绑死"的更优方法

**Q：那对标他这个模式，静态编译的场景下面有没有更优的方法，能够避免 vendor层与接口一一对应 强制绑死呢，当前如接口有变动，就得频繁的调整vendor层**

**A：**

先排除一个最直觉但**不可行**的方案：**"把 typelib 静态塞进二进制"行不通**。

最自然的想法是：保留 lgi，把 typelib 文件 embed 进二进制、libffi/girepository 静态链，就既能静态化又能自动跟接口。**硬伤在符号解析**：lgi 调用任何函数都走 `gi.c:630` 的 `g_typelib_symbol()`，它本质是 `dlsym`/动态符号表查找。**静态链接的二进制没有动态符号表**，函数地址根本解析不到（除非 `--export-dynamic` 自注册符号表，那就等于把 gi.c 重写一遍）。lgi 的整个架构建立在这个运行时反射之上，静态化 = 重写核心，不如写自己的。

#### 正确的模式翻译：把"运行时反射"变成"构建时反射"

lgi 的三层架构本身是对的——**通用引擎 + 接口描述（typelib）+ 特例修补（override）**。问题是它在运行时读接口描述。静态场景下可以平移成：

| lgi 层 | 静态化等价物 |
| --- | --- |
| 通用 marshal 引擎（core.c/callable.c） | **一个通用 C dispatcher**（针对我们支持的参数类型子集，一次性写死） |
| typelib（g-ir-scanner 生成的接口描述） | **构建时生成器产出的元数据表**（编译期固化进二进制） |
| override/*.lua（特例修补） | **特例表**（transfer/回调/GC 的手工决策，集中在一处，量级大减） |

具体到当前 fork 的可落地路径，按激进程度分三档：

**档位 A：生成式 shim（低风险，保留现架构）**

写一个生成器，输入 GIR XML（或直接调 `g-ir-scanner` 扫库头文件）+ awesome lib/ 的实际调用清单，**输出现有的 lgishim C 注册表代码**（`luaL_Reg` 表、参数出入栈样板全部自动生成）。现在 9,036 行里约七成是机械样板——`cairo.Context:xxx` 每个 10~30 行的样板代码全是可生成的。人工只留语义特例。
- 接口变动：重新生成 → 编译，不再手写。
- 风险：不动运行时架构，只是把"手写"变"生成"，马上能用。

**档位 B：元数据表 + 通用 dispatcher（彻底，接近"静态 typelib"）**

不是每个接口一个 C 函数，而是每个接口一行元数据（`{name, ret_type, arg_types, transfer, flags}`）进表，一个通用 dispatcher 按表 marshal。这就是 lgi 的静态版。接口增删改 = 改元数据表（生成器产出）+ 重新编译。
- 好处：新增接口的成本降到"元数据一行"（或全自动），语义特例仍走特例表。
- 成本：dispatcher 要覆盖 awesome 实际用到的参数类型子集（audit 过：cairo 指针/数字/矩形结构体、pango layout、glib timer/variant、gio 文件/dbus——类型面其实很窄）。

**档位 C：覆盖断言 CI（无论 A/B 都要配）**

核心痛点其实是"接口变动了**我不知道**"。加一个构建期/CI 检查：自动扫描上游 lib/ 的 `lgi.*` 调用 vs shim 元数据覆盖，有缺口直接编译失败或开 issue——把"运行时炸 nil"提前到"构建时报错"。这才是治"频繁调整"的根：**变动变成显式的、可追踪的，而不是漏网的**。

#### 现实性评估

awesome 对 lgi 的调用面很小且类型单调（Pango 7 处调用点、GLib 主要是 timer/variant/keyfile、Gio 是 file/dbus、cairo 是 Context 路径绘制），不是全量 GI 库绑定——**生成器 + 子集收敛完全可行**。参照物：lgi 自己的 `cairo.lua`（923 行）和上游的 gir 绑定项目（如 lua-gtk）都证明了 gir→代码生成的成熟度。

#### 建议落地顺序

**先做 C（覆盖断言），再做 A（生成式），B 视维护频率再定。** 理由：
1. C 成本最低（一个扫描脚本），立刻止血"不知道哪里漏了"；
2. A 把存量 9,036 行机械样板消灭，接口变动从"改 C"降为"重新生成"；
3. B 是终极形态但重构风险大，等 A 跑顺、确认维护成本仍然高再动。

### 3.5 问答：Lua 为何需要 vendor 绑定层？能否"一把梭哈"全链入

**Q：lua为啥要vendor绑定层呢？其他的c程序则不要 直接静态链接编译就可以了。另外，能否把依赖库的所有接口都链接进来，一把梭哈，lua自行按需使用？**

**A：**

#### 为什么 Lua 必须有绑定层，其他 C 程序不需要

**因为 Lua 虚拟机只能调用一种签名的函数：`int (*)(lua_State*)`（lua_CFunction），参数全部走 Lua 栈。** 而 libcairo/libpango 的 C 函数签名是 `cairo_status_t cairo_rectangle(cairo_t*, double, double, double, double)`——完全对不上。

对比两种调用路径：

```
C 程序：      cairo_set_source_rgb(cr, 1, 0, 0);
              编译期：头文件给了签名 → 编译器生成直接 call → 链接器从 .a 里拉符号 → 运行时直接跳转
              （不需要任何胶水）

Lua 程序：    cr:set_source_rgb(1, 0, 0)
              运行时：Lua VM 只认识"名字"→ 必须有人提前把 set_source_rgb 注册成 lua_CFunction
              → 这个注册函数负责：从栈里取 cr/userdata + 3 个 number → 转成 C 类型 → 调真实 cairo
              → 再把返回值压回栈。这层胶水就是绑定（lgi 动态生成 / lgishim 手写）
```

**胶水层缺一不可，区别只在谁写**：lgi 用 libffi 自动生成（运行时反射），lgishim 用手写（编译期固化）。其他 C 程序不需要，是因为**它就是 C**——签名在编译期就确定了。Lua 是解释执行的字符串，编译期不知道脚本会调什么，签名转换和语义信息必须靠一个中间层。

#### "一把梭哈：所有接口链进来，Lua 自行按需使用"——拆两层看

**链接层：符号全链可行，但现在没这么做（也不需要）。**
静态链接器的规则是：**只把被引用的 .o 从 .a 里拉进来**。awesome 的 C 层引用了部分 cairo/pango 符号；但 Lua 层要用的 `pango_layout_new`、`cairo_pattern_add_color_stop_rgba` 等，C 层没引用——**lgishim 里每一行"调用真实 C API"的代码，本质就是在制造符号引用，让链接器把那个 .o 拉进二进制**。要全链也行：`-Wl,--whole-archive` 强制拉整个 .a，代价是二进制膨胀（所有代码段都进去，哪怕没用）。

**调用层：这才是"一把梭"的真正障碍——符号表只有"名字→地址"，没有"签名→类型"。**

就算把所有符号都链进二进制，Lua 还是不会用，因为调用一个 C 函数还差三类信息，符号表里**全都没有**：

| 缺的信息 | 例子 | 在哪儿 |
| --- | --- | --- |
| 参数/返回值的类型与转换 | `set_source_rgb` 的第 2 个参数是 double 还是 int？ | 头文件/GIR annotation |
| 所有权/生命周期 | `pattern_get_surface` 返回的指针是 borrowed 还是 owned？谁负责 unref？ | GIR transfer annotation |
| 对象模型 | 哪些函数是方法（第一参数是 self）？哪些是构造器？ | GIR 元数据 |

- 动态方案里，这三类信息在 **typelib**（GIR 编译产物）里，lgi 运行时读。
- 静态方案里，这三类信息只能**编译期固化**——要么手写（现在的 lgishim），要么生成（3.4 讨论的元数据表）。

所以"链进来"≠"能用"：**还差一张"签名+语义"描述表**。这正是为什么不能真·一把梭。

#### "一把梭"最接近的可行形态 = 档位 B 的完整版（静态 lgi）

1. **符号全链**：`--whole-archive` 或生成引用桩（一行 `(void)cairo_xxx;` 就能拉进一个 .o），把 cairo/pango/glib/gio/gobject/gdkpixbuf 的**全部**符号固化进二进制——这部分真能"梭"。
2. **全量元数据表**：从 GIR XML 一次性生成**所有接口**的元数据（不做 awesome 子集裁剪——反正是一次性生成，多不了多少维护量）：`{名字, 所属类, 参数类型列表, 返回类型, transfer, 是否构造器/方法}`。
3. **通用 dispatcher**：一个 lua_CFunction 按元数据表 marshal 所有调用——`__index` 查表 → 按表取参 → 调真实符号 → 按表回栈。
4. **特例表**：cairo（非 GObject）、回调、GVariant 动态类型、transfer 特殊的——lgi 保留 override 也是这个原因。

#### 结论

- **符号层面**：可以一把梭（`--whole-archive`），代价只是二进制变大（可 UPX 对冲）。
- **调用层面**：可以生成全量元数据表 + 通用 dispatcher，把"每个接口手写 10~30 行 C"变成"一行元数据"，接口增删改 = 重新生成——**90% 的"按需使用"能做到全自动**。
- **完全零维护不现实**：10% 的特例（回调/transfer/动态类型）永远要人工，但可以从 9,036 行缩到几百行特例表。

也就是说，**"半把梭"可行且是正解**：符号全链 + 全量生成元数据 + 通用 dispatcher，这就是把 lgi 的"运行时 typelib"翻译成"编译期固化"的完整形态——比 3.4 建议的档位 A（生成 C 注册表）更彻底，且不需要运行时反射。唯一的前置成本是写那个 GIR→元数据生成器。

### 3.6 优化条目（待录入）

（暂无条目——上方 3.1~3.5 中的「可借鉴/优化点」经用户确认后移入本子节）

---

## 四、2026-09-26 收口：Phase 3 交付物与当前真值

> 第一/二章的问答写在 09-25 上午。**09-25 下午到 09-26 的那一轮工作没有落进前两章**，
> 集中记在这里。细节见 `2026-09-25-record3.md` 第五章与 12.x 系列。

### 1、交付物

| 项 | 值 |
| --- | --- |
| 包 | `awm-static-musl-4.3-1910-5247a66-260925-204338.tar.gz`（4,399,604 B） |
| sha256 | `7148deb0…` |
| 二进制 | 9,621,832 B → 3,412,740 B（UPX 7 轮阶梯 `-9 --lzma`） |
| prefix | `/usr/local/static/awm`（旧 `/usr/local/static/awesome` 作废） |
| 包名 | `awm-static-musl`（旧 `awesome-static-musl` 作废） |
| 包内条目 | 346 |
| 验证 | T1/T2/T2b/T2c 全绿 + 宿主 `env -i` 裸跑（无 `--search`、无环境变量）出图通过 |

**包名四段**：`PKG`（硬编码）+ `VERSION`（`git describe --tags --long` 去 `v` 去 `-g<sha>`）
+ `SHA`（`rev-parse --short=7 HEAD`）+ `TS`（UTC+8，首次生成后冻结）。
`VERSION` 的尾号 = `git rev-list --count v4.3..HEAD`，是**离上游 tag 的累计提交数**，
单调递增（连 docs commit 都会 +1）。**判断两个包是否等价看 `SHA` 段，不看 `VERSION` 段。**
详见 record3 一·2 §2.1。

### 2、本轮做了什么（对应第一/二章里已经过期的结论）

| 第一/二章的说法 | 现状 |
| --- | --- |
| 产物 9,597,256 B | 已重编，UPX 后 3,412,740 B |
| 包名 `awesome-static-musl-*`、prefix `/usr/local/static/awesome` | 改名 `awm-static-musl-*`、prefix `/usr/local/static/awm` |
| gdk-pixbuf `loaders.cache` 路径烤死 → 图片全解码失败 | **已不成立**：`lgishim_pixbuf.c` 自解码 PNG/JPEG/XPM（`71e3aa7b`），不依赖 loader |
| dbus socket 路径烤进 rodata | **仍未修**，修法已定案：libdbus 用 `${prefix}/var` 构建，唯一解是重编 libdbus 加 `--sysdir=/run/dbus` |
| 真机黑屏根因待定（疑 C2） | **已定案**：双根因 —— `_native` full userdata（`cca0939d`）+ 编译期前缀烤死；C2 成因判断错 |
| librsvg 缺失影响 menubar/Surface 图标 | `imagebox.lua` 守卫已修（`1c0f142b`），`M.Rsvg = {}` 现为安全空表、PNG 回退生效 |
| shim 合计 9,036 行 | 现 **9,667 行 C + 57 行 Lua** |
| P1 六项 | 见 §3 |

**本轮另做的**（前文完全没提）：`Gio.Async.start` 落地（`lgishim_gio.c`，不做 lua 侧 fork）、
`cairo.Context` 补 `new_sub_path` / `user_to_device_distance`、`GLib.Timer` 三方法、
`Surface:write_to_png`、`pixbuf_get_from_surface`（含手写回退）、spec shim 化、
`screen.primary` 短路判据、出包脚本改成只做依赖预检（`preflight()`，不装任何东西）。

### 3、当前遗留（比第一章 §当前状态 更全）

1. **D-Bus 全栈 / socket 路径烤死** —— **唯一确认未修的缺陷**。改 awesome 的 prefix
   救不了；需重编 libdbus 加 `--sysdir=/run/dbus`（依赖层，不是 awesome 层）。
   影响：`naughty` 通知降级 + 启动 1 行警告，不崩。
2. **`cairo.Context:get_pixels` 未实现** —— 10 个 `spec/surface/crop_surface` 用例仍失败
   （失败点从 `:50` 前移到 `:58`，总数不变）。
3. **`SvgSurface.create` / `ImageSurface.create` 缺失** —— 仍为 nil。
4. **32 项 harness + Ubuntu glibc 重跑** —— 未做。
5. **Pango ≥1.50 分支** —— `get_preferred_size_at_dpi` / `get_height_for_width*` 未实现
   （`get_preferred_size` 已实现，「4 个未实现 layout API」这条已过期）。
6. **SVG 主题降级** —— 功能缺口，非阻塞。
7. **step 5 收尾 chown** —— 脚本里仍无收尾 `chown`（`:572` 只 chown `$OUT_DIR`，源文件会被改成 `root:root 444`）。
8. **theme 路径** —— 未做。
9. **12.7 配置加载顺序** —— 需用户侧确认实际 conffile（`luaa.c:1311-1342` 的查找顺序坑）。
10. **transient `lgishim_gio` 回调错误** —— 无法复现，已接 traceback，等它自己再出现。
11. **P2 尾巴** —— closure guard 定向泄漏测试、GTK 警告文案误导。
12. **P3 尾巴** —— `phase2-07` 仍把 Gio 列为阻塞（已过期）、`record1.md` 3 个待办未勾、
    「4 文件分叉」无集中清单、Part 7 API 名未核。

### 4、一个已知缺口（脚本层，唯一）

`11-package-static-release.sh` 生成的随包 `README.txt` 里写「dbus 不在运行时环境时，
通知静默降级，窗口管理不受影响」——用户照此理解会以为装个 `dbus-daemon` 就能收通知，
实际 socket 路径是编译期固化的，换路径必须重编 libdbus。这条说明还没写进去。
