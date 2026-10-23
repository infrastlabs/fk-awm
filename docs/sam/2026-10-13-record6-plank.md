# AwesomeWM 全静态构建 · Phase 2 记录（续六）：plank dock 点击拦截与窗口穿透系列（2026-10-13）

> 📌 **来源**：本文由 `docs/sam/2026-09-28-record5.md` **第八章**拆出，拆分日期 **2026-10-24**。
> 章节**序号原样保留**（未重编号）；拆出时同步修正标题层级错位（共 30 处 `#` 数量调整，
> **正文一字未动**）：原 `### 3.1/3.2` 与 `### 1/2/3/4` 同级 → 下沉为 `####`，子节
> `#### 3.x.x` → `#####`；原 `##### 4.3~4.7` 比 `#### 4.1/4.2` 深一级 → 上提为 `####`，
> 子节 `###### 4.x.y` → `#####`（`4.3.2.x` 多一级序号，仍为六层）。
> 层级现状：`## 八、` → `### 1/2/3/4/5` → `#### 2.x / 3.1 / 3.2 / 4.x / 5.1 / 5.2` →
> `##### 3.x.x / 4.x.y / 5.1.x` → `###### 4.3.2.x`（**序号层级 == 标题层级**）。
> **2026-10-24 结构调整（当天两次）**：
> 1. 新建第 5 节（`### 5、`）——当时编号为 `#### 4.5` 的「历史文档全景」原样移入为
>    `#### 5.1`（子节 4.5.x → 5.1.x），当时编号为 `##### 4.7.3` 的「代码变更回顾 +
>    风险分析」**上调一级**为 `#### 5.2`；
> 2. **序号前移填空号**——当时编号 `#### 4.6`（popen 根因）→ `#### 4.5`（子节
>    4.6.x → 4.5.x），当时编号 `#### 4.7`（用户理解 + shaped 识别）→ `#### 4.6`
>    （子节 4.7.x → 4.6.x）。**故本文现出现的 4.5 / 4.6 均为前移后编号**；第 5 节
>    标题里的「4.5 / 4.7.3」指前移前的编号。
> 两次均只改序号与 `#` 数量并同步交叉引用，正文一字未动（5.2 开头补一条
> 「`9464b0a0a` 时代」过时标注）。
> 拆出原因：record5.md 体积过大（原 2024 行，本章占 729 行）。
>
> **去向**：原文件第八章位置保留占位标题「## 八、（已拆出）…」并指回本文，
> 章节序号保持连续（七 → 八〔占位〕 → 九）。
>
> 正文中提到的「八·1 / 八·2 / 八·3」「record5 八」「record5 第八章」均指本文
> （即原 record5 第八章），历史表述未改动。
>
> **本文件内容范围：**

| 节 | 主题 | 覆盖 |
| --- | --- | --- |
| 1 | 核验与根因修复 | plank input shape 未传播到 frame |
| 2 | 复核实现与独立问题 | 2.0 模拟验证 / 2.1 部署复跑 / 2.2 10x10 问题 |
| 3 | 穿透系列 | 3.1 最大化缝隙 + 3.2 拖大死区 |
| 4 | 底部穿透与 shape 全链 | 4.1 核实 ~ 4.6 识别机制 |
| 5 | 文档全景与风险分析 | 5.1 两仓全景 / 5.2 代码变更 |

> **相关文档**：`2026-09-28-record5.md`（收尾记录：一~七章、九章）、
> `2026-10-19-actions.md`（三架构出包工作流与 GH Actions 实跑）、
> `fk-awesome-wm-theme/docs/adjustment-log-01~04.md`（主题侧 plank 系列排查）。

---

## 八、plank 点击拦截与穿透修复（2026-10-13）

> 现象：启动 plank（合成器 picom 在跑时 plank 膨胀到全宽）→ 底部一整行透明
> 遮罩拦截点击，区域内的窗口无法操作。

### 1、maki> 核验与根因修复

**maki> 的要点**：plank 窗口比可见 dock 大（为图标放大动画/阴影预留），透明区靠 X
"input shape" 实现点击穿透；openbox 正确应用 → 透明区穿透；awesome 不处理 →
整块 frame 矩形吃点击；xcompmgr/picom 是扳机（plank 检测到合成器就膨胀，实测
1280x137 vs 无合成器 314x63）。

**核验结论**：

| 论断 | 判定 |
|---|---|
| 机理（input shape + frame 吃点击） | ✅ 实测确认：plank 客户窗 input shape = `\`314x57+483+80\``（真形状），awesome 的 frame 无对应形状 → 全矩形拦截 |
| picom/合成器是膨胀触发器 | ✅ 实测：有 picom = 1280x137 全宽；无 = 314x63 |
| "awesome 连 API 都没有" | ❌ **不成立**：源码有 `client.shape_input` 属性（`objects/client.c:4260-4338`）、`xwindow_get_shape(win,INPUT)`/`xwindow_set_shape(...INPUT...)`（`xwindow.c:264/380`）、`globalconf.have_input_shape`（`awesome.c:773`）、ShapeNotify（`event.c:939`） |

**真缺的一环**：客户端自己设的 input shape 不会自动同步到 awesome 的 frame 窗口。
完整主题 rc 下实测 frame = 1280x137、input 全开（全矩形吃点击）。

**修复**（`event.c` + `objects/client.c`）：

1. `event_handle_shape_notify`：`XCB_SHAPE_SK_INPUT` 时读客户端 input shape，
   应用到 frame（尺寸 = 几何 + border×2，offset = -border_width），并emit
   `property::shape_client_input` 信号；shape 移除（surf NULL）时重置为全矩形。
2. `client_manage`：几何赋值后做一次初始同步（客户端在 manage 前已设 shape
   的情况）。
3. `event.c` 补 `#include <cairo.h>`。

**验证**：插桩确认 `xwindow_get_shape` 返回非 NULL（读到 plank 有效 shape
surface）+ `xwindow_set_shape` 实际执行；合成测试环境 frame/客户窗几何随 rc 与
时序抖动（frame 有时 10x10 有时 1280x137），端到端点击穿透**待真机确认**。

**状态**：🔶 已实现待真机验证。备选路径（maki>）：1 停 picom（plank 缩回
314x63）2 关 plank 缩放动画 + IconShadowSize=0。

### 2、复核实现与独立问题（2026-10-13）
#### 2.0、Xvfb 模拟验证（2026-10-13）

> 环境：Xvfb 1280x800 + awesome（部署后的修复版） + picom + plank + 完整主题
> rc（`/tmp/awm_env/awesome_vfy/rc.lua`）。判定方法：`xdotool getmouselocation`
>（XQueryPointer 按 **input shape** 返回真实点击目标窗口，shape 排除区返回 None）。

**采样点实测**（y=750，处于 plank 的 1280x137 覆盖带内）：

| 采样点 | 点击目标 | 输入区 | 判定 |
|---|---|---|---|
| (150,750) 透明区 | root | 1280x800 | ✅ 穿透 |
| (600,750) dock 图标区 | dock 窗口 | 376x57+452+80 | ✅ dock 正常响应 |
| (1100,750) 透明区 | root | 1280x800 | ✅ 穿透 |

**结论**：修复生效——透明区点击落到 root/下层窗口，不再被拦；dock 图标区
正常。`client.c` manage 同步 + `event.c` ShapeNotify(INPUT) 传播的机制在
Xvfb 模拟确认。

**边界（如实记录）**：测试环境里 awesome 的 frame 尺寸不稳定（min_rc 下恒
10x10 盖不住客户窗；完整主题 rc 下有时 1280x137 全输入区、有时 10x10），
"frame 撑满整条拦截"这一用户的真实形态在合成环境无法稳定复现——旧二进制在
同配置下也常显示穿透。因此端到端**最终确认仍依赖真机**（`Mod+Ctrl+r` 重载后
起 plank 实测）。frame 未跟随客户窗几何（停在 10x10）本身疑似独立问题，
待查。


#### 2.1、`✅ 已验证` 部署二进制复跑（2026-10-13）

> 用**当前部署的二进制**（`/usr/local/static/awm/bin/awesome`，2026-10-13 11:26，
> 静态 3.6MB）复核第 2 节结论：Xvfb 1280x800 + **picom 先行** + 完整主题 rc
> （`awesome_vfy` 全树拷贝、autostart 去掉 xcompmgr 避免双合成器）。
> 本次成功复现**膨胀形态**：plank 窗口 `1280x137+0+663`，带内三点采样：

| 采样点 | 点击目标 | 判定 |
|---|---|---|
| (150,750) 左边透明区 | root | ✅ 穿透 |
| (600,750) dock 图标区 | plank 客户端 | ✅ dock 正常响应 |
| (1100,750) 右边透明区 | root | ✅ 穿透 |

**结论**：8·1 状态 `🔶 已实现待真机验证` → **✅ 部署版生效确认**——透明带点击
穿透在膨胀形态下成立，与 8·2 模拟验证一致。（注：仅靠 `xwininfo -shape` 直接
列 frame 形状在本环境不好套用——frame/客户窗 id 需从 reparent 关系里精确辨识，
行为采样更直接可靠；真机复核仍按 8·2 末尾建议执行。）

**合成器无关加测**：同一复核换 **xcompmgr 先行**（主题 autostart 同款）复跑，
plank 同样膨胀 `1280x137`、三点采样结果与 picom 完全一致 → 修复与合成器种类
无关。早前在 xcompmgr 下观察到的 314x63 属**启动竞态**（plank 先于合成器起），
不是合成器机制差异。

**真机回报（2026-10-13）仍失败**：另一台机器上带内「桌面右键菜单」不响应——
桌面上的窗口能点、原始桌面点不动、关 plank 即恢复。本环境**事件级补测通过**：
xcompmgr 先行（plank 几何实测 `1280x137` 确认膨胀）+ xfdesktop 桌面窗 +
客户端 `button::press` 探针计数 → 带内 (150/1100,750) 点击**确实落到桌面窗**
（计数 +1）、dock 区 (600,750) 归 dock（不计）、关 plank 后同点位正常。
部署版（`v4.3-1848-g5316bb76f`）事件投递成立 → **真机差异待查**，第一嫌疑是
该机仍跑修复前二进制。真机对照命令：
`awesome -v`（须 `…1848-g5316bb76f` 或更新 sha）、
`xwininfo -root -tree | grep -i plank`（须 `1280x137` 而非 `314x63`）、
`xdotool getmouselocation`（带内须 root/桌面窗而非 plank 或 frame）、
`pgrep -ax 'xfdesktop|pcmanfm|nemo'`（看桌面右键菜单的宿主）。

**✅ 已关闭（2026-10-13）**：用户确认该机换的正是**未更新**的修复前 awe；
重新部署修复版后，带内桌面右键菜单恢复正常 → 8·1 修复全链闭环，无遗留。
（真机复核结论：之前 Xvfb 事件级通过 + 真机更新后通过 = 部署版即修复版，
无需进一步改动。）

#### 2.2、10x10 几何独立问题（2026-10-13）

> 触发：plank 排查中顺带发现——测试环境里 awesome 的 frame 恒为 10x10，
> 客户窗却 1280x137 溢出。分析结论：**独立于 input shape 的第二个问题**：
> awesome 的 frame 不跟随"自管理几何"客户端的窗口变化。

**机理链路**

1. `client_manage`（`objects/client.c:2147`）用客户窗**当时**几何创建 frame
   ——plank 初建 10x10 → frame 10x10。
2. 客户窗后续自改尺寸（reparent 后子窗仍归客户，`XResizeWindow` 合法）→
   plank 窗 1280x137，frame 不自动跟。
3. `event_handle_configurenotify`（`event.c:458`）**只处理 root**（屏幕刷新），
   managed client 自己的 ConfigureNotify 零处理 → `c->geometry` 与 frame 不跟进。
4. frame 唯一变尺寸的路：`client_geometry_refresh`（frame configure at
   `client.c:2006`）由 `c->geometry` 驱动；`c->geometry` 只来自 Lua 侧
   `c:geometry()`（布局/规则/拖动）或 `request::geometry` 被 Lua 接受
   （`event.c:404`）。极简 rc 无 handler → 请求丢弃 → 永恒 10x10。

**判定**

- 独立问题 ✓：与 input shape 传播无关。
- 正常客户端无感：awesome 用布局/规则自定几何，frame 恒贴合。
- 对 plank 的纠葛：客户窗溢出 10x10 frame 时，点击解析由客户窗自己的
  input shape 决定（frame 盖不住不参与）→ min_rc 下"碰巧穿透"；用户真机被拦 =
  环境里 frame 被撑满（request::geometry 被接受）→ 正是 shape 传播修复的场景。
- 若 frame 恒贴合客户几何：plank 的 frame 必为 1280x137，点击穿透完全由
  shape 传播机制决定，两条线合流。

**修法思路（✅ 已实施，后判定误认已回退）**

- C 侧 `event_handle_configurenotify` 加 managed client 分支：客户窗自改几何 →
  更新 `c->geometry` + 触发 `client_resize_do`/refresh 让 frame 包住客户
  （参考 openbox track-client-config）。
- 需按窗口类型仲裁：普通客户由 WM 布局（自 resize 通常忽略），dock/override
  尊重客户——避免与布局/规则冲突。

> **2026-10-13 实施注**：已实现——`event_handle_configurenotify` 对 managed
> client 跟踪自改几何（客户尺寸+titlebar → `client_resize(c, geo, false)`）。
> Xvfb 插桩验证：handler 触发、`c->geometry` 与 `x11_frame_geometry` 均到
> 1280x137（账簿正确），**但 X 上 frame 窗口仍 10x10——账簿与 X 脱钩**（frame
> 的 X configure 未落地或被执行后回退，未定位，需 X 层跟踪）。改动保留（无害），
> X 脱钩另查。

> **2026-10-13 复评（质疑后修正）**：子项（frame 自改几何跟踪）在**真机环境
> 无额外效果**——主修复（input shape 传播，已部署）已覆盖点击问题；子项针对
> 的"无 request::geometry handler 的 rc + 客户端直改尺寸"边缘场景，实测时
> 账簿本就正确（`c->geometry`/`x11_frame_geometry` 已到 1280x137），且真正
> 的错误（X 层 frame 窗口恒 10x10）它管不着。**判定：零增益的无害补丁**。
>
> **真正未解决**：frame 的 X configure 未落地/被回退（账簿 1280x137 vs X
> 窗口 10x10 脱钩）——需 X 层 ConfigureNotify 跟踪专项排查。
>
> **2026-10-13 定论（查清后推翻本小节前提）**：「frame 停在 10x10 / X 脱钩」是
> **窗口误认**——一路查询的 `0x800001 "plank" 10x10` 是 **plank 自己的辅助窗口**
> （X 树里 plank 有多个同名窗口），awesome 的真 frame 是**未命名**窗口
> （`0x20000a`）。干净实测：picom 态 frame = **1280x137+0+663 正确包裹客户窗**
> （no-picom 态 314x63 同样正确）；插桩也显示 `FRAME_CFG frame=20000a ->
> 1280x137`。**frame 始终跟随客户几何，无脱钩**。
>
> 八·3 的子项修复（ConfigureNotify 跟踪）**建立在错误前提上**——账簿本已正确
> （插桩证实 c->geometry/x11_frame_geometry 早已 1280x137），属冗余补丁，
> **建议回退**。真正有效且已部署的是 八·1 的 input shape 传播修复。
>
> **2026-10-13 处置**：按用户决定**已回退**子项代码（`git revert 0607133c9`，
> event.c 回到 ConfigureNotify 仅处理 root 的基线）。八·3 本节与子项修复
> 均为窗口误认驱动的产物，留档备查。

### 3、穿透系列：缝隙与拖大死区（2026-10-14~15）
#### 3.1、最大化缝隙点击穿透（2026-10-14）

##### 3.1.1、根因分析与修复方案

> 现象：窗口最大化后几乎铺满，但最右侧 ~20px（真机 dpi 缩放）鼠标左右键
> 穿透到桌面，不再聚焦于窗口。

**根因**：`lib/awful/placement.lua:1375` `placement.maximize`：

```lua
local bw   = (not args.ignore_border_width) and d.border_width or 0
ngeo.width  = sgeo.width  - 2*bw
ngeo.height = sgeo.height - 2*bw
```

- 主题给客户设非零 `border_width`（真机 dpi 放大后一侧 ≈10px）→ **最大化几何 =
  workarea − 2×border** → 右/下侧各留 border 宽的缝隙，点击落入缝隙即穿透
  （用户 ~20px = 2×10px；Xvfb 里 border 2px 所以实测内缩 4px：1276x797）。
- 链：`client_set_maximized_common`（objects/client.c，只设标志并 emit
  `request::geometry("maximized")`）→ `permissions.geometry`
  （lib/awful/permissions/init.lua:365）→ `aplace.maximize`。
- "手动 c:geometry 被核心弹回" = 最大化几何由该函数支配，弹回值即 workarea−2bw。

**对 maki>「C 根因（border_width_maximized 未实装）」的澄清**：那个黑盒
（`beautiful.border_width_maximized*`）是「最大化时换独立边框宽度/颜色」的独立
特性，与本症状无关。**隐藏边框走 lib 层开关，本 fork 已完整支持**：
`permissions.geometry` 里 `original_context=="maximized" and
beautiful.maximized_hide_border==true` → `props.ignore_border_width=true` →
`placement.maximize` 里 `bw=0` → 几何 = **workarea 全铺**。C 侧无需改动。

**修复方案**

- **方案 A（推荐，配置层一行）**：主题 rc 里 `beautiful.maximized_hide_border =
  true`（`beautiful.init` 之后）。效果：最大化 = workarea 全铺、无缝隙、穿透
  消失；最大化窗口视觉无边框（符合预期）。
- **方案 B（备选，仅当 A 实测不生效时）**：再查 C 侧几何应用路径为何未吃到
  全铺值——当前判断 C 不需要改。

**状态**：🔶 待实测验证（Xvfb 主题配置 + knob → 最大化几何应为 1280x775 全铺）
后落地主题仓库。

##### 3.1.2、size hints 取整残差（2026-10-14）

> ⚠️ **推翻 1 节的「knob 一行就好」与 maki> 的「C 侧硬编码」两个判断**。
> 决定性探针（部署版 + xterm，workarea 1280x800）：

| 组合 | 最大化几何 |
|---|---|
| bw=0 + **size_hints_honor=false** | **1280x800+0+0 全铺**（X frame 与客户窗均 1280x800，客户窗无偏移） |
| bw=0 + size_hints_honor=true | 1276x797（−4/−3） |
| bw=10 | 1258x771（再取整） |

**根因修正**：Lua 链**确实在跑**——`placement.maximize` 应用 workarea−2bw；随后
C 的 `client_resize` **honor size hints** 按字符格向下取整 → 1280→1276、
800→797。所以：

- 「硬残差」= **size hints 取整**（xterm 等增量尺寸应用），非 C 硬编码、非
  border 隐藏能消（hide_border 生效后 bw=0 仍被取整）。
- maki> 实测表全对上：bw=5 多缩、knob 无效、1276x769 恒定 = 取整主导。
- 真机 ~20px = **2×border(10px dpi) + size hints 取整** 叠加。

**修复（修正版，配置层可 100% 全铺，无需假最大化/无需改 C）**：最大化时
`border_width=0` + `size_hints_honor=false`（还原时恢复），并在 property 信号里
重跑一次 `awful.placement.maximize(c)` 按全铺计算——已实测 1280x800 全铺。
代价：最大化期间终端字符格错位（文本像素偏移），可接受/按应用取舍。

**状态**：🔶 已定论，主题侧修复待落地验证。

##### 3.1.3、几何不跟随理论核验（2026-10-15）

> 触发：真机最大化后近 1/3 屏幕穿透到桌面，maki> 归因「frame 不跟随 client
> 几何」（client_resize_do 只更新 client 自身窗、frame 靠 client_geometry_refresh
> 维护、且「client.c 内 client_refresh 无调用点」→ resize 路径 frame 从不刷新
> → 增长区输入死区）。

**代码核验**

| maki> 断言 | 核实 |
|---|---|
| client.c 内 client_refresh 无调用点 | ❌ **不实**：`client_refresh()` ← `awesome_refresh()`（event.h:47）← `a_glib_poll`（awesome.c:457，主循环 poll 回调）**每轮都调**；`client_geometry_refresh` 遍历所有 client，`AREA_EQUAL` 不符即 `xcb_configure_window(frame,...)`（client.c:2006） |
| x11_frame_geometry 只在 refresh 更新 | ✅ 对（client.c:2013，与 X configure 锁步写） |

**实测核验**（部署版 + bare rc，floating 客户 `c:geometry(900x600)`）

- frame X 尺寸 **898x589 = 跟随客户窗** ✓——「frame 不跟随」直接 resize 路径**复现不了**。
- **异常发现**：该探针里 frame `Map State: IsUnMapped`（客户窗 IsUnviewable）→ 整窗
  不可见、全区域点击穿透——比死区更严重，疑似 manage 事件里**同步调 c:geometry()
  的时序 artifact**（正常路径未见），待排除。

**结论**：awe 的 frame 刷新链是通的（主循环每轮），直接 resize 下 frame 尺寸跟随
正常；maki>「resize 路径 frame 从不刷新」与代码、实测均不符。真机 1/3 穿透的量级
Xvfb 复现不出，方向更可能是 frame 未映射/事件处理在某种路径的真实缺陷或真机
特有时序——**需真机数据（分辨率 / panel 高度 / 最大化后窗口几何）对症**。

**建议**：
1. 防御性无害改动：`client_resize_do` 末尾补 `client_geometry_refresh()`——frame
   刷新同步化，消除跨 poll 窗口，任何环境不亏。
2. 对症需真机数据；`fee2793` 主题最大化修复（workarea 精确）Xvfb 已验，真机效果
   待确认/按需回退。

#### 3.2、拖大死区点击穿透（2026-10-15）

##### 3.2.1、传播修复引入回归与修法

> 现象：win+右键拖大/移动窗口后，**新覆盖区**鼠标左右键穿透到桌面（旧区正常）；
> 不限最大化。官方默认 rc + 静态版必现；apt 4.3 无；**0926 包（4.3-1910-5247a66）
> 正常**——对照坐实回归点在 5316bb7（plank input shape 传播）。

**根因链**：

1. `5316bb7` 在 manage 时把客户 input shape 同步到 frame：`xwindow_get_shape(INPUT)`
   对**无 shape 客户**（xterm 等）不返回 NULL，而是返回**当前几何尺寸的全窗矩形
   表面**（GetRectangles(INPUT) 无 shape 时给 1 个全窗 rect，不是 0）→ 被当真形状
   应用，尺寸定格在 manage 时。
2. 客户后续 resize（拖大/移动/最大化）时，修复前**从未重建 frame 输入区** →
   frame 输入区停在旧尺寸。
3. X 输入判定**父窗口输入区说了算**：增长区在 frame 输入区外 → 子窗收不到输入 →
   穿透。比例 = 增长量。

**证据**（拖大 484x316 → 988x680 后）：frame 几何 988x701 已跟随 ✓，但 frame
**INPUT SHAPE = [484x316@-1,-1]（旧尺寸）**——死区元凶。

**修复**（`9abfc2182`，event.c + objects/client.c）：

- `client_geometry_refresh` 几何变化分支：按**当前几何**重建 frame 输入区
  （`xwindow_get_shape(c->window, INPUT)` → set_shape(frame, ...)）。普通客户 =
  全窗矩形（无死区），plank 有 shape = 维持穿透。
- manage 同步保留（初始即刻生效），不再有陈旧问题（resize 重建兜底）。

**验证**：拖拽回归——增长区 (601,343)/(951,693) 点击目标 = 客户窗（修复前 root）；
plank 穿透保持（透明区→root、dock 图标区→dock）。包
`awm-static-musl-261015-092431-v4.3-1870-9abfc21`（sha ✓ 已部署）。

> **经验**：`xcb_shape_get_rectangles(INPUT)` 对无 shape 窗口返回**全窗 1 rect**
> （非空列表）——"rects>0 即有 shape"的判定不成立；正确模型 = 始终同步 +
> resize 后按当前几何重建。

### 4、`✅ 已验证` 底部穿透与 shape 全链（2026-10-23）

> 用户反馈：浮动布局窗体底部约一个标题栏高度（24px）穿透——鼠标点窗体底部那条，
> 事件落到下层窗口/桌面。分析见主题侧 `docs/adjustment-log-03.md` 六·6.2（实测+机制）。
> 本节为 awe 侧核对结论 + 修法（源自主题 6.2.4 分析，已逐行对照本仓源码核实属实）。

#### 4.1、机制链核对通过

- **两笔提交时间线吻合**：
  - `5316bb76f`（10-13）`event.c + client.c`：client input shape 传播到 frame（修 plank 透明区拦截）；**副作用即本问题**；
  - `be76ba285`（10-15）`client_geometry_refresh`：几何变化后重放（把空洞从 >30px 缩到 ~24px，**未修干净**）。
- **机制链核实**（fk-awesomewm 源码）：
  1. `xwindow_get_shape` INPUT 分支（`xwindow.c:274-289`）：**不查 input_shaped**，
     直接 `xcb_get_geometry` 取 client 尺寸 → 普通 client（无 shape）返回 **client 尺寸 A1 surface**；
  2. `client_geometry_refresh`（`client.c:2021-2029`）：
     `xwindow_set_shape(frame, geometry尺寸, INPUT, surf, -border_width)`，geometry = frame 尺寸；
  3. `xwindow_shape_pixmap`（`xwindow.c:353-376`）：按 **frame 尺寸**建 pixmap、surf **贴 (0,0)**；
  4. `xcb_shape_mask(SO_SET, INPUT, frame, offset, offset, pixmap)`（`xwindow.c:391`）整体替换。
- **根因**：surf 是 client 尺寸、pixmap 是 frame 尺寸、surf 贴 `(0,0)`——但 client 在 frame 内
  实际下移了 titlebar 高度（官方 rc 20px / 主题 rc 24px）。frame 底部 `frame高 − client高`
  （= titlebar 区）无 shape 像素 → 输入为空 → 点击落 root/下层窗口。
- **为什么 10-10 前无此问题**：无传播代码 → frame 无 input shape = 默认全矩形输入，全窗可点。

#### 4.2、最小修法（awe 侧）

- `xwindow_get_shape` INPUT 分支先 `xcb_shape_query_extents` 查 `input_shaped`：
  - **false（普通 client）→ 返回 NULL**；
  - 调用方对 NULL 已走 `xwindow_set_shape` 的 `surf=NULL → pixmap=XCB_NONE`（`xwindow.c:387-391`
    已核实）→ `xcb_shape_mask(..., XCB_NONE)` = 恢复 frame 默认全矩形输入 → 空洞消失；
  - `input_shaped=true`（plank 类 shaped dock）→ 维持现有传播，穿透保持。
- 顺带（双保险）：`xwindow_shape_pixmap` 建 pixmap 按 surf 实际尺寸，消除 20/24px 恒差。
- ⚠️ 状态：待用户确认后在 fk-awesomewm 落地 + 编译 + 重测（主题 6.2.4.4 同方向）。

#### 4.3、`✅ 已验证` 落地与 Xvfb A/B（2026-10-23）

##### 4.3.1、普通窗底部空洞验证

- **代码**（commit `9464b0a0a`）：`xwindow.c` INPUT 分支先 `xcb_shape_query_extents`
  查 `bounding_shaped/clip_shaped`（X11 无 `input_shaped` 可查——input shape 默认
  = bounding shape；文档 6.2.4.4 说的「查 input_shaped」是笔误，实际用 bounding_shaped）：
  - 普通 client（无 shape）→ 返回 NULL → frame 恢复全矩形输入 → 底部空洞消失；
  - shaped（plank 类）→ 维持原传播。
- **Xvfb A/B 探针**（官方 rc + xterm 604x394，frame 604x415 底 515，titlebar 21px）：

  | 探测点（frame 底部 y） | 旧 10-15 | 修复后 |
  |---|---|---|
  | y=490（底-25，client 内） | xterm ✓ | xterm ✓ |
  | y=494~514（底-21~-1，空洞区） | **root 穿透** | **xterm ✓** |
  | y=518+（frame 外） | root（正常） | root（正常） |

  修复后底部 21px 空洞消失，全窗可点 ✓。

##### 4.3.2、20px 前移与不回归（2026-10-23）

> 上文用术语，补一段人话解释 + 与原始问题的关系 + 修复评估。

###### 4.3.2.1、验证目标：plank 不回归

修普通窗口底部穿透时，还要确认**别把 plank（底部 dock）修坏**——plank 正是当初
引入传播代码的原因（边缘透明区要能点穿到桌面）。造了一个模拟 plank 的窗口（上面
90% 可点、底部 10% 透明区），在**修复前/后**各跑一遍，窗口上从上往下点：

| 点的位置 | 旧版 | 新版 |
|---|---|---|
| 窗口上面（可点区） | 点在窗口上 ✓ | 点在窗口上 ✓ |
| 窗口底部（透明区） | 点穿到桌面 | 点穿到桌面 |

**结论：新版与旧版一模一样，plank 没被修坏 ✓**（5316bb7 原始意图不回归）。

###### 4.3.2.2、「边界 20px 前移」是什么

预期可点区到 y=371，实测 y=320 往上就穿透——可点区比预期少约 20px（≈标题栏
高度）。**旧版就是这样，与本次修复无关**，是更早存在的小毛病（shaped 传播把窗口
形状贴到边框时，起点算偏了一个标题栏高度）。

###### 4.3.2.3、与原始问题的关系

| | 原始问题 | 20px 前移 |
|---|---|---|
| 谁受影响 | 普通窗口（xterm/sakura）底部一整条 | 带透明区窗口（plank）底部一点点 |
| 严重度 | 一整条点穿，明显 | 边缘 20px，几乎无感 |
| 本次已修 | ✅ 已修 | ❌ 未动（非本次活） |

###### 4.3.2.4、对体验的影响

只影响 plank 类透明窗口；差的 20px 本就是透明区（"看穿+点穿"给桌面是设计意图）；
唯一可感知点：**若 plank 图标区恰好贴边（如最右垃圾桶按钮），边缘处可能点起来不灵**
——需真机看 plank 布局才能确定。

###### 4.3.2.5、怎么修

贴形状时按窗口实际显示位置算起点（加标题栏高度偏移），而非从 0 开始——改
`xwindow_shape_pixmap` 的偏移计算。

###### 4.3.2.6、`✅ 已修` 修复评估（2026-10-23）

- **代码量**：很小——`xwindow_shape_pixmap` 一处偏移参数（`x/y` 起点加 titlebar 高）
  或调用方传偏移，约 1~3 行改 + 1 处传参（`client.c` 调用点）。
- **与前一个修复（4.3.1）是否互为干扰/影响**：
  - **不干扰**——两者走的路径不同：4.3.1 改的是普通 client（无 shape）→ 返回 NULL
    （不传播，frame 全矩形）；本修复改的是 shaped client → 传播时的**偏移计算**。
    一个管「要不要传播」，一个管「传播时贴在哪」，正交，互不影响；
  - **可独立提交/独立验证**：改完只影响 shaped 窗口位置，普通窗口路径不变。
- **建议**：影响面极小（仅 plank 边缘 20px），真机若感知不到可不修；要修则下次出包
  时顺带（一行偏移），与主修复互不干扰。
- ✅ 后续：已修复并验证（见 4.3.3.1）。

##### 4.3.3、`✅ 已修` 20px 前移与后续问题（2026-10-23~24）

###### 4.3.3.1、`✅ 已验证` 20px 前移修复（2026-10-23）

- **代码**：`xwindow_shape_pixmap`/`xwindow_set_shape` 加 `y_off` 参数——
  surf 贴 pixmap 时按 titlebar top 高度下移（client 在 frame 内被 titlebar 顶下，
  形状也应按同偏移贴入 frame）；INPUT 传播处传 `c->titlebar[CLIENT_TITLEBAR_TOP].size`
  （client.c 2025/2250/4372 + event.c 965），BOUNDING/CLIP/drawin 传 0（无 titlebar，
  保持原语义）。
- **验证（Xvfb，shaped 窗口 400x300 挖底 50px）**：

  | 探测点 y | 修复前 | 修复后 |
  |---|---|---|
  | 330/350/360/368 | client ✓ | client ✓ |
  | **371（理论边界）** | **320 就穿** | **root 穿（边界=371 精确）** |
  | 374/380/400/415 | root 穿 | root 穿 |

  透明区边界 y≈320 → **y=371**（=client y121 + shape 高 250），**20px 前移完全
  消除** ✓；普通窗口底部探测（y=750~799）全窗 xterm 无空洞（主修复不回归）。
- **与 4.3.1 关系**：正交独立——4.3.1 管「普通 client 不传播（NULL）」，本修复管
  「shaped client 传播时贴的位置」，不互相影响。
- ⚠️ 后续：本修复**不含判据**（只管偏移），但它配套的 4.3.1 判据有误——
  **plank 穿透回归**，根因已锁定（见 4.3.3.2）。

> ✅ 三架构（x64/arm64/armv7）GitHub Actions 已发布包待验（四·9）；⚠️ 触发方式：
> **本仓（gitea）push 不触发构建**，需**手动转推到 GitHub 仓库**才跑 GH Actions；
> 本次修复已提交 `9464b0a0a`，Xvfb 验证通过。✅ **真机验证通过（2026-10-23 用户确认）**：
> 窗体底部区域穿透已修复（新包 `261023-212612-v4.3-1867-ad9dec5`）。

###### 4.3.3.2、`✅ 已验证` plank 穿透回归根因（2026-10-24）

用户反馈：**plank 周边透明区域可穿透到下层窗口，但点不到桌面（右键菜单无反应）；
左右两侧是其他窗口时右键正常**。回归区间 `9abfc21`（10-15 穿透正常）→ `ad9dec5`
（10-23 当前部署）。区间内两个 shape 相关提交：`9464b0a0a`（NULL 判据）与
`190ca2196`（y_off）。

**结论：`9464b0a0a` 的判据用了错误代理，plank 被判成「无 shape」而不传播。**
4.3.1 的**目的**（跳过 no-op 传播、避免 frame 尺寸不匹配）是对的，**判据**错了。

- **为什么原判据必然错——X 协议没有 `input_shaped` 字段**（自查对照实验，自建
  窗口逐步设 shape + hex dump 原始回复，28 字节回复体逐字节对齐）：

  | 步骤 | 回复 offset 8-11 |
  |---|---|
  | 未设任何 shape | `00 00 00 00` |
  | 设 BOUNDING (10,10 60x40) | `01 00 00 00` ← `bounding_shaped` |
  | **只设 INPUT (20,20 20x20)** | **`01 00 00 00`（四字节一个没变）** |
  | 再设 CLIP (25,25 50x50) | `01 01 00 00` ← `clip_shaped` |

  即 **X `ShapeQueryExtents` 回复里根本没有 `input_shaped`**——xcb 结构体没这个
  字段**不是漏建模，是协议本身不提供**（`pad1[2]` 在设了 INPUT 后也恒为 0）。
  → **4.2 原「方案 A」（判据加 `input_shaped`）直接作废，字段不存在。**

- **plank 确实只设 INPUT shape**（符号级证据，与 X server 实现无关）：
  `/usr/bin/plank` 只是 14KB 的 main 入口 stub（仅依赖 libplank.so.1 + glib），
  真正实现全在 `libplank.so.1`：

  ```
  $ nm -D -u /lib/x86_64-linux-gnu/libplank.so.1 | grep -i shape
  U gdk_window_input_shape_combine_region   ← INPUT shape（不改 bounding）
  U gtk_widget_shape_combine_region
  ```

  按对照实验，只设 INPUT 时 `bounding_shaped` 保持 0 → `xwindow.c:284`
  `(bounding_shaped || clip_shaped)` = 0 → **返回 NULL → 不传播 plank 的 input
  shape → frame 保持全矩形输入 → 透明区吃鼠标事件 = 用户所见现象**。
  而 `9abfc21`（10-15）无此判据、总是传播 → 穿透正常，**回归区间完全吻合**。

- **坑记录**：`nm -u`（不带 `-D`）对 stripped 程序返回 0 条，会误判「plank 没有
  shape 调用」；必须用 `nm -D -u`（`.dynsym` 不被 strip 删除），且要查 `.so` 而非
  入口 stub。

**🔧 建议修法（改判据，不换目的）**：不再依赖协议标志，直接测「传播是否有实际
效果」——**client 的 INPUT 矩形是否覆盖整个 client 窗口**：

| client 状态 | INPUT 矩形 | 动作 | 效果 |
|---|---|---|---|
| 普通窗口（xterm 等，无 shape） | = 全窗矩形 | 返回 NULL → `xcb_shape_mask(XCB_NONE)` | frame 恢复全矩形输入，**4.3.1 的空洞修复保住** ✓ |
| plank（只设 INPUT） | ≠ 全窗（仅图标区） | 正常传播 | **穿透恢复** ✓ |

反正在 343 行之后本来就要把 rectangles 画进 cairo A1 surface，画完后检测 surface
是否**全满**即可（`cairo_surface_flush` + 扫 `cairo_get_data` 位平面），不需要
写矩形合并算法；语义上也更贴合 4.3.1 注释的本意（「input shape 是全窗矩形时传播
是 no-op」）。`y_off`（`190ca2196`）保持不动，二者正交。

- 落地范围：`xwindow.c` INPUT 分支改判据；调用点 `client.c`（2023/2248/4372）与
  `event.c`（963）、`drawin.c`（731）经 `xwindow_get_shape` 统一入口，无需各自改。
- 🟡 残留：`xcb_shape_mask` 用 `XCB_NONE` 清除 = 恢复默认（风险 1，见 5.2）——
  本修法**扩大**了返回 NULL 的场景覆盖正确性，但不改变该隐患本身。

**⚠️ 关键限制：Xvnc 的 shape 实现不可靠，其 shape 数据不能作真机结论。** 硬证据
（Xvnc :311 实测）：
1. `xcb_shape_query_extents` 回复的 `length` 字段**恒为 0**（100x100 窗口应为 5）；
2. plank 客户窗 bounding extents 明显被改过，但 `bounding_shaped=0`——**自相矛盾**
   （自建窗口对照实验里该标志随 BOUNDING 设置正确翻转，说明是 Xvnc 侧不一致）；
3. 同一窗口 `query_extents` 与 `get_rectangles` 返回的尺寸不一致（192x65 vs 191x64
   vs 190x63 三套数字）。

AGENTS.md 早已记「Xvnc 与真机 RandR/shape 行为有差异」。**故：代码可以照建议改，
但「修好了」必须真机 A/B 确认，Xvnc 上验证不出这个结论。**

**✅ 已落地（2026-10-24，commit `bdc1a0790`）**：按上表实现，判据从「协议标志」换成
「传播是否有实际效果」。

- 新增 `static bool xwindow_shape_surface_full(cairo_surface_t *, int, int)`：
  `cairo_surface_flush` 后扫 A1 位平面最高位（stride 按 32 位对齐），全满返回 true。
- `xwindow_get_shape` INPUT 分支**删掉** `xcb_shape_query_extents` +
  `(bounding_shaped || clip_shaped)` 代理判据；改为 `xcb_get_geometry` 直接取 client
  几何 → rasterize（本来就要画）→ 画完测全满：
  - **满** → 销毁 surface、返回 NULL，调用方走 `xcb_shape_mask(XCB_NONE)` 恢复 frame
    默认全矩形输入 —— 4.3.1 的底部空洞修复保住；
  - **不满** → 返回 surface 正常传播 —— plank 穿透恢复。
- BOUNDING/CLIP 分支一字未动，仍用 extents + `shaped` 标志：那两个标志语义是可靠的，
  只有 INPUT 没有对应标志。
- 顺带补上原代码漏掉的 `free(geom)`。
- 调用点（`client.c` / `event.c` / `drawin.c`）未动，统一走 `xwindow_get_shape`；
  `y_off`（`190ca2196`）保持不动，与本次改动正交。

编译期两个坑（已记 memory）：
1. 容器 `/usr/include/cairo/cairo.h` 声明 `cairo_image_surface_get_data` 返回
   `unsigned char *`、**未定义 `cairo_byte_t`** —— 首编直接 `unknown type name`，改用
   `const unsigned char *`（对齐 `draw.c:103`）后通过；
2. `CMakeLists.txt:55-95` 把源文件全写成 `${BUILD_DIR}/xxx.c`，ninja 编的是**构建目录
   里的副本**；打包脚本与 ninja 规则都不负责同步（容器内也无同步进程）→ 改完仓库源码、
   跑 ninja 前必须 `diff` 两处同名文件，必要时 `cp` 过去再编。

**验证状态**：`ninja awesome` rc=0、0 告警；mtime 链 `xwindow.c` → `.o` → `awesome`
确认编的是新源码（静态函数被 `-O1` 内联，`nm` 查不到符号属正常，不能当判据）。
Xvnc 只能确认「起得来、不崩」，穿透是否恢复需真机 A/B。

**✅ 真机验证通过（2026-10-24 用户确认）**：部署 `bdc1a0790` 后的新包到真机，
plank 穿透问题解决——**Xvnc 上无法证实的这条结论已由真机补齐**，本节闭环。
注：本机 `/_ext/tmp/awe-package` 下最新包仍是 `261024-090237-v4.3-1888-3a1a3ba`
（早于 `bdc1a0790`），所验包应来自更晚的构建。

###### 4.3.3.3、`✅ 已验证` 判据自身 bug 与修法（2026-10-24）

**提交**

- `9096ba0db` — `xwindow.c` 修复
- `ec43a27a2` — `docs/sam/2026-10-13-record6-plank.md` 4.3.3.3 + 交叉引用

**真根因**

`bdc1a0790` 的 `xwindow_shape_surface_full()` 拿 `cairo_image_surface_get_stride()`（字节）去比 `width`（像素）。A1 是每像素 1 bit、行按 32 位对齐，所以 100px 宽的 surface 的 stride 是 **16** —— `16 < 100` 恒为真 → 函数**从来没返回过 true** →「全满则 bail」分支从未生效 → 每次 shape-notify 都把 client 的 input mask 传播到 frame 上，而 client 尺寸的 mask 是从 titlebar 下方起画的，上方那条带的像素被写成「无输入」。

修法是**在分配 cairo surface 之前**就按矩形并集决策：

- 单矩形快路径：`x<=0 && y<=0 && x+w>=W && y+h>=H`
- 多矩形：clamp 进 client 区后逐格累加已覆盖格数，等于 `W*H` 即满
- `num_rects == 0` 也 bail —— 传播空 mask 等于给 frame 一个**空** input mask，比默认更差

**Xvnc 四路 A/B（探针同时查 client 与 frame 的 INPUT 矩形）**

| 二进制 | commit / md5 | 普通窗 frame INPUT | 标题栏条带 | plank frame INPUT | plank 穿透 |
|---|---|---|---|---|---|
| D | `d87d04b04`（`bdc1a0790^`）`316f2b8f` | `[-1,-1 201x175]` | **全覆盖** | `[-1,-1 201x151]` | **无** |
| A | `bdc1a0790` `18fba2bc` | `[-1,23 200x150]` | **未覆盖** | `[139,99 40x20]` | 有 |
| C | `3a1a3ba8c` `0bd3112a` | `[-1,23 200x150]` | **未覆盖** | `[139,99 40x20]` | 有 |
| **B** | `9096ba0db` `2fc8a6df` | `[-1,-1 201x175]` | **全覆盖** | `[139,99 40x20]` | **有** |

四组 client 侧读数完全一致，说明差异只出在判据。**B 是唯一同时满足「标题栏可点」与「plank 穿透」的构建。**

文档里记录了两处更正（没有悄悄改掉）：

1. 之前的「pristine C = v4.3-1888-ga3a1ba3c8」**不是**判据引入前的基线 —— `3a1a3ba8c` 在 `bdc1a0790` **之后** 2 个提交（只改 docs），所以 C 与 A 行为完全一致。真正的父提交是 `d87d04b04`；我把它构建成 D 补测，确认那里坏掉的是 plank 穿透。
2. 4.3.3.2 里建议的捷径（「不需要写矩形合并算法」）已撤回 —— 它绕开的那次 A1 扫描正是本次的真根因。

另外：标题栏穿透其实早于 `bdc1a0790`，可追溯到 `5316bb76f`；`9464b0a0a` 的代理判据碰巧把它挡掉了，代价是 plank 一起被挡。所以「标题栏可点」与「plank 穿透」之前是二选一，现在才算都成立。

busted：`576 successes / 0 failures / 10 errors / 2 pending` —— 与迁移 Xvnc 后记录的基线完全一致；那 10 个 error 全是既有的 `gears.surface get_pixels` shim 缺口。

###### 4.3.3.4、`✅ 已记录` 未覆盖三块细节（2026-10-24）

4.3.3.3 是本次修复的摘要记录，本节补它没展开的三块。

**为什么 plank 穿透看起来「也修好了」**：因为 `bdc1a0790` 真正生效的只有
「删掉 `(bounding_shaped || clip_shaped)` 代理判据」这一半，另一半（全满则 bail）
是死的。两个方向叠加，正好让两边都看起来对。

**探针口径**：容器内自建探针 `probe_frame.c`（xcb 直接建窗、设
`_NET_WM_WINDOW_TYPE`、GC 画 depth-1 mask 设 INPUT、查 **frame 与 client 两边**的
INPUT 矩形），rc.lua 对非 dock 客户端 `c.border_width = 1` + `c:titlebar_top(24)`
造出真实 24px 标题栏条带。4.3.3.2 已证 Xvnc 的 shape 数据不能作真机结论，但四组
用同一台同一探针、**同一把尺**，相对比较成立。

**`num_rects == 0` 为何是唯一入口**：`ShapeQueryExtents` 没有 `input_shaped` 字段
（4.3.3.2 已用 hex dump 实证），矩形个数是唯一能看出「client 设了空 mask」的入口。

**回归面**：`xwindow_get_shape` 统一入口，调用点 `client.c` / `event.c` /
`drawin.c` 一字未动，BOUNDING/CLIP 分支与 `y_off`（`190ca2196`）不动。风险 1
（`xcb_shape_mask(XCB_NONE)` 即恢复默认，见 5.2）不变。

**编译**：`ninja awesome` rc=0、0 告警（busted 结果见 4.3.3.3）。

#### 4.4、`✅ 已记录` 验证方式对比（2026-10-23）

> 主题侧验证方式见 `fk-awesome-wm-theme/docs/adjustment-log-03.md` 六·6.2.4.5（完整验证
> 探查过程，含改用 Xvnc）。本节为我的验证（见 4.3）与它的**逐项对比**，原样保存。

##### 4.4.1、对比表

| 维度 | 主题侧（6.2.4.5） | 我的验证 |
|---|---|---|
| **显示服务器** | **Xvnc**（TigerVNC，真机 xrdp 后端同路径，动态 monitor/RDP 协商分辨率） | **Xvfb** 1280×800 固定（未经 Xvnc 复测） |
| **判据演进** | 四步：几何法→点击法→**桌面右键菜单判定**→**鼠标下窗口扫描**（指向 root） | 一步到位：**鼠标下窗口扫描**（xdotool getmouselocation） |
| **覆盖 app** | xterm + **sakura**（GTK）+ st（对照，st 无空洞曾被误判） | 仅 xterm |
| **版本 A/B** | 三版（10-10 / 10-13 / 10-15）锁根因 | 两版（10-15 vs 修复后） |
| **shaped 回归** | 未覆盖（主题分析侧重普通 client 空洞） | **已覆盖**（自写 C 探针建 shaped 窗口，验证传播保留） |
| **harness 坑记录** | 6 个坑全部记录（lock/pgrep 自匹配/漏 $/awk/xwininfo 无 Parent/xdotool 十进制+截断） | 踩了 pkill 自匹配 + xterm `-geometry` 字符网格坑，未系统记录 |
| **结果** | 指明 awe side `xwindow_get_shape`/`xwindow_shape_pixmap` 尺寸不匹配 | 同根因，且**已落地修复**（commit 9464b0a0a）并验证空洞消失；判据后续修正见 4.3.3.2 与 4.3.3.3 |

##### 4.4.2、关键差异与优缺点

**主题侧更强的地方**：
1. **Xvnc 而非 Xvfb**——真机等价性高（Xvfb 与 Xvnc 不同路径，先前 6.2.1 的 Xvfb
   几何"无空洞"正是被用户纠正的误判来源）；
2. **判据分层演进**——几何法误判 → 逐步逼近到输入层，每步排除一个维度；我的直接
   跳到输入层扫描，跳过了"证明几何层没问题"这一步；
3. **多 app 覆盖 + 对照**——st 无空洞的对照极有价值（避免「xterm 特有」误判），
   我只有 xterm。

**我的验证更强的地方**：
1. **落地修复了**——不只是定位，而是改代码 + 编译 + A/B 验证修复生效（主题侧
   6.2.4.4 还停在"待开工"）；
2. **shaped 不回归验证**——主题侧没做，我自写 C 探针建 shaped 窗口验证传播保持
   （5316bb7 的原始意图不破坏）。

##### 4.4.3、我的验证不足

1. **Xvfb vs Xvnc**——我的空洞消失结论建立在 Xvfb 上，真机（Xvnc/xrdp）未复测。
   主题侧明确 Xvfb 曾给出误判（6.2.1 几何无空洞），同样风险存在于我的"修复后无
   空洞"——**严格说应到 Xvnc 复测一遍确认**（用户真机 xterm 24px 空洞才是最初
   报告的）；
2. **覆盖 app 单一**——只 xterm；sakura（GTK，用户指定 app）未测空洞消失是否
   同样生效；
3. **harness 系统坑记录**——我踩了 2 个坑（pkill 自杀、xterm -geometry 字符单位）
   未按主题侧那样记录成清单。

##### 4.4.4、结论

我的验证**定位更准（直接到输入层）+ 走到了修复落地 + 补了 shaped 回归**，但在
**真机等价性（Xvnc）与 app 覆盖广度**上不如主题侧——空洞在 Xvfb 消失 ≠ 真机
Xvnc 消失，需补一步 Xvnc 复测（尤其用户最初是 Xvnc 真机报的 24px）。

#### 4.5、`✅ 已定案` popen 宏位置根因（2026-10-23）

> 用户反馈：新版包（261023）部署后主题报 `popen not supported`；旧包二进制直接
> 替换新版二进制也报同样错，整个新版本包都一样。

##### 4.5.1、现象与定位

- 同一主题、同一环境，旧二进制（261020）`io.popen` 正常，新二进制（261023）报
  `'popen' not supported`。
- Xvfb + 最小测试 rc（rc 里 `pcall(io.popen, "echo popen_ok")`）A/B 实测：
  | 二进制 | 结果 |
  |---|---|
  | 旧 261020 | `POPENTEST OK: popen_ok` |
  | 新 261023 | `POPENTEST FAIL: 'popen' not supported` |
- 主题触发点：`fk-awesome-wm-theme/utils.lua:182` `run_command_sync` 用 `io.popen(command)`。

##### 4.5.2、根因（已实证）

- `io.popen` 在 Lua 5.3 走 `liolib.c` 的 `io_popen`，实现受 **`LUA_USE_POSIX`** 宏条件编译：
  - 定义了 → POSIX `popen()` 实现；
  - 未定义 → `#else` 分支直接返回 `'popen' not supported`。
- `liblua.a` 里 `liolib.o` 的 strings 含 **`'popen' not supported`** 且无 popen 实现
  = 编译时 `LUA_USE_POSIX` 未生效。
- **03 脚本（`docs/sam/scripts/03-container-build-static-libs.sh:151`）和 CI build.sh
  （`.github/scripts/build.sh:97`）都把 `-DLUA_USE_POSIX` 放在 `MYLDFLAGS`**：
  ```
  make generic MYCFLAGS="-fPIC" MYLDFLAGS="-DLUA_USE_POSIX" MYLIBS="-lm"
  ```
  **`-D` 宏必须在编译期（MYCFLAGS），放 MYLDFLAGS（链接期）不生效** → liolib.c
  编成无 POSIX 版本。
- 实证（builder-awe 重编对比）：
  | liolib.o 编译方式 | 结果 |
  |---|---|
  | `MYLDFLAGS="-DLUA_USE_POSIX"`（当前） | strings 含 `'popen' not supported` → popen 坏 |
  | `MYCFLAGS="-fPIC -DLUA_USE_POSIX"`（修复） | strings 无 not-supported 串，popen 有实现 ✓ |

##### 4.5.3、旧包正常与脚本历史

- 该宏位置 bug **一直存在**（非 15 号后引入）——15 号后脚本调整仅：11 包名格式
  （`33da9964d`）、11 configure 注入 LUA_EXECUTABLE（`5f7b90364`）、03 路径 /ext→/_ext
  （`6f06dd541`），均与 lua 编译宏无关。
- 旧包（261020）正常是因为当时链接的 liblua.a 是更早环境（apk lua 或早期正确编译）
  的产物；builder-awe 的 liblua.a 是 **10-19 重编**（时间戳 Oct 19 16:45），用了
  MYLDFLAGS bug 版 → 261023 出包链到它，暴露 popen 坏。

##### 4.5.4、两处脚本分开提交

- `docs/sam/scripts/03-container-build-static-libs.sh`：`-DLUA_USE_POSIX` 从 MYLDFLAGS
  移到 MYCFLAGS。
- `.github/scripts/build.sh`：同上。
- 重编 liblua.a → 重出包验证 popen 恢复。

##### 4.5.5、出包脚本调整清单

| 脚本 | commit | 内容 |
|---|---|---|
| 11 | `33da9964d` | 包名改 `<yymmdd-HHmmss>-v<ver>-<sha7>` |
| 11 | `5f7b90364` | configure 注入 `-DLUA_EXECUTABLE=/usr/bin/lua5.3` |
| 03 | `6f06dd541` | 源码路径 `/ext`→`/_ext` + 环境变量覆盖 |

##### 4.5.6、`✅ 已验证` 真机验证

- 修复落地：03 脚本（`2128f7cb6`）+ CI build.sh（`ad9dec52c`）把 `-DLUA_USE_POSIX`
  从 MYLDFLAGS 移到 MYCFLAGS。
- 验证链：
  1. lua 重编（`-DLUA_USE_POSIX` 进 CFLAGS）→ `liolib.o` 无 "popen not supported" 串；
  2. awesome 重链 → popen 测试 rc `POPENTEST OK: popen_ok`；
  3. 正式出包 `261023-212612-v4.3-1867-ad9dec5`（7/7 完成、静态确认）；
  4. **包内二进制实测 popen OK + 用户真机确认：popen 与窗体底部区域穿透均无问题**。
- ✅ 闭环：popen not supported（4.5）+ 窗体底部穿透（4.3）双双真机验证通过。

#### 4.6、`✅ 已记录` 用户理解与识别机制（2026-10-23）

##### 4.6.1、用户总结理解原文

> 用户总结（原文 1 字不落），下方为核对修正。

**用户原文**：
1. 前者作用于普通窗体，于是窗体响应区域 没有把标题栏给算进来？然后又是从标题栏的
   顶部开始计算的，然后就导致底部也往上移了？
2. shape问题，作用于plank这类需要透明，需要穿透的特殊窗口应用，原来的问题是它的
   顶部往下移了20px，从而可能导致plank区域顶部20px区域鼠标的操作不落在他的窗体
   响应区，而是穿透到底层了。
3. 第2条做了修复之后，真机体验: plank工作正常，鼠标操作响应这些都正常。暂没有
   发现不良影响。

**核对结论（方向修正）**：

- **第 1 条理解 ✅，方向精确化**：client 形状贴到 frame 时以 frame 顶部（标题栏顶）
  (0,0) 为原点、**没把标题栏高度算进来**；但 client 实际被标题栏顶到下方 21px
  （titlebar 高）。结果形状整体**上移** titlebar 高 → frame 顶部 21px 被形状错误覆盖，
  **frame 底部 21px 无输入像素 → 底部穿透**。准确说是「形状整体上移、底部缺一段」，
  不是「底部往上移」——效果等价（底部 21px 空洞），方向描述「上移」更准。
- **第 2 条理解 ✅，方向修正**：不是「顶部往下移 20px」，而是**形状整体上移约
  titlebar 高（20px）**。修复前透明区边界从理论 y=371 提前到 y=320（= 可视区底部
  最后 ~20px 提前变透明/穿透），影响方向在**底部边界**不在顶部——若 plank 贴边
  图标（如最右垃圾桶）靠近底部，那 20px 里点击可能落在穿透区。plank 需透明穿透
  （XShapeInput），修复后位置准确 + 穿透语义保持。
- **第 3 条 ✅ 真机确认**：plank 工作正常、鼠标响应正常、无不良影响——正是修复
  目标：plank 透明区穿透保持（5316bb7 原始意图不回归）+ 形状位置修正（20px 前移
  消除）+ 普通窗体底部空洞消失。

##### 4.6.2、shaped 识别与穿透处理（2026-10-23）

> 用户疑问：plank 这类需透明/穿透的特殊窗口，awm 如何识别？按名字匹配吗？
> 业界同类应用有哪些？

**识别机制：纯事件驱动 + X 层查询，与名字/class 完全无关**（源码核实）：

- **管理时初始同步（`client_manage`，client.c:2240-2255）**：每个窗口被接管时
  **无条件**调 `xwindow_get_shape(c->window, INPUT)` 查 input shape——查的是
  **X 层窗口实际 shape 状态**，不关心窗口叫什么。
- **运行中（ShapeNotify 事件，event.c:943 `event_handle_shape_notify`）**：窗口后设
  shape 时 X server 发 ShapeNotify（awesome 订阅 Shape 扩展事件），收到后若
  `shape_kind == XCB_SHAPE_SK_INPUT` 就重新传播到 frame。
- **识别依据 = X shape 存在性**（`xwindow_get_shape` 查 `bounding_shaped`）：
  - 窗口**没设任何 shape**（普通 xterm/sakura）→ input shape 默认全矩形 → 不传播
    （返回 NULL，frame 全矩形输入）；
  - 窗口**设了 shape**（plank 类）→ 按实际 shape 传播到 frame（透明区穿透保持）。
- **名字匹配是另一回事**：主题仓 `rc.lua` 里 `class = "Plank"` 例外是**装饰规则**
  （免标题栏），不是穿透识别——穿透识别完全不看 class。

**业界同类（X11 里靠 input shape 做穿透的应用）**：

| 应用 | 用途 |
|---|---|
| **plank / docky** | dock：窗口比可见条大（预留弹跳/放大/阴影），input shape 挖透明区 |
| **rofi / dmenu** | 启动器：圆角/透明边，shape 让圆角外点击穿透 |
| **compton/picom 圆角** | 合成器给普通窗口裁圆角（bounding shape），点击圆角外穿透 |
| **tint2**（面板） | 可配置透明/圆角，input shape 让非可见区穿透 |
| **polybar**（可配） | 同上 |
| **conky**（可配 own_window_type 时） | 桌面挂件，shape 让透明区穿透 |
| **st/终端圆角补丁** | 圆角裁剪，shape 点击穿透 |
| **GIMP 取色器等** | 临时 shape 窗口 |

> 共性：**凡是"窗口大于可见内容、非可见区要点击穿透"的 X11 应用**都用 input
> shape；awm 的传播机制对所有这类应用通用（不特判名字），plank 只是最常见的一个。

### 5、`✅ 已记录` 文档全景与风险分析（2026-10-24）

#### 5.1、`✅ 已记录` 两仓历史文档全景（2026-10-23）

> 回顾主题仓 `fk-awesome-wm-theme` 与 awe 仓 `fk-awesomewm` 全部 plank 相关历史
> 记录，1 字不落保留。原始出处见各文档对应节。

##### 5.1.1、主题仓 `fk-awesome-wm-theme`

**`docs/adjustment-log-01.md` 二·2.1（Plank 被套标题栏，2026-10-14）**
- catch-all 规则 `titlebars_enabled=true` 一刀切 → Plank 被套标题栏
- 危害：frame 87px（标题栏24+内容63），底到 1104 超屏下沿 24px → **被顶出屏幕**
- 修：`rc.lua` 按 class 加例外（`border_width=0, titlebars_enabled=false`），实测 geometry 回到 `190x63` 贴底
- 更彻底方案（未采纳）：按 `c.type == "dock"/"splash"/"toolbar"` 统一免装饰

**`docs/adjustment-log-02.md` 十一·1（plank 底部透明遮罩拦点击，2026-10-13）**
- 现象：底部整行透明遮罩、区域内窗口无法点击；openbox 正常、fluxbox/awesome 不行
- 机制：plank 窗口 > 可见 dock（预留弹跳/放大/阴影空间），靠 **XShapeInput** 穿透；xcompmgr 是**诱因**（认领 CM_S0 后 plank 扩窗）
- 实测：有 xcompmgr `1280x137`（全宽，透明区~74px 拦点击）；无则 `314x63`
- WM 差异：openbox 应用 input shape ✓；**awesome `shape.lua` 无 input shape + reparent 到矩形 frame → 全拦截**
- 解决：不跑 xcompmgr 最干净（已实测）；→ 引出 awe 侧 input shape 传播修复（`5316bb76f`）

**`docs/adjustment-log-03.md` 二·5（plank 误隐藏排查闭环，2026-10-15）**
- hide-mode Dodge Active 下非最顶窗触 dock 误隐藏
- 探针 II 实测：指针落底部条带时在 **plank 自身窗口**（1280x137，透明扩展区），非客户端 → **sloppy focus 无偷焦机会** → 撤回方案 2/2.5，`rules.lua` 零改动
- 遗留：成因待细化（后自动好了）；教训：A/B 探针必须覆盖目标区域坐标

**`docs/adjustment-log-03.md` 六·6.1（贴条感应，2026-10-22）**：plank 穿透保持作为回归项

**其他**（impl-plan/redesign/impl/impl2）：plank 作为**回归清单项**（确保不复发）+ 菜单过滤非常规窗体（弹窗/plank）

##### 5.1.2、awe 仓 `fk-awesomewm`

**`docs/sam/2026-10-13-record6-plank.md` 第八章（plank dock 点击拦截，2026-10-13 起）
（2026-10-24 前位于 `docs/sam/2026-09-28-record5.md` 第八章，已拆出）**
- **八·1**：maki> 分析 + 根因（plank 窗口>可见 dock、input shape 穿透、awesome 不处理→frame 吃点击）+ 修复（`5316bb76f` 传播 input shape 到 frame）
- **八·2**：Xvfb 模拟验证
- **八·3**：部署版复跑确认（`✅ 已复核`）+ 3.1/3.2 拆号 + frame 10x10 独立问题（已回退）
- **八·4**（2026-10-23 新增）：**5316bb7 传播的副作用回归**——浮动布局窗体底部 21px 穿透
  - 4.1 核实：普通 client 底部 `frame高−client高`（titlebar）无 shape 像素 → 空洞
  - 4.2 修法（INPUT 查 bounding_shaped → NULL）
  - 4.3.1 主修复落地（`9464b0a0a`）+ Xvfb 验证空洞消失
  - 4.3.2 拆子节（shaped 不回归 + 20px 前移，人话版）
  - 4.3.3.1 **20px 边界前移修复**（`190ca2196`，set_shape 加 y_off 按 titlebar 偏移）
  - 4.3.3.2 **4.3.1 判据有误：plank 穿透回归根因锁定（2026-10-24）** + 修法已落地
    （`bdc1a0790`）+ `✅ 真机验证通过`（2026-10-24 用户确认）
  - 4.3.3.3 **判据自身的 bug：A1 位平面 stride/width 混用（2026-10-24）**
    → 判据改回 rectangles 并集、rasterize 前决策 + Xvnc 四路 A/B（D/A/C/B）
    + 更正 4.3.3.2 两处旧结论（pristine 基线误标、「不需要矩形合并算法」不成立）
  - 4.3.3.4 **4.3.3.3 没覆盖的三块细节**：plank 为何看起来「也修好了」+ 探针口径 + 回归面
  - 4.4 我的验证 vs 主题侧 6.2.4.5 对比

**commits**：`a1af48308`(八章新建) → `5316bb76f`(传播) → `be76ba285`(几何重建) → `9464b0a0a`(NULL 修复) → `190ca2196`(y_off 偏移) → `bdc1a0790`(rectangles 覆盖判据) → `9096ba0db`(rectangles 并集判据，rasterize 前决策，见 4.3.3.3)

##### 5.1.3、时间线总结

```
10-13 plank 透明遮罩拦截（主题02）→ awe 5316bb7 传播修复（record5 八）
10-14 Plank 被套标题栏（主题01）→ rc.lua 例外
10-15 误隐藏排查（主题03 二·5）→ 排除 sloppy
       拖大死区回归 → be76ba285（record5 八·3）
10-22 贴条感应两问题（主题03 六·6.1）
10-23 浮动布局底部穿透（record5 八·4）→ 9464b0a0a + 190ca2196 双修复
10-24 判据真根因锁定（4.3.3.2）→ bdc1a0790 rectangles 覆盖判据修复 → ✅ 真机验证通过
10-24 标题栏穿透回归（4.3.3.3）→ 真根因 = A1 stride/width 混用 → 改回 rectangles 并集判据
     → Xvnc 四路 A/B（D/A/C/B）：仅新修复（`9096ba0db`）同时满足「标题栏可点 + plank 穿透」
```

#### 5.2、代码变更回顾与风险分析（2026-10-23）

> ⚠️ 本节写于 2026-10-23，描述的是 `9464b0a0a` 时代的实现。`bdc1a0790`
> （2026-10-24，见 4.3.3.2）已按 rectangles 覆盖判据重写 INPUT 分支；
> **本次修复（`9096ba0db`，见 4.3.3.3）又把它换成了不经过像素的 rectangles 并集判据，并在
> `query_extents` 在 INPUT 分支已由 `bdc1a0790` 换成 `xcb_get_geometry`（本次修复
> 沿用，仍是 2 个同步 round-trip，开销不变）；**风险 3** 描述的 `extents==NULL`
> 降级路径在 INPUT 分支已随判据一并移除（`get_geometry` 失败时仍返回
> error-state surface，属另一条待验路径）；风险 1、4、5、6 仍适用。

**代码变更回顾（6 commit，5 文件，~230 行）**：

| commit | 文件 | 改动量 | 思路 |
|---|---|---|---|
| `5316bb76f` | event.c + client.c | +33/-1 | **传播**：ShapeNotify INPUT + manage 初始同步——client 的 input shape 复制到 frame |
| `be76ba285` | event.c + client.c | +21/-2 | **补**：geometry 变化后重放（防 stale） |
| `9464b0a0a` | xwindow.c | +16 | **隔离**：INPUT 分支查 bounding_shaped，普通窗口返回 NULL（不传播） |
| `190ca2196` | xwindow+client+drawin+event | +18/-14 | **对齐**：set_shape 加 y_off（titlebar 偏移） |
| `bdc1a0790` | xwindow.c | +59/-17 | **改判据**：删代理判据，改按 A1 位平面「全满」检测（4.3.3.2） |
| `9096ba0db` | xwindow.c | +86/-38 | **判据再改**：A1 stride/width 混用使上一步的「全满」检测恒 false → 改回不经过像素的 rectangles 并集判据，且在 rasterize 之前决策（4.3.3.3） |

实现思路：三节点（manage 初始 / ShapeNotify 增量 / geometry_refresh 重放）→
`xwindow_get_shape` 统一查询 → `xwindow_set_shape` 传播。核心：**按 X shape 存在性
识别，不认名字**。

**🟡 中等风险（3 处，未触发但属隐患）**：

1. **`xcb_shape_mask` 用 XCB_NONE 清除 = 恢复默认 vs 丢失 Lua 预设**
   （xwindow.c:407）：若 Lua 通过 `client.shape_input` 给 frame 设过自定义 input
   shape，普通窗口的 NULL 传播会用 XCB_NONE **清掉它**。目前主题未用 `shape_input`
   API，但存在——「未用路径的潜在冲突」。
2. **普通窗口每次 geometry 变化都发 `xcb_shape_get_rectangles` + `query_extents`
   两个 round-trip**（client.c:2023-2028）：xterm/sakura 频繁 resize 时每次多 2 个
   X 请求。非阻塞（async），但 resize 风暴下是纯开销——可优化（缓存/仅 shaped 才查）。
3. **`xwindow_get_shape` 查询失败静默降级**（xwindow.c:285-287）：`extents==NULL`
   （查询失败）→ shaped=false → 返回 NULL → 走「普通窗口」路径。plank 若查询失败
   会变全矩形拦截（穿透失效）。X 正常时不触发，但属于「失败静默降级」。

**🟢 低风险/边界（3 处）**：

4. **BOUNDING/CLIP 不传播**（event.c:950-953）：只发 Lua 信号不复制到 frame——设计
   如此（frame 只需全矩形 + input shape），但 picom 圆角等靠 frame bounding shape
   的应用视觉与点击区可能不一致。主题未用，低风险。
5. **`have_input_shape` 全局开关**：SHAPE < 1.1 的 X 全禁用——正确降级，无隐患。
6. **y_off 仅 titlebar TOP**：若窗口有 LEFT/RIGHT/BOTTOM titlebar（罕见），X 偏移
   仍不完整（只修 TOP 方向）——4.3.3.1 已注明的边界，真机无此类窗口。

**风险优先级建议**：3（查询失败降级）最值得补——加一条「extents 查询失败时按
shaped 处理（保守传播）」，避免 plank 穿透静默失效；2（round-trip 开销）可在频繁
resize 场景实测后再优化；1（Lua 预设冲突）在主题用上 `shape_input` 前无需处理。
