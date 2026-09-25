# 子任务回执 · GdkPixbuf.pixbuf_get_from_surface 实现（2026-09-25 20:30 +08）

> **性质：** 并行子任务返回的完整回执原文，未经裁剪。
> **任务：** 审计 P2 第 16 项。`GdkPixbuf.pixbuf_get_from_surface` 未实现，
> 调用方是 `spec/gears/surface_spec.lua:50,57`。
> **产物：** `lib/vendor/lgishim_pixbuf.c`（+156）、`lib/vendor/lgishim_cairo.c`（+27）、
> `lib/vendor/lgishim.lua`（+6）。
> **模型：** 子任务用主任务模型（`model_tier=strong`）。

---

## 完成

**改了 3 个文件**（未 commit / add / push）：`lib/vendor/lgishim_pixbuf.c`（+156 行）、`lib/vendor/lgishim_cairo.c`（+27）、`lib/vendor/lgishim.lua`（+6）。

### 1. 关键发现：`gdk_pixbuf_get_from_surface` 在本工具链里根本不存在

任务给的 C API 直接调会「编译过、链接炸」：
- `/usr/include/cairo/cairo-features.h` 无 `CAIRO_HAS_GDKPIXBUF_FUNCTIONS`，cairo.h 里那段声明整段缺失；
- `nm /usr/lib/libcairo.a` 和 `nm -D libcairo-2.so.2` 对 `gdk_pixbuf` 匹配数 **0**；
- 顺带 `gdk_pixbuf_new_from_copy` 也不存在（容器里 /usr/local 手编的 gdk-pixbuf 2.42 没有）。

所以写了 `#if defined(CAIRO_HAS_GDKPIXBUF_FUNCTIONS)` 双层：有胶水就调原生，没有就走 `surface_to_pixbuf()` 手抄像素——**`draw.c:85` 的 `draw_surface_from_pixbuf()` 已经在反向手抄，这就是这个 fork 的既有做法**，我的是镜像。

### 2. 新增函数位置
| 位置 | 内容 |
|---|---|
| `lgishim_pixbuf.c:441-476` | `pixbuf_get_from_surface`（模块级） |
| `:459-462` | `w<=0 \|\| h<=0` → `luaL_error` |
| `:345-439` | `surface_to_pixbuf()` 回退路径（`create_similar` → 非 IMAGE 则退到 `cairo_image_surface_create` → paint → 逐行 memcpy → `gdk_pixbuf_new(RGB,TRUE,24,w,h)`） |
| `:524-530` | 挂在**模块表**：`lua_pushcfunction` + `lua_setfield(-2, "pixbuf_get_from_surface")`，紧跟在 `Pixbuf` 的 `setfield` 之后——那次 setfield 已弹掉类表，模块表重新在栈顶，cfunction 正确绑定到模块表。没进 `PIXBUF_M` |
| `lgishim.lua:40-45` | `M.Gdk = M.GdkPixbuf` |

### 3. 怎么取裸 `cairo_surface_t*`
仿 `lgishim_pango_check_layout` 的既有跨 TU 拆分，在 `lgishim_cairo.c:95` 加原型、`:677-693` 定义导出 `lgishim_cairo_check_surface()` 返回 `w->p`，`chk_surface` 改为委托它。比硬转多了一道 `w->cls != C_SURFACE` 检查——跨模块传进来的可能是任意 full userdata，否则 GdkPixbuf wrap 会被当 surface 解引用。

### 4. wrap / destroy 复用
和 `pixbuf_new_from_file` 逐字一致：`return push_obj(L, pixbuf, d_pixbuf, C_PIXBUF)`。`d_pixbuf` = `g_object_unref`，`__gc`/`release()` 不动。pixbuf 持有**自己的**字节副本（`gdk_pixbuf_new` + memcpy，不借用 surface 内存），所以无 borrowed pointer、引用只释放一次。返回值满足 RGBA / n_channels=4 / has_alpha=TRUE / depth=24。

过程中修掉两个自己的问题：`create_similar` 收到的是 `cairo_content_t`（`0x3000`）不是 `cairo_format_t`（`ARGB32==0`）——传错既是 `-Wenum-conversion` 告警又是语义 bug；以及一处错误分支漏 `cairo_surface_destroy`。

### 5. 单 TU 编译（未跑 ninja / busted / 未产出可执行文件）
用仓库自己的 flag 集（`-std=gnu99 -Wall -Wextra -Wundef -Wmissing-prototypes -Wstrict-prototypes -Wno-unused-parameter -Wuninitialized -Wsign-compare -Wformat-nonliteral`）：
```
pixbuf OK (0 warnings)
cairo  OK (0 warnings)
orig (git HEAD 版 pixbuf.c) rc=0   ← 基线无回退
```
lgishim_cairo.c 需要额外 `pango pangocairo` 的 cflags（任务给的命令不含，会报 `pango/pangocairo.h: No such file`）。`luac -p lgishim.lua` 通过。另用 `nm -u` 逐个核对：14 个 `cairo_*` 全在 libcairo、`gdk_pixbuf_new/get_pixels/get_rowstride` 全在 libgdk_pixbuf-2.0.a、`lgishim_cairo_check_surface` 在 lgishim_cairo.o 里是 `T`。

### 需要你决策的两件事
1. **下一个阻塞点**：`surface_spec.lua:58` 接着调 `pbuf1_in:get_pixels()`，而 `PIXBUF_M`（`:502-507`）仍无 `get_pixels`——本次改动后这 10 个测试会晚一帧挂在 `method 'get_pixels'`。约 6 行（`gdk_pixbuf_get_pixels` + `w*h*n_channels` → `lua_pushlstring`，同文件即可），但那是第二个 API 且可能有并行子任务在改，我没动，交给你定。
2. **`M.Gdk` 别名是 shim 特有的**：上游 lgi 的 Gdk namespace 也不带这个函数（`docs/sam/subagent/2026-09-25_1518-spec-shim.md:38` 记过，该 spec 本身的写法是错的），没有这行别名会先挂在 `index a nil value (upvalue 'gdk')`。全仓库 `grep Gdk` 确认无其他消费者，无冲突。

工作树里 `lgishim_pango.c`、`spec/gears/filesystem_spec.lua` 有别人的未提交改动，我没碰。

---

## 主任务复核（追加，非子任务原文）

### 验证了工具链缺胶水的判断
```
grep -c CAIRO_HAS_GDKPIXBUF_FUNCTIONS /usr/include/cairo/cairo-features.h  → 0
nm /usr/lib/libcairo.a | grep -c gdk_pixbuf                                → 0
nm -D /usr/local/lib/libcairo-2.so.2 | grep -c gdk_pixbuf                 → 0
grep -n draw_surface_from_pixbuf draw.c                                    → :85, :190
```
判断成立，手写回退路径是这条工具链唯一可行做法。

### 发现并修掉一个真实 bug：`gdk_pixbuf_new` 第 4 参数不是 depth

子任务写的 `gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 24, w, h)` 里 **`24` 是 depth，
但第 4 参数是 `bits_per_sample`**。depth 由 `bits_per_sample * n_channels` 推出，
所以 `TRUE` + `24` 得到 bits_per_sample = 6，触发：

```
(process:23751): GdkPixbuf-CRITICAL **: gdk_pixbuf_calculate_rowstride:
assertion 'bits_per_sample == 8' failed
```

`surface_to_pixbuf()` 因此返回坏 pixbuf，happy path 抛
`"failed to read the surface"`。**注释也跟着错了**（写「depth 24」，实为 24 当
bits_per_sample 用）。已改成 `8`，并改写注释说明参数语义。

修法：`lib/vendor/lgishim_pixbuf.c:428` → `gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, w, h)`
→ depth 32 / n_channels 4 / bits_per_sample 8，与上游 `gdk_pixbuf_get_from_surface()`
承诺的形状一致。

### 运行时验证（真二进制 + Xvfb，非单 TU 编译）

用一份 rc.lua probe 直接跑安装产物：

```
PIXBUF-PROBE
  shim                   true
  module fn              function          ← 模块表注册生效
  alias Gdk->GdkPixbuf   true              ← M.Gdk 别名生效
  alias gdkpixbuf        true
  ImageSurface.create    nil               ← 另一个缺口（见下）
  ImageSurface.create_for_data function
  happy path             ok type=userdata  ← 修复后通过
  w=0 reject             RAISED: width and height must be positive, got 0x8
  h<0 reject             RAISED: width and height must be positive, got 8x-1
  arg1 nil               RAISED: expected cairo.Surface at arg 1, got nil
  arg1 non-wrap          RAISED: expected cairo.Surface at arg 1, got table
  arg1 context wrap      RAISED: expected cairo.Surface at arg 1, got class 1
```

6 条错误路径全部按预期抛清晰错误、无 segfault；`GdkPixbuf-CRITICAL` 归零。
修复后重新 `ninja` + 覆盖安装，happy path 由 RAISED 转为 ok。

### 附带发现（未处理，超出这三项范围）

- **`cairo.ImageSurface.create` 也不存在**（probe 实测 `nil`）。shim 只有
  `create_for_data(pixels, fmt, w, h, stride)`，而 `lib/` 只有 `dbus.lua:117` 用后者，
  所以 lib/ 侧不构成缺口；但 probe/测试要用 `create` 就得自己换。
- **`get_pixels` 未实现**：`grep get_pixels lib/` 为空 → 只有 spec 用。
  子任务正确地把这个留给主任务决策，我没扩范围。
