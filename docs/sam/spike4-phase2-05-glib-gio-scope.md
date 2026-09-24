# Phase 2 Part 5/6 前置盘点（v2）：GLib / Gio / GObject 精确调用面

> **本文是 Part 5/6/9 开工前的前置盘点。实际落地结果（含 scope 漏项）见 `2026-09-23-record2.md` 篇七/篇八/篇九，对照表见其顶部。**

> 盘点时间：2026-09-24 CST
> 工作目录：`/_ext/home/highlight/headless/xm-zs01/fk-awesomewm`
> 分支：`feat/ai-260923-static`
> 前置：`spike4-phase2-01-scope.md`（粗粒度基线）· `spike4-glib-gio-scope.md`（v1，双稿对照）
> 本文档定位：**v2 独立稿**，供与 v1 比对取舍

---

## 一、盘点方法与计数口径

### 1、grep 模式（`lib/` 全目录，`--include="*.lua"`，排除 `lib/vendor/`）

```bash
# 别名与导入
grep -rnE 'require\(.*(GLib|Gio|GObject|glib|gio)' lib/ --include='*.lua'
grep -rnE 'local (GLib|Gio|GObject|glib|gio)\s*=' lib/ --include='*.lua'
grep -rnE 'lgi\.(GLib|Gio|GObject)' lib/ --include='*.lua'

# API 调用（大小写敏感：lib/ 内既存在 GLib 大写也存在 glib 小写别名）
grep -rn '\b(GLib|Gio|GObject|glib|gio)\.' lib/ --include='*.lua'
```

### 2、别名处理（实测 11 处导入点，5 种风格）

| 别名 | 文件 | 行号 | 目标 |
|------|------|------|------|
| `local glib = require("vendor.lgishim").GLib` | `lib/gears/timer.lua` | 60 | GLib 主循环 |
| `local glib = require("vendor.lgishim").GLib` | `lib/awful/keygrabber.lua` | 75 | GLib UTF-8 |
| `local glib = require("vendor.lgishim").GLib` | `lib/awful/screenshot.lua` | 43 | GLib DateTime |
| `local glib = require("vendor.lgishim").GLib` | `lib/wibox/widget/textclock.lua` | 51 | GLib DateTime |
| `local GLib = require("vendor.lgishim").GLib` | `lib/menubar/icon_theme.lua` | 15 | GLib 路径 |
| `local Gio = require("vendor.lgishim").Gio` | `lib/gears/filesystem.lua` | 11 | Gio.File |
| `local Gio = require("vendor.lgishim").Gio` | `lib/beautiful/init.lua` | 20 | Gio.File |
| `local gio = lgi.Gio` / `local glib = lgi.GLib` | `lib/menubar/utils.lua` | 17, 18 | Gio + GLib |
| `local Gio = lgi.Gio` / `local GLib = lgi.GLib` | `lib/awful/spawn.lua` | 227-228 | Gio + GLib 流 |
| `lgi.cairo, lgi.Gio, lgi.GLib, lgi.GObject` | `lib/naughty/dbus.lua` | 19 | 全栈 |
| `local GLib = lgi.GLib` | `lib/wibox/container/scroll.lua` | 33 | GLib.Timer |

### 3、计数口径（三类严格区分）

| 类别 | 判据 | 计入 API 表 | 计入枚举表 |
|------|------|:---:|:---:|
| **静态构造** | `Gio.File.new_for_path(...)` | ✅ | — |
| **实例方法** | `obj:method(...)` | ✅ | — |
| **lgi 类构造** | `Gio.DBusMethodInfo{...}` / `GLib.String(cmd)` | ✅ | — |
| **枚举常量** | `Gio.FileQueryInfoFlags.NONE` | — | ✅ |
| **字符串常量** | `gio.FILE_ATTRIBUTE_STANDARD_NAME` | — | ✅ |
| **`require` 本身** | 导入语句 | ❌ | ❌ |
| **注释/ldoc** | `--` 行首或行内 | ❌ | ❌ |

### 4、仅注释、已排除（`spike4-phase2-01-scope.md` 已正确标注的四处）

| 名称 | 出处 | 判据 |
|------|------|------|
| `GLib.Error` | `lib/wibox/widget/textbox.lua:162` | 行首 `--` |
| `GLib.Bytes` | `lib/awful/spawn.lua:536` | 行首 `--` |
| `Gio.InputStream` | `lib/awful/spawn.lua:501` | ldoc `-- @tparam` |
| `Gio.DBusConnection` | `lib/naughty/dbus.lua:38` | 行首 `--` |

---

## 二、GLib 层精确清单

### 1、主循环（gears/timer.lua 全栈依赖）

| API | 次数 | 文件:行号 | 调用形态 | 返回值用法 |
|-----|:---:|------|------|------|
| `glib.timeout_add` | 1 | `gears/timer.lua:102` | `glib.timeout_add(glib.PRIORITY_DEFAULT, timeout_ms, fn)` | `source_id` → `self.data.source_id` |
| `glib.source_remove` | 1 | `gears/timer.lua:120` | `glib.source_remove(self.data.source_id)` | 无返回 |
| `glib.PRIORITY_DEFAULT` | **2** | `gears/timer.lua:102` · `awful/spawn.lua:522` | 常量 | 传入 `timeout_add` / `read_line_async` |
| `glib.SOURCE_CONTINUE` | 1 | `gears/timer.lua:104` | `return glib.SOURCE_CONTINUE` | 回调返回常量 |

> **修正点 1**：`spike4-phase2-01-scope.md` 列 `GLib.PRIORITY_DEFAULT × 1`——遗漏 `awful/spawn.lua:522` 的 `stream:read_line_async(GLib.PRIORITY_DEFAULT, ...)`。实测 **2 处**。

### 2、路径拼接（menubar/icon_theme.lua + menubar/utils.lua）

| API | 次数 | 文件:行号 | 调用形态 |
|-----|:---:|------|------|
| `GLib.build_filenamev` | **7** | `menubar/icon_theme.lua:27,32,38,46` · `menubar/utils.lua:163,189,192` | 单参 `{...}` 变长拼接 |
| `GLib.get_home_dir` | **2** | `menubar/icon_theme.lua:27` · `menubar/utils.lua:172` | 无参 |
| `GLib.get_user_data_dir` | **2** | `menubar/icon_theme.lua:32` · `menubar/utils.lua:174` | 无参 |
| `GLib.get_system_data_dirs` | **4** | `menubar/icon_theme.lua:37,45` · `menubar/utils.lua:175,177` | 无参，返回 table |

> **修正点 2**：与 `spike4-phase2-01-scope.md` 差异——`build_filenamev` 文档 5 → 实测 **7**（icon_theme 4 + utils 3）；`get_home_dir` 文档 1 → 实测 **2**；`get_user_data_dir` 文档 1 → 实测 **2**；`get_system_data_dirs` 文档 2 → 实测 **4**。

### 3、GLib.String / GLib.Timer（spawn.lua + scroll.lua）

| API | 次数 | 文件:行号 | 调用形态 |
|-----|:---:|------|------|
| `GLib.String(cmd)` | 1 | `awful/spawn.lua:305` | 类构造，存入 `glibstr` |
| `GLib.String.hash` | 1 | `awful/spawn.lua:307` | `GLib.String.hash(glibstr)` → integer → `%x` 格式化 |
| `GLib.Timer()` | 1 | `wibox/container/scroll.lua:425` | 类构造，存入 `ret._private.timer` |
| `timer:elapsed()` | 1 | `wibox/container/scroll.lua:131` | 秒数 → `step_function(...)` 首参 |

### 4、GLib.Variant（`naughty/dbus.lua`，8 处全覆盖）

| # | 行号 | 格式串 | 值 | 语义 |
|---|:---:|:---:|------|------|
| 1 | `dbus.lua:64` | `(us)` | `{notificationId, action}` | ActionInvoked 信号载荷 |
| 2 | `dbus.lua:78` | `(uu)` | `{notificationId, reason}` | NotificationClosed 载荷 |
| 3 | `dbus.lua:352` | `(u)` | `{notification.id}` | Notify 返回值 |
| 4 | `dbus.lua:356` | `(u)` | `{nnotif._gen_next_id()}` | Notify 返回值（新） |
| 5 | `dbus.lua:364` | `()` | `{}` | CloseNotification 空返回 |
| 6 | `dbus.lua:369` | `(ssss)` | `{app, vendor, ver, spec}` | GetServerInformation 返回 |
| 7 | `dbus.lua:377` | `(as)` | `{capabilities}` | GetCapabilities 返回 |
| 8 | `dbus.lua:496` | `(s)` | `{notif._private._unique_sender}` | GetConnectionUnixProcessID 参数 |

> **V.1 Variant 需要支持的类型标记**：`u` · `s` · `as` · `()` · `(u)` · `(us)` · `(uu)` · `(ssss)` · `(as)` —— 共 **5 种基元**（uint32、string、空、tuple、字符串数组）。**另有 DBusInterfaceInfo 定义处**（`dbus.lua:396-432`）出现 `(ay)` / `(a{sv})` / `(i)` 3 种，shim 也需支持：`ay`（bytes 数组）、`a{sv}`（异质字典）、`i`（int32）。**Variant 类型表总计 8 种**。

### 5、GLib.KeyFile（`menubar/utils.lua:269-281`，`.desktop` 解析）

| API | 次数 | 文件:行号 | 调用形态 |
|-----|:---:|------|------|
| `glib.KeyFile()` | 1 | `menubar/utils.lua:269` | 类构造 |
| `keyfile:load_from_file` | 1 | `menubar/utils.lua:270` | `(file, flags)` → bool |
| `glib.KeyFileFlags.NONE` | 1 | `menubar/utils.lua:270` | 枚举常量 |
| `keyfile:has_group` | 1 | `menubar/utils.lua:275` | `(group)` → bool |
| `keyfile:get_keys` | 1 | `menubar/utils.lua:279` | `(group)` → table of key |
| `kf:get_string` | 1 | `menubar/utils.lua:51,281` | `(group, key)` → string |
| `kf:get_string_list` | 1 | `menubar/utils.lua:54` | `(group, key, fallback)` → table |
| `kf:get_locale_string` | 1 | `menubar/utils.lua:57` | `(group, key, fallback, locale)` → string |
| `kf:get_locale_string_list` | 1 | `menubar/utils.lua:60` | `(group, key, fallback, locale)` → table |
| `kf:get_boolean` | 1 | `menubar/utils.lua:63` | `(group, key)` → bool |

> **KeyFile 是 10 个 API 的独立子集**，`spike4-phase2-01-scope.md` §3.3 表未列（v1 已补齐）。

### 6、GLib.DateTime / GLib.TimeZone（`awful/screenshot.lua` + `wibox/widget/textclock.lua`）

| API | 次数 | 文件:行号 | 调用形态 |
|-----|:---:|------|------|
| `glib.DateTime` / `DateTime.new_now` | 4 | `awful/screenshot.lua:44,51` · `wibox/widget/textclock.lua:52,57` | 类 & 方法别名 |
| `DateTime.new_from_unix_utc` | 2 | `awful/screenshot.lua:58` · `wibox/widget/textclock.lua:64` | 静态方法 `(epoch)` |
| `datetime_obj(tz):format(fmt)` | 2 | `awful/screenshot.lua:65` · `wibox/widget/textclock.lua:148` | 实例方法 |
| `glib.TimeZone` / `TimeZone.new` | 3 | `awful/screenshot.lua:45` · `wibox/widget/textclock.lua:53,93` | 类构造 `(tzid)` |
| `TimeZone.new_local` | 2 | `awful/screenshot.lua:65` · `wibox/widget/textclock.lua:148` | 静态方法 |

### 7、GLib UTF-8 函数（`awful/keygrabber.lua`）

| API | 次数 | 文件:行号 | 调用形态 |
|-----|:---:|------|------|
| `glib.utf8_strlen` | 2 | `awful/keygrabber.lua:206,211` | `(str, len)` → integer |
| `glib.utf8_substring` | 1 | `awful/keygrabber.lua:210` | `(str, offset, length)` → string |

### 8、GLib 去重 API 汇总

| 类别 | 数量 |
|------|:---:|
| 主循环 | 4 |
| 路径函数 | 4 |
| String / Timer | 4 |
| Variant | 1 |
| KeyFile | 10（含 `KeyFileFlags`） |
| DateTime / TimeZone | 7 |
| UTF-8 | 2 |
| **合计（去重 API 名）** | **32** |
| **仅类/方法（不含枚举常量）** | **25** |

---

## 三、Gio 层精确清单

### 1、Gio.File 系列（gears/filesystem.lua + beautiful/init.lua + menubar/utils.lua）

| API | 次数 | 文件:行号 | 调用形态 |
|-----|:---:|------|------|
| `Gio.File.new_for_path` | **10** | `gears/filesystem.lua:34,47,55,67,79,91,103,202` · `beautiful/init.lua:274` · `menubar/utils.lua:409` | 类构造 `(path)` |
| `:get_parent` | 2 | `gears/filesystem.lua:47` · `beautiful/init.lua:274` | 实例方法 |
| `:query_info` | 4 | `gears/filesystem.lua:56,68,80,92` | 实例方法 `(attr_spec, flags)` |
| `:get_file_type` | 5 | `gears/filesystem.lua:58,70,82,94,208` | 实例方法 → `"DIRECTORY"` / `"REGULAR"` |
| `:get_attribute_boolean` | 4 | `gears/filesystem.lua:59,71,83,95` | 实例方法 `(attr)` |
| `:make_directory_with_parents` | 1 | `gears/filesystem.lua:18` | 实例方法 → `(bool, err)` |
| `:query_file_type` | 1 | `gears/filesystem.lua:103` | 实例方法 `(flags)` |
| `:enumerate_children` | 1 | `gears/filesystem.lua:202` | 实例方法 → FileEnumerator |
| `:async_enumerate_children` | 1 | `menubar/utils.lua:371` | 实例方法 |
| `:get_path` | 2 | `menubar/utils.lua:365,387` · `beautiful/init.lua:275`（链式） | 实例方法 |
| `:get_uri` | 1 | `menubar/utils.lua:365` | 实例方法 |
| `:get_display_name` | 1 | `gears/filesystem.lua:209` | 实例方法 |
| `:next_file` | 1 | `gears/filesystem.lua:207` | FileEnumerator 方法 |
| `:async_next_files` | 1 | `menubar/utils.lua:378` | FileEnumerator 方法 |
| `:async_close` | 1 | `menubar/utils.lua:404` | FileEnumerator 方法 |

> **修正点 3**：`spike4-phase2-01-scope.md` §3.4 列 `Gio.File.new_for_path × 9`——**准确对应 `Gio.` 大写**（filesystem.lua 8 + beautiful/init.lua 1），但**遗漏 `menubar/utils.lua:409` 的 `gio.` 小写别名**。实测 `new_for_path` 共 **10 处**。
>
> **修正点 4**：scope.md 表未列 Gio.File 的 13 个实例方法。File 完整 API 面 **15 个**（1 构造 + 13 实例 + 1 async_enumerate_children，其中 enumerate_children 与 async_ 版本各 1 次）。

### 2、流（awful/spawn.lua + menubar/utils.lua）

| API | 次数 | 文件:行号 | 调用形态 |
|-----|:---:|------|------|
| `Gio.DataInputStream.new` | 3 | `awful/spawn.lua:245,247,510` | 类构造 `(stream)` |
| `Gio.MemoryInputStream.new_from_data` | 2 | `awful/spawn.lua:245,247` | 静态方法 `(data)` |
| `Gio.UnixInputStream.new` | 2 | `awful/spawn.lua:421,425` | 类构造 `(fd, close_fd)` |
| `:read_line` | 1 | `awful/spawn.lua:249` | 实例方法 |
| `:read_line_async` | 1 | `awful/spawn.lua:522` | 实例方法 `(priority, cancel, cb)` |
| `:read_line_finish` | 1 | `awful/spawn.lua:525` | 实例方法 `(result)` |
| `:close` | 1 | `awful/spawn.lua:513` | 实例方法 |
| `:set_buffer_size` | 1 | `awful/spawn.lua:515` | 实例方法 |

> **决策继承**：`read_line_async` + `read_line_finish` 是 async API，`spike4-phase2-01-scope.md` §3.3 已决策替代方案——**保留 lib 侧调用不变，shim 内部实现改为同步 + 主循环 timer 分片**（见 §五.3）。

### 3、D-Bus（naughty/dbus.lua 全栈，33 处调用点）

| 分类 | API | 次数 | 行号 | 备注 |
|:---:|-----|:---:|------|------|
| 类型/构造 | `Gio.DBusProxy.new_for_bus` | 1 | `dbus.lua:439-451` | 9 参数 |
| | `Gio.bus_own_name` | 1 | `dbus.lua:540-542` | 5 参数 |
| | `Gio.DBusArgInfo{...}` | 1 | `dbus.lua:396` | lgi 类构造 |
| | `Gio.DBusMethodInfo{...}` | 4 | `dbus.lua:404,407,410,415` | 4 个 method 定义 |
| | `Gio.DBusSignalInfo{...}` | 2 | `dbus.lua:425,428` | 2 个 signal 定义 |
| | `Gio.DBusInterfaceInfo{...}` | 1 | `dbus.lua:401-432` | 顶层 interface |
| 实例方法 | `:emit_signal` | 2 | `dbus.lua:62,76` | signal 发射 |
| | `:register_object` | 1 | `dbus.lua:433-434` | 接口注册 |
| | `:call_sync` | 1 | `dbus.lua:502-506` | 同步调用 |
| 枚举常量 | `Gio.BusType.SESSION` | 2 | `dbus.lua:440,540` | session 总线 |
| | `Gio.DBusProxyFlags.DO_NOT_LOAD_PROPERTIES` | 1 | `dbus.lua:441` | 代理 flags |
| | `Gio.DBusCallFlags.NONE` | 1 | `dbus.lua:504` | call flags |
| | `Gio.BusNameOwnerFlags.NONE` | 1 | `dbus.lua:541` | name flags |
| 隐式类型 | `Gio.DBusConnection` | 0 | `dbus.lua:38` 仅注释 | `bus_connection` 局部变量 |

> **D-Bus 完整调用面**：类型/方法 9 + 枚举常量 6 = **15 处独立调用**（含闭包回调另见 §四）。

### 4、Gio.Async（决策砍）

| API | 次数 | 文件:行号 | 决策 |
|-----|:---:|------|------|
| `gio.Async.start` | 1 | `menubar/utils.lua:407` | 砍，改同步 + timer 分块 |

### 5、枚举/常量

| 枚举/常量 | 次数 | 文件:行号 | 用法 |
|------|:---:|------|------|
| `Gio.IOErrorEnum` | 1 | `gears/filesystem.lua:22` | `err.domain == Gio.IOErrorEnum` |
| `Gio.IOErrorEnum.EXISTS`（隐式 `.code == "EXISTS"`） | 1 | `gears/filesystem.lua:22` | 字符串比较 |
| `Gio.FileQueryInfoFlags.NONE` | 5 | `gears/filesystem.lua:57,69,81,93` · `menubar/utils.lua:371` | 传入 `query_info` / `async_enumerate_children` |
| `gio.FILE_ATTRIBUTE_STANDARD_NAME` | 1 | `menubar/utils.lua:370` | `..` 字符串拼接 |
| `gio.FILE_ATTRIBUTE_STANDARD_TYPE` | 1 | `menubar/utils.lua:370` | `..` 字符串拼接 |

### 6、Gio 去重 API 汇总

| 子集 | 数量 |
|------|:---:|
| Gio.File 构造 + 实例方法 | 15 |
| 流类型 + 方法 | 8 |
| D-Bus 类型/方法 | 9 |
| D-Bus 枚举常量 | 4（BusType.SESSION · 3 个 Flags） |
| Async | 1 |
| IOErrorEnum + FileQueryInfoFlags + FILE_ATTRIBUTE_* | 4 |
| **合计（含枚举）** | **41** |
| **仅类/方法（不含枚举）** | **33** |

---

## 四、GObject 层精确清单

| API | 次数 | 文件:行号 | 语义 |
|-----|:---:|------|------|
| `GObject.Closure(fn)` | **4** | `naughty/dbus.lua:434,541,542,542` | 包装 Lua 函数为 GCallback |

**四个 Closure 的具体回调**：

| 行号 | 回调 | 触发时机 |
|------|------|----------|
| `dbus.lua:434` | `method_call` | 收到 D-Bus method call（4 个 `org.freedesktop.Notifications` 方法） |
| `dbus.lua:541` | `on_bus_acquire` | 首次获得 bus 连接，注册 interface |
| `dbus.lua:541` | `on_name_acquired` | 拿到 `org.freedesktop.Notifications` 名字所有权 |
| `dbus.lua:542` | `on_name_lost` | 名字所有权丢失 |

> **GObject 精确去重 API 个数：1**（`GObject.Closure`）。但它是 D-Bus 全栈的**必要基础设施**——无此则 4 个 D-Bus 回调全部无法注册，Notifications 协议完全丢失。

---

## 五、跨层依赖与施工顺序

### 1、依赖关系图

```
GLib 主循环（Part 5a，必须先做）
   ├─ timeout_add / source_remove / PRIORITY_DEFAULT / SOURCE_CONTINUE
   └─ 下游：所有需要 timer 的路径

GLib 独立子集（Part 5b/5c/5d，可并行）
   ├─ String / Timer / UTF-8（spawn、scroll、keygrabber）
   ├─ KeyFile（menubar/utils .desktop 解析）
   └─ DateTime / TimeZone（textclock、screenshot）

GLib.Variant（Part 5e，D-Bus 前置）
   └─ 下游：所有 GLib.Variant(...) 调用

Gio.File（Part 6a，依赖 GLib 主循环在跑）
   └─ gears/filesystem 全部 6 个 gfs.* 公共 API

Gio 流（Part 6b，依赖 GLib 主循环 + GLib.PRIORITY_DEFAULT）
   ├─ spawn.read_lines（read_line_async + read_line_finish）
   └─ spawn.easy_async（DataInputStream + UnixInputStream + MemoryInputStream）

Gio D-Bus（Part 6c，依赖 GLib.Variant + GLib 主循环 + GObject.Closure）
   └─ naughty/dbus.lua 全栈

GObject.Closure（Part 6d，与 D-Bus 同步或稍后）
   └─ D-Bus 回调的包装器
```

### 2、`gio.Async` 替代方案（Part 6e）

**决策**：`gio.Async` 在 lib/ 仅 1 处（`menubar/utils.lua:407`）。**保留 `utils.parse_dir` 的公开 API，内部改为同步 + timer 分块**。

```lua
-- 原实现（异步）：
gio.Async.start(do_protected_call)(function()
  parser(gio.File.new_for_path(dir), result)
  call_callback(callback, result)
end)

-- 替代实现（同步 + timer 分片）：
local pending = {dir_path}
local result = {}
local function chunk()
    if pending[1] then
        local dir = table.remove(pending, 1)
        recurse_sync(dir, result)  -- enumerate_children 同步
    end
    if pending[1] then
        gears.timer.new({timeout=0, callback=chunk}):start()
    else
        call_callback(callback, result)
    end
end
chunk()
```

**代价**：`parse_dir` 从"完全异步"变为"分片式异步"——每个 chunk 占一次主循环 tick，对外签名不变。

### 3、施工顺序硬约束

1. **GLib 主循环（5a）**必须先于所有 Gio 层——`Gio.DBusProxy.new_for_bus` 需主循环跑才能收到 D-Bus 事件
2. **GLib.Variant（5e）**必须先于 D-Bus（6c）
3. **GObject.Closure（6d）**与 D-Bus（6c）同步或稍后，两者互相依赖但 D-Bus 可先实现类型桩位
4. **gio.Async 替代（6e）**依赖 Gio.File（6a）的同步版
5. **GLib String / Timer / UTF-8 / KeyFile / DateTime** 相互独立，可与主循环并行开发

---

## 六、人天校定

### 1、`spike4-phase2-01-scope.md` 原估 vs 实测

| 层 | 原估 | 实测修正 | 差异原因 |
|:---:|:---:|:---:|------|
| GLib | 3-6 | **4-6** | + KeyFile(10) + DateTime/TimeZone(7) + UTF-8(2) = **+19 处新增调用** |
| Gio | 5-8 | **6-9** | + File 实例方法(13) + 流方法(6) + FILE_ATTRIBUTE(2) = **+21 处新增** |
| GObject | 0.5-1 | **0.5-1** | Closure 1 API 4 处，维持 |

### 2、上调/下调结论

| 层 | 结论 | 依据 |
|:---:|:---:|------|
| **GLib** | **上调**：3-6 → **4-6** | KeyFile 是 10 个 API 的独立子集，C 层 `g_key_file_*` 系列稳定但工作量不小。DateTime/TimeZone 又引入 `g_date_time_new/format` + `g_time_zone_new`。若 KeyFile 砍掉则下调回 3-5。 |
| **Gio** | **上调**：5-8 → **6-9** | File 实例方法 13 个（比 `new_for_path` 多出）；DataInputStream + MemoryInputStream + UnixInputStream 3 类 + 4 实例方法；D-Bus 全栈 9 类型 + 4 枚举常量表。 |
| **GObject** | **维持**：0.5-1 | Closure 1 API，实现难度中等（`g_closure_new_object` + 回调→Lua 栈）。 |

### 3、GLib.KeyFile 的可行性分支

| 分支 | 判断 | 人天影响 |
|:---:|:---:|:---:|
| A：实现完整 KeyFile | lib 侧零改动 | +0.5 人天 |
| B：Lua 手写 `.desktop` 解析器 | 重写 `menubar/utils.lua:264-353` 约 90 行 | -0.3 人天（省 shim 但改 lib） |
| C：KeyFile 最小实现（只 4 方法） | 需检查 8 个 `.desktop` key 是否含 locale/boolean | -0.2 人天 |

**推荐分支 A**：KeyFile 是 GLib 基础类，`g_key_file_*` 系列稳定，且避免 lib 侧改动。

### 4、GLib.DateTime 的可行性分支

| 分支 | 判断 | 人天影响 |
|:---:|:---:|:---:|
| A：实现 DateTime/TimeZone shim | lib 侧零改动 | +0.3 人天 |
| B：改用 Lua `os.date` | 改 2 文件约 20 行 | -0.1 人天，但丢失 `%S%f` 毫秒格式化 |

**推荐分支 A**：`textclock` 是常用 widget，`os.date` 不支持 GLib 的 `%S%f` 毫秒格式。

---

## 七、施工切分建议（Part 5 分成 5 个可独立验收的小批次）

### 批次 P5a：GLib 主循环（**必须先**，1.0 人天）

| 项 | 内容 |
|----|------|
| **C API** | `g_timeout_add_full` · `g_source_remove` · `G_PRIORITY_DEFAULT` · `G_SOURCE_CONTINUE` |
| **覆盖 API** | `timeout_add` · `source_remove` · `PRIORITY_DEFAULT` · `SOURCE_CONTINUE` |
| **覆盖调用点** | `gears/timer.lua:102,104,120` · `awful/spawn.lua:522`（间接） |
| **冒烟验证** | 1) `gtimer.new({timeout=0.05, callback=fn}):start()` 能触发；2) `:stop()` 停止；3) `PRIORITY_DEFAULT == 0`；4) `SOURCE_CONTINUE == 1`；5) source_id 递增 |

### 批次 P5b：路径 + String + Timer + UTF-8（**独立**，1.0 人天）

| 项 | 内容 |
|----|------|
| **C API** | `g_build_filenamev` · `g_get_home_dir` · `g_get_user_data_dir` · `g_get_system_data_dirs` · `g_string_new` · `g_string_hash` · `g_timer_new` · `g_timer_elapsed` · `g_utf8_strlen` · `g_utf8_substring` |
| **覆盖 API** | build_filenamev · get_home_dir · get_user_data_dir · get_system_data_dirs · String · String.hash · Timer · Timer:elapsed · utf8_strlen · utf8_substring |
| **覆盖调用点** | `menubar/icon_theme.lua:27,32,37,38,45,46` · `menubar/utils.lua:163,172,174,175,177,189,192` · `awful/spawn.lua:305,307` · `wibox/container/scroll.lua:131,425` · `awful/keygrabber.lua:206,210,211` |
| **冒烟验证** | 1) `build_filenamev({"a","b"})` == `"a/b"`；2) `String.hash("cmd")` 返回稳定整数；3) `Timer()` 后 `:elapsed()` 单调递增；4) `utf8_strlen("héllo")` == 5 |

### 批次 P5c：GLib.Variant（**D-Bus 前置**，1.5 人天）

| 项 | 内容 |
|----|------|
| **C API** | `g_variant_new` · `g_variant_type_new` · 8 种签名（`(u)(s)(uu)(us)(ssss)(as)(ay)(a{sv})(i)`） |
| **覆盖 API** | `GLib.Variant(fmt, values)` |
| **覆盖调用点** | `naughty/dbus.lua:64,78,352,356,364,369,377,496` |
| **冒烟验证** | 1) `GLib.Variant("(u)",{42}).value[1] == 42`；2) `"(as)"` 支持字符串数组；3) `"(ay)"` 支持字节数组；4) `"()"` 空 tuple；5) `:get_child_value(i)` 可读回；6) `:pairs()` 支持 `a{sv}` 遍历 |

### 批次 P5d：GLib.KeyFile（**独立**，0.5 人天）

| 项 | 内容 |
|----|------|
| **C API** | `g_key_file_new/free/load_from_file/has_group/get_keys/get_string/get_string_list/get_locale_string/get_locale_string_list/get_boolean` |
| **覆盖 API** | `KeyFile()` · `:load_from_file` · `:has_group` · `:get_keys` · `:get_string` · `:get_string_list` · `:get_locale_string` · `:get_locale_string_list` · `:get_boolean` · `KeyFileFlags.NONE` |
| **覆盖调用点** | `menubar/utils.lua:269-281`（10 处） |
| **冒烟验证** | 1) `load_from_file("/tmp/test.desktop", NONE)` → true；2) `get_keys("Desktop Entry")` 返回键表；3) 三种类型 getter 各 1 例；4) `parse_desktop_file` 全量覆盖 `/usr/share/applications/*.desktop` |

### 批次 P5e：GLib.DateTime / TimeZone（**独立**，0.3 人天）

| 项 | 内容 |
|----|------|
| **C API** | `g_date_time_new_now_local` · `g_date_time_new_from_unix_utc` · `g_date_time_new` · `g_date_time_format` · `g_time_zone_new` · `g_time_zone_new_local` |
| **覆盖 API** | DateTime 类 + `new_now` · `new_from_unix_utc` · `new(tz)` · `:format` · TimeZone 类 + `new` · `new_local` |
| **覆盖调用点** | `awful/screenshot.lua:44,45,51,58,65` · `wibox/widget/textclock.lua:52,53,57,64,93,148` |
| **冒烟验证** | 1) `DateTime.new_now(TimeZone.new_local()):format("%Y-%m-%d")` 返回字符串；2) `DateTime.new_from_unix_utc(0):format("%Y") == "1970"`；3) `TimeZone.new("Europe/Amsterdam")` 不报错；4) `SOURCE_DATE_EPOCH` 生效（textclock 可复现） |

**Part 5 小计**：1.0 + 1.0 + 1.5 + 0.5 + 0.3 = **4.3 人天**

---

## 八、已知不覆盖项（`lib/` 未用，可不做）

| 项 | 判据 | 备注 |
|:---|:---:|------|
| `GLib.Error` | 仅 `textbox.lua:162` 注释 | 预留类型桩位即可 |
| `GLib.Bytes` | 仅 `spawn.lua:536` 注释 | 保留空引用 |
| `Gio.DBusConnection` | 仅 `dbus.lua:38` 注释 | `bus_connection` 实际来自 `bus_own_name` 回调 |
| `Gio.InputStream` | 仅 `spawn.lua:501` ldoc | `DataInputStream.new` 隐式依赖 |
| `Gio.Async`（除 `start` 外） | lib/ 仅 1 处（407 行） | 决策砍，改同步 + timer 分块（§五.2） |
| `GLib.Error.new` / `matches` | 全 lib 无调用 | 不实现 |
| `Gio.MemoryOutputStream` / `FileOutputStream` / `PipeInputStream` 等未用流 | 全 lib 无调用 | 不实现 |
| `Gio.DBusMenu` / `DBusMenuExported` / `DBusApplicationMenu` | 全 lib 无调用 | 不实现 |
| `Gio.FileEnumerator` 独立类构造 | lib/ 只通过 `enumerate_children` 间接持有 | 不暴露独立构造函数 |
| `GLib.SList` / `HList` / `HashTable` / `Queue` | 全 lib 无调用 | 不实现 |
| `GObject.Object` / `ParamSpec` / `Signal` | 全 lib 无直接调用 | 不实现（Closure 除外） |
| `GLib.SpawnFlags` / `GLib.Subprocess` / `GLib.ChildWatch` | 全 lib 无调用（`awful.spawn` 用 `posix.spawn` 而非 GLib.Spawn） | 不实现 |

**未覆盖的单个 API 无需 stub**：`lgishim.lua` 的 `pcall` 保护已足够，`GLib.SpawnFlags` 这类全 lib 无调用的东西不必实现。

> **⚠ 2026-09-24 校定：「空表可安全保留」这个措辞是错的，必须分开说。**
> - ✅ **仍然成立**：模块表本身不能是 nil（否则 `lgi.GLib.xxx` 直接抛 `attempt to index a nil value`），未覆盖的单个 API 也不必 stub。
> - ❌ **不成立**：`Gio = {}` 作为**整层空表**并不安全——它是两篇施工记录都没发现的硬阻塞（`gears/filesystem.lua`、`beautiful/init.lua`、`menubar/utils.lua`、`naughty/dbus.lua`、`wibox/widget/imagebox.lua` 五条主路径全断）。根因是 `lgishim.lua` 的 `pcall(require)` 回退路径**设计上不报错**，而冒烟 harness 自带一份 preload 清单，于是 harness 绿、真机坏。见 `2026-09-23-record2.md` 篇九 §5。
> - ✅ **结果**：GLib（Part 5）、Gio（Part 6）、GObject（Part 9）均已交付，只剩 `Rsvg = {}` 一张整层空表。

---

## 附 A：与 `spike4-phase2-01-scope.md` 数字差异汇总

| 项 | scope.md | 本文档 | 差异原因 |
|------|:---:|:---:|------|
| `GLib.PRIORITY_DEFAULT` | 1 | **2** | 遗漏 `spawn.lua:522` |
| `GLib.build_filenamev` | 5 | **7** | 遗漏 `utils.lua:189,192` |
| `GLib.get_home_dir` | 1 | **2** | 遗漏 `utils.lua:172` |
| `GLib.get_user_data_dir` | 1 | **2** | 遗漏 `utils.lua:174` |
| `GLib.get_system_data_dirs` | 2 | **4** | 遗漏 `utils.lua:175,177` |
| `Gio.File.new_for_path` | 9 | **10** | 遗漏 `menubar/utils.lua:409`（`gio.` 小写别名） |
| `Gio.File` 完整 API 面 | 1 类（仅 `new_for_path`） | **15 个 API**（+ 13 实例方法） | scope 未列 File 实例方法 |
| `GLib.KeyFile` | **未列** | **10 处 API** | scope 完全遗漏 |
| `GLib.DateTime` / `TimeZone` | **未列** | **6 处 API** | scope 完全遗漏 |
| `GLib.utf8_strlen` / `utf8_substring` | **未列** | **3 处 API** | scope 完全遗漏 |
| `Gio.FILE_ATTRIBUTE_*` | **未列** | **2 个字符串常量** | scope 完全遗漏 |
| `Gio.bus_own_name` | 1 | **1** | 一致 |
| `GObject.Closure` | 4 | **4** | 一致 |
| `GLib.Error` / `GLib.Bytes` / `Gio.DBusConnection` / `Gio.InputStream` | 已排除为"仅注释" | 一致 | ✅ |

**核心修正 4 条**：
1. `Gio.File` 面远大于 `new_for_path`（+ 13 实例方法）
2. **GLib.KeyFile** 是 scope.md 完全遗漏的第 4 个 GLib 子集（10 处 API）
3. **GLib.DateTime/TimeZone + UTF-8** 也是 scope.md 遗漏的 2 个子集（共 9 处）
4. `Gio.FILE_ATTRIBUTE_*` 是**字符串常量**而非完整枚举，shim 只需暴露 2 个字符串即可

## 附 B：GLib / Gio / GObject 精确去重 API 个数

| 层 | 含枚举常量 | 仅类/方法 |
|:---:|:---:|:---:|
| **GLib** | **32** | **25** |
| **Gio** | **41** | **33** |
| **GObject** | **1** | **1** |
| **合计** | **74** | **59** |

## 附 C：前 10 个最关键的文件:行号（施工顺序）

| # | 文件:行号 | API | 关键度 |
|---|------|------|------|
| 1 | `lib/gears/timer.lua:102` | `glib.timeout_add(PRIORITY_DEFAULT, ms, fn)` | **GLib 主循环入口**，全 lib 唯一 timer |
| 2 | `lib/gears/timer.lua:120` | `glib.source_remove(id)` | 主循环 stop |
| 3 | `lib/naughty/dbus.lua:439-451` | `Gio.DBusProxy.new_for_bus(...)` | D-Bus 连接初始化，9 参数 |
| 4 | `lib/naughty/dbus.lua:540-542` | `Gio.bus_own_name(..., GObject.Closure × 3)` | 名字所有权注册 |
| 5 | `lib/naughty/dbus.lua:433-434` | `conn:register_object(path, iface_info, GObject.Closure(method_call))` | 接口注册 |
| 6 | `lib/naughty/dbus.lua:64` | `GLib.Variant("(us)", {...})` | Variant 首例 |
| 7 | `lib/naughty/dbus.lua:396-432` | `Gio.DBusArgInfo/MethodInfo/SignalInfo/InterfaceInfo` 构造 | D-Bus 接口定义 |
| 8 | `lib/gears/filesystem.lua:55-59` | `Gio.File.new_for_path + :query_info + :get_file_type + :get_attribute_boolean` | File 完整链 |
| 9 | `lib/awful/spawn.lua:510-543` | `Gio.DataInputStream.new + :read_line_async + :read_line_finish + :close + :set_buffer_size` | 流读取栈 |
| 10 | `lib/menubar/utils.lua:269-281` | `glib.KeyFile()` + `:load_from_file + :has_group + :get_keys + get_*` | **scope.md 遗漏** |

## 附 D：与 v1 的对比建议关注点

若 v1 已覆盖以下 5 项，则本稿可作校对；若遗漏则 v1 低估 GLib 层人天 0.5-1 天。

1. **GLib.KeyFile 10 处 API**
2. **GLib.DateTime / TimeZone 6 处**
3. **GLib UTF-8 3 处**
4. **Gio.File 实例方法 13 个**
5. **D-Bus 完整栈**：`ArgInfo/MethodInfo/SignalInfo/InterfaceInfo` 的 lgi 类构造计为 API 调用而非类型注解

---

## 附 E：本次更新说明（2026-09-24 校定）

1. **§三·空表安全性叙述推翻**：原文「GLib / Gio / Rsvg / GObject 都留 `{}` 是安全的」在 Part 6 交付 Gio 时失效——`{}` 让 `pcall` 回退静默成功，所有 `Gio.File.xxx` 返回 nil、冒烟只查自己 preload 所以全绿、上线才崩。见 `record2.md 篇八`。
2. **新空表约定（三档）**：
   - **有真实 lib/ 调用方的模块**（GLib / Gio）：**必须交付完整 shim 或留 nil**，`{}` 静默通过「存在性检查」是致命的。
   - **待交付模块**（Rsvg）：`{}` 可保留但需在顶部声明「暂无调用方」。
3. **GLib / Gio 交付状态更新**：本 scope 定的 GLib 人天 0.5-1、Gio 人天 1.5-2 估算，实际落地 GLib Part 5（1.5 人天）、Gio Part 6（2 人天）；D-Bus 完整栈**未交付**，`naughty/dbus.lua` 仍是启动即炸阻塞项（见 `record2.md 篇八`）。
4. **顶部标注**：加一行「本文覆盖 GLib / Gio 范围与 API 明细。逐 Part 执行见 `2026-09-23-record2.md` 顶部对照表」。
