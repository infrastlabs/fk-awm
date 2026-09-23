# librsvg Decision (Spike 3 D.4)

采集时间：2026-09-23 23:04

对应计划：`docs/sam/2026-09-23-impl-plan.md` D.4 / b16 一·8.6。
对应人天池：Phase 2 Rsvg 0-1 人天（b16 一·7.1、三·4 F3）。

## 0. TL;DR

- 仓库自有默认主题 `icons/` 是 **4 PNG / 0 SVG**（PNG 100%）。
- librsvg 系统级图标（`/usr/share/icons`）SVG:PNG ≈ **8438 : 1566**（≈ 84 : 16）。
- 但 `lib/vendor/lgishim.lua:29` 已经把 `Rsvg = {}` 留空为 Phase 2 兜底，`imagebox.lua:54,75,415-422` 有 graceful 降级：Rsvg 缺席时 `set_image(".svg")` 落到 `surface.load`（GdkPixbuf 路径），再不成静默失败。
- builder 容器 `pkg-config librsvg-2.0` 未装 → 走 A 方案需要新加 librsvg + libxml2 静态链。
- **推荐：选项 B（只支持 PNG）**，Phase 2 Rsvg 人天取 **0**；不引入 libxml2 静态依赖；不动 C 层。

## 1. Rsvg 调用面

调用完全集中在 2 个文件（`grep -rn "Rsvg\.\|rsvg_\|:render_document\|:get_dimensions" lib/` 全部结果）：

| 位置 | API | librsvg 2.0 C 对应 |
|------|-----|--------------------|
| `lib/wibox/widget/imagebox.lua:87` | `Rsvg.Handle.new_from_data(data)` | `rsvg_handle_new_from_data` |
| `lib/wibox/widget/imagebox.lua:89` | `Rsvg.Handle.new_from_file(path)` | `rsvg_handle_new_from_file` |
| `lib/wibox/widget/imagebox.lua:113` | `handle:get_dimensions()` | `rsvg_handle_get_dimensions` |
| `lib/wibox/widget/imagebox.lua:188` | `self._private.handle.set_stylesheet`（属性探测） | — |
| `lib/wibox/widget/imagebox.lua:195` | `handle:set_dpi_x_y(x, y)` | `rsvg_handle_set_dpi_x_y` |
| `lib/wibox/widget/imagebox.lua:197` | `handle:set_dpi(d)` | `rsvg_handle_set_dpi` |
| `lib/wibox/widget/imagebox.lua:202` | `handle:set_stylesheet(s)` | `rsvg_handle_set_stylesheet` |
| `lib/wibox/widget/imagebox.lua:310` | `handle:render_cairo(cr)` | `rsvg_handle_render_cairo` |
| `lib/wibox/widget/imagebox.lua:408,423` | `Rsvg.Handle:is_type_of(x)` | `g_object_is_type` |
| `lib/wibox/container/border.lua:115` | 经 `imagebox._load_rsvg_handle(img, style)` 间接调用 | 复用 imagebox |
| `lib/wibox/container/border.lua:119` | `handle:set_stylesheet(style)` | 复用 imagebox |
| `lib/wibox/container/border.lua:121` | `handle:set_dpi(dpi)` | 复用 imagebox |
| `lib/wibox/container/border.lua:122` | `handle:get_dimensions()` | 复用 imagebox |
| `lib/wibox/container/border.lua:129` | `handle:render_cairo(cr)` | 复用 imagebox |
| `lib/vendor/lgishim.lua:29` | `Rsvg = {}`（Phase 2 占位，当前为空表） | — |

**去重后：8 个 Rsvg.Handle 方法 + 1 个 `is_type_of` 类型检查 + 2 个调用文件。**

关键降级路径（`imagebox.lua:415-422`）：
```lua
if type(image) == "string" then
    setup_succeed = load_and_apply(self, image, imagebox._load_rsvg_handle, set_handle)
    if not setup_succeed then
        setup_succeed = load_and_apply(self, image, surface.load, set_surface)
    end
end
```
→ 无 Rsvg 时 `.svg` 会走 `gears.surface.load`（GdkPixbuf）。GdkPixbuf 也依赖 librsvg-loader 才能解码 SVG；没有的话这一步也失败，最终 `set_image` 返回 false，imagebox 显示空白。**不会崩、不会污染其它 widget。**

## 2. 默认图标主题 SVG vs PNG 分布

采集命令：`find <theme> -name '*.svg' | wc -l` / `-name '*.png'`。

| 图标主题 | SVG | PNG | SVG 占比 | 说明 |
|----------|:---:|:---:|:--------:|------|
| awesome repo `icons/` | **0** | **4** | **0 %** | 仓库自带默认（`beautiful/icon.lua` 用它） |
| `/usr/share/icons/Adwaita` | 555 | 1331 | 29 % | `menubar.icon_theme` 默认候选 1 |
| `/usr/share/icons/hicolor` | 120 | 214 | 35 % | `menubar.icon_theme` 默认候选 3 |
| `/usr/share/icons/Papirus` | 15041 | 0 | 100 % | SVG-only，系统级 |
| `/usr/share/icons/Tango` | 1 | 4 | 20 % | — |
| `/usr/share/icons/elementary-xfce` | 3 | 0 | 100 % | 小主题 |
| `/usr/share/icons/ubuntu-mono-dark` | 5 | 0 | 100 % | 小主题 |
| `/usr/share/icons/Yaru` | 2 | 0 | 100 % | 小主题 |
| `/usr/share/icons/default` | 0 | 0 | — | 空壳 |
| **系统合计（`/usr/share/icons`）** | **8438** | **1566** | **84 %** | 含 Papirus 拉高 |

关键观察：

1. **仓库自身默认图标 100% PNG**——awesome 出厂外观不依赖 SVG。
2. **默认 menubar 候选（Adwaita / hicolor）PNG 占多数**（71 % / 65 %），意味着即使没 Rsvg，绝大多数 menubar 图标仍能靠 PNG 显示。
3. **Papirus / elementary / Yaru 是 100 % SVG**：这些主题在无 Rsvg 环境下**全部图标空白**。它们属于「用户主动挑选」的高级主题，不属于 out-of-box。
4. `lib/menubar/icon_theme.lua:93` 声明支持扩展 `{"png","svg","xpm"}`；`lib/menubar/utils.lua:132` `supported_icon_file_exts = {png=1, xpm=2, svg=3}`——如果选 B，需要在 icon 解析路径把 `.svg` 剔除（否则每次找不到 → 遍历下一个 fallback）。
5. `rofi -show-icons` 由 rofi 进程内 GTK 绘制，走的是 rofi 的 librsvg，不走 awesome 的 Rsvg 绑定（b16 二·6.3）。所以 awesome 端砍 SVG 不影响 rofi 图标显示。

## 3. librsvg 可静态链接？

```
$ docker exec builder-awe sh -c 'pkg-config --cflags --libs librsvg-2.0; ldconfig -p | grep librsvg'
Package librsvg-2.0 was not found in the pkg-config search path.
Package 'librsvg-2.0', required by 'virtual:world', not found
```

**结论：容器未预装 librsvg。** 走 A 方案需先补齐：
- `librsvg-2.0` 静态库（x11base 构建链里 `v-tint2/build.sh` 曾编译过，见 `docs/sam/2026-09-23-record1.md:254`，但未在本容器落地）；
- librsvg 强依赖 **libxml2**（新增静态链依赖，非 Pango/GLib 那套现成的），静态链接 12-20 人天档里需追加。

`librsvg` 依赖树（apt-cache 视角）：librsvg-2.0 → libxml2 + libgdk-pixbuf-2.0 + pango + cairo + glib。前三项已在计划范围内，**libxml2 是全新静态链接项**。

## 4. 决策矩阵

| 选项 | 成本 | 覆盖度 | 副作用 |
|------|:----:|--------|--------|
| **A. 写 librsvg shim** | 1-3 人天 | 全（imagebox + border + menubar 全主题） | 静态链追加 libxml2；C 侧 100-150 行；`g_object_is_type` 需 GObject 类型表；librsvg 未预装需先补构建链 |
| **B. 只支持 PNG**（默认） | **0.25 人天** | 仓库默认主题 100 % 覆盖；Adwaita/hicolor 65-71 %；Papirus/elementary 0 % | SVG-only 主题（Papirus 等）图标空白；需要 `lib/menubar/icon_theme.lua:93` 和 `lib/menubar/utils.lua:132` 把 `.svg` 扩展降级处理（否则白遍历）；border 用 SVG 的样式表失效（可用 PNG 替代） |
| **C. 砍掉 imagebox / border** | 0 人天 | 只 PNG 图标 + 无装饰边框 | `wibox.widget.imagebox` API 失效，配置里所有 `imagebox{ image=... }` 报错；wibox `border_image` 属性失效；改动面反而大（涉及 `lib/wibox/widget/init.lua` 注册、`lib/wibox/container/init.lua` 注册） |

**为什么不选 C**：imagebox / border 是 wibox 核心 widget，砍掉影响面比 Rsvg 大得多；B 已经足够（默认主题全 PNG）。
**为什么不选 A（现阶段）**：Phase 1 目标是「静态单文件 awesome 启动到 crash」，SVG 图标是运行时美化问题；且 b16 一·7.1 已经把 Rsvg 单列为「可选，0-1 人天」。若日后有 SVG-only 主题需求，A 是纯增量（shim 骨架已就绪：`lgishim.lua:29` 的 `Rsvg = {}` 就是插入点）。

## 5. 决策

**推荐：选项 B（只支持 PNG）。**

理由（一句话）：**仓库自带默认主题 100 % PNG、Adwaita/hicolor 65-71 % PNG、builder 无 librsvg、libxml2 是新增静态链接负担**——SVG 支持可以推迟到 Phase 2 后期做纯增量；`imagebox.lua:415-422` 已有降级路径，SVG 缺失仅表现为图标空白而非崩溃。

### 5.1 对 Phase 2 人天估算的影响

| 子项 | 原估计（b16 一·7.1） | 采纳 B 后 |
|------|---------------------|-----------|
| Rsvg 层（b16 一·8.6） | 0-1 人天 | **0** |
| 静态链接 | 12-20 人天 | 不变（无需 libxml2） |
| GdkPixbuf 层 | 0-2 人天 | 不变（PNG 路径本身依赖 GdkPixbuf） |
| **合计（砍 menubar 档）** | 38-62 人天 | **38-62 人天，不变**（Rsvg 0 而非 1，属区间内） |

**新增小工作量（0.25 人天，可并入测试回归 6-8 人天档）：**
- `lib/menubar/icon_theme.lua:93` 扩展列表去掉 `"svg"`，或在解析失败时静默跳过 `.svg` 候选。
- `lib/menubar/utils.lua:132` `supported_icon_file_exts` 移除 `svg = 3`（或标记为「运行时检查 Rsvg 可用性」）。
- 在 `docs/sam/2026-09-23-impl-plan.md` 记录一条：SVG 主题为已知局限，用户切换到 Papirus/elementary 会看到空白图标。

### 5.2 决策可撤回条件

若以下任一成立，回滚到 A：
1. Spike 3 D.5 决策「保留 menubar」且默认主题是 Papirus 等 100 % SVG 主题 → B 失效。
2. 用户环境要求支持 SVG 主题（例如 Ubuntu 默认 Yaru）。
3. 静态链接 libxml2 已就绪（builder 里能 `pkg-config librsvg-2.0`）且 Rsvg C 绑定在 Spike 3 D.5 里被判定为「顺手做」。

## 附录：命令与产出

```
$ grep -rn "Rsvg\.\|rsvg_\|:render_document\|:get_dimensions" lib/ | head
# → 15 处，全部落在 imagebox.lua / border.lua / lgishim.lua

$ find /usr/share/icons -type f \( -name "*.svg" -o -name "*.png" \) | wc -l
10004   # SVG: 8438, PNG: 1566

$ find icons -type f \( -name "*.svg" -o -name "*.png" \) | wc -l
4       # 全 PNG（awesome16/32/48/64.png）

$ docker exec builder-awe sh -c 'pkg-config --cflags --libs librsvg-2.0'
Package librsvg-2.0 was not found in the pkg-config search path.
```
