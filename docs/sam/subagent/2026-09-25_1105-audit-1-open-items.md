# 审计报告一 · 计划与遗留项盘点（2026-09-25）

> **性质：** 独立只读调查的原始报告，正文未经裁剪。
> **范围：** `2026-09-23-impl-plan.md` 全文 + `record1.md` / `record2.md` / `record3.md` 所有
> 「遗留 / 未完成 / TODO / 待修 / 下一步 / 已取消 / 砍掉 / 不实现 / 跳过」章节
> + `spike4-phase2-01-scope.md`。
> **方法：** grep 定位章节后读上下文；impl-plan 的 Part 清单表逐行核对状态列。
> **写于：** HEAD `bf6ee42c`。
>
> 由主任务归纳进 `2026-09-25-record3.md` 篇五。本报告保留完整原文。

---

已读完全部指定文件并交叉核对了代码与磁盘产物。下面是合并清单。

## 一、合并清单

图例：**【真未做】** 没实现/没验证 ｜ **【已砍-有依据】** 明确放弃且文档给了理由 ｜ **【已修-待复验】** 修了但没重跑验证 ｜ **⚠️骗人** = 文档写"完成"但其实验证的是另一条链路

### A.【真未做】

| # | 项 | 出处 | 谁提的/哪轮 |
|---|---|---|---|
| A1 | **出包流水线没收口**：UPX 7 轮没跑；修完 pango/ rc.lua 后只跑了 step 6，**从没跑过 step 7**。磁盘上唯一 tarball 是 `4c31690`（09-25 00:16，**pango UAF 修好之前**）；`59b4cf7`(02:13)、`1b03351`(02:57) 只有 step6 log，无 tar.gz | `2026-09-25-record3.md:29,253`；实测 `/_ext/tmp/awe-package/` 只有 `4c31690-*.tar.gz`；`awe-build-release/upx.done` 时间戳 00:04（陈旧） | record3 §五-2；2026-09-25 收口轮 |
| A2 | **T2b 重判**：上一轮「30/32 通过」是按 rc-only 判的，多少是"进程没死"不是"跑起来了"，需按新 T2b 重跑 | `2026-09-25-record3.md:28,251-252`；数据源 `2026-09-23-record2.md:1053,1100-1114` | record3 现状-2 |
| A3 | **spec 从未在 shim 环境跑过**，全在真 lgi 下；shim 下行为未知，尤其 `surface_spec` 那 10 个 | `2026-09-23-record2.md:1366-1384,1370,1455`；实测 `tests/` 仍有 50+ 处 `require("lgi")` | record2 篇十二 §4/§7-1 |
| A4 | **transient `lgishim_gio` 回调错误**无法复现，已接 traceback，等它自己再出现 | `2026-09-25-record3.md:32,201-226,254`；前次记录 `2026-09-23-record2.md:1059` | record3 §四-3 |
| A5 | **出包脚本收尾缺 chown**：step 5 会把源码改成 root:root 444，脚本只 chown `$OUT_DIR`（:529），不碰源码目录 | `2026-09-25-record3.md:33,234,255`；`11-package-static-release.sh:529` | record3 §四-4 |
| A6 | **D-Bus 未配置**（容器无 dbus-daemon、无 `/etc/machine-id`），naughty 通知静默降级 | `2026-09-23-record2.md:1057`；实测最新 `1b03351.smoke.log` 仍在报 `Could not connect to D-Bus session bus` | record2 篇十 §5 |
| A7 | **theme 路径未验证**：`beautiful.init` 测试时路径不存在，走 fallback | `2026-09-23-record2.md:1058` | record2 篇十 §5 |
| A8 | **容器无 non-root 用户**：2 个 root 误判（`file_readable`/`dir_readable`）没消；且 `filesystem_spec.lua` 失败**带副作用**，root 下反复跑会污染工作区 | `2026-09-23-record2.md:1399-1407,1456-1457,1464-1465` | record2 篇十二 §5/§7-2/§7-6 |
| A9 | **busted 版本口径未知**：Alpine 3.15 的 2.0.0-0，上游 CI 可能不是，用例计数口径可能不一致 | `2026-09-23-record2.md:1458` | record2 篇十二 §7-3 |
| A10 | **无可复现出包环境**：这包是在跑了很多轮构建的容器里出的，干净镜像上重跑会踩不同坑，**没有 Dockerfile / 镜像 tag** | `2026-09-23-record2.md:1301-1305` | record2 篇十一 §4.6 |
| A11 | **Pango ≥1.50 迁移分支未测**（容器 1.48.10，`#if` 分支保留但不生效）；`PangoColor` 结构体直接读字段，**硬编码 1.48，无 fallback 机制** | `2026-09-23-record2.md:727`；`2026-09-23-impl-plan.md:302`；`2026-09-23-record1.md:971-987,1210` | record2 篇六 §8-4；Spike 2 §11 交接-1 |
| A12 | **SVG 主题降级 0.25 人天没做**：`menubar/icon_theme.lua:93` 扩展列表仍含 `"svg"`（实测未改）、`menubar/utils.lua:132` `svg=3` 未改、impl-plan 未记"SVG 主题为已知局限" | `2026-09-23-record1.md:1131,1233`；实测 `lib/menubar/icon_theme.lua:93` = `{"png","svg","xpm"}` | Spike 3 D.4（+0.25 人天，"可并入测试回归档"） |
| A13 | **别名陷阱未系统解决**：lib/ 混用 `glib.`/`GLib.`、`gio.`/`Gio.`，按大写 grep 的计数会持续漏项 | `2026-09-23-record2.md:726`；`2026-09-23-impl-plan.md:314` | record2 篇六 §8-3 |
| A14 | **枚举回落契约只锁了 `set_wrap`**：`set_ellipsize`/`set_alignment`/`set_font_slant` 走同一 `enum_from`，行为相同但**未逐个断言**（枚举拼错是静默丢弃，不是报错） | `2026-09-23-record2.md:678,728` | record2 篇六 §8-5 |
| A15 | **GLib closure guard + `a_glib_poll` 栈断言规范从未实测**（Spike 2 未涉及 GL，Phase 2 也未直接测） | `2026-09-23-record1.md:1216`；`2026-09-23-impl-plan.md:233,247` | Spike 3 新增-4 |
| A16 | **上游追踪/回退补丁未维护**：revert 后与上游在 4 文件分叉，cherry-pick 冲突集中，无"回退补丁"文件 | `2026-09-23-record1.md:93`；`2026-09-23-impl-plan.md:367` | record1.md 第二章 §3 风险-2 |
| A17 | **包名版本号写死 `4.3`**（实际 git describe 是 `v4.3-1841-g1b033515`）；**UPX 阶梯硬编码**，换架构（arm64）档位不一定齐；**出包脚本对失败不重试** | `2026-09-23-record2.md:1284-1292,1307-1310` | record2 篇十一 §4.3/4.4/4.7 |
| A18 | **Spike 2 多个复选框从未勾**：`- [ ] Commit`(B.1)、`- [ ] timeout 10 ./awesome`(C.4)、`- [ ] 能启动到 crash`(C.4)、C.5 commit 块 | `2026-09-23-impl-plan.md:118,172-180` | 实施计划 v3 原文 |
| A19 | **`gio.Async` 的 timer 分块未在生产落地**：95 行原型只留档在 `spike3-timer-proto.md` 附录 A，`menubar/utils.lua` 的 `parse_dir` 仍是同步版（只在保留 menubar 时才需要） | `2026-09-23-record1.md:1096,1188,1226`；`2026-09-23-impl-plan.md:248` | Spike 3 D.2 / D.5 调整-3 |
| A20 | **`lgi.require('Gtk','3.0')` 调用点 shim 无 `require` 字段**，靠 pcall 优雅降级，旧 scope 完全未记录 | `spike4-phase2-07-...md:759` | spike4-phase2-07 §7.5 |

### B.【已砍-有依据】

| # | 项 | 出处 | 谁提的/哪轮 |
|---|---|---|---|
| B1 | **Part 10 X11/XRandR 整批删除（省 5-8）**：lib/ 里 0 处调用，typelib 不存在，RandR 全走 C 层 XCB | `2026-09-23-impl-plan.md:305`；`spike4-phase2-06-...md:180,271` | 2026-09-24 scope 盘点 |
| B2 | **Part 11 librsvg 砍（决策 B，只 PNG）**：仓库 icons/ 100% PNG，builder 无 librsvg-2.0，省一个 libxml2 静态链 | `2026-09-23-impl-plan.md:306`；`2026-09-23-record1.md:1127-1133` | Spike 3 D.4 |
| B3 | **menubar 默认砍**（+9-12 人天若保留；Gio 从 3-5 涨到 9-16 且需 fork） | `2026-09-23-impl-plan.md:241,368`；`2026-09-23-record1.md:1189` | Spike 3 D.5 调整-4 |
| B4 | **oopango 不采纳**：数量覆盖 52% / 语义 31% / shim 特性 12.5%，强依赖 abandoned oocairo | `2026-09-23-record1.md:1135-1147`；`2026-09-23-impl-plan.md:218` | Spike 3 D.4.5 |
| B5 | **oocairo 不复用**：容器无 oocairo、命名空间不兼容、无 `._native`/`is_type_of`/幂等 `__gc`、对象模型冲突、净收益≈0 | `spike4-phase2-01-scope.md:27-39`；`2026-09-23-impl-plan.md:275-292` | Phase 2 起步决策 01:20 |
| B6 | **方案 B（只改 glib 版本号）排除**：编译期能过，运行期 `require("lgi").GioUnix` 在 glib 2.70.5 无 typelib 直接失败 | `2026-09-23-record1.md:80-86` | 方案 A/B 论证 |
| B7 | **一批 API 明确不实现**：`GLib.Error`/`GLib.Bytes`/`GLib.SpawnFlags`/`GLib.Subprocess`/`GLib.ChildWatch`、`Gio.MemoryOutputStream`/`FileOutputStream`/`PipeInputStream`、`Gio.DBusMenu` 系列、`GLib.SList`/`HashTable`/`Queue`、`GObject.Object`/`ParamSpec`/`Signal` | `spike4-phase2-05-...md:445-452` | spike4-phase2-05 |
| B8 | **cairo `__typename` 字段不做**：改 `is_type_of` 元方法（lib/ 里 `__typename` 实测 0 处） | `2026-09-23-record1.md:1104-1108,1161` | Spike 3 D.3 关键修正 |
| B9 | **4 个 Pango 1.50+ layout API 不做**（`get_preferred_size*`/`get_height_for_width*`）；`PangoColor.alpha` 故意留 nil | `2026-09-23-record1.md:962-964,983` | Spike 2 §3（"本轮不做"） |
| B10 | **不转向 R7**：R7 只解决"静态 WM"，无法保留 60+ 处 lgi 调用背后的 Lua widget/layout 生态 | `2026-09-23-record1.md:1180` | Spike 3 D.5 |
| B11 | **`glgi.Async` 不写 shim**（全库仅 1 处调用） | `2026-09-23-record1.md:1096,1163,1188`；`2026-09-23-impl-plan.md:248` | Spike 3 D.2 |

### C.【已修-待复验】——其中 5 条是⚠️骗人的"完成"

| # | 项 | 出处 | 为什么算待复验 |
|---|---|---|---|
| **C1 ⚠️** | **Part 13 spec 回归"✅ 完成 577/589=97.96%，新增回归 0，0.25 人天"** | `2026-09-23-impl-plan.md:308,366,381`；`2026-09-23-record2.md:1319-1327` | **验证的是另一条链路**：`LGI_SHIM_FLAG=false`，busted 加载系统 lgi 0.9.2（record2:1370）。它证明"lib/ 迁移结果在真后端下没变"，**没验证 shim 本身**（record2:1374-1379 自认）。shim 的验证全部来自手写冒烟 |
| **C2 ⚠️** | **Part 12 交付物：UPX 后 3,402,220 bytes、包 4,389,035 bytes / 346 条目、"可发布包"** | `2026-09-23-record2.md:1128-1161`；`2026-09-23-impl-plan.md:307` | 这个包是 `4c31690` 构建，**早于 pango UAF 修复（`59b4cf73`）和 rc.lua 修复（`fb67b582`）**。修完之后 step 4/step 7 从未重跑（见 A1）。即"可发布"结论对应的产物已被证明是坏的 |
| **C3 ⚠️** | **"Phase 2 全部批次完工…运行验证 30/32 通过；spec 97.96%…新增回归 0"** | `2026-09-23-impl-plan.md:366` | 30/32 是 `4c31690` 二进制 + rc-only 判定采的（record3:28,251-252 要重判）；spec 见 C1。**这一句是当前整份文档里最容易被当成"全部完成"的条目** |
| **C4 ⚠️** | **Cairo 冒烟 §20"真实 awesome 调用路径"+ 829→830 checks / 0 failures** | `2026-09-23-impl-plan.md:289,291`；`2026-09-23-record2.md:530,691,955`；`2026-09-25-record3.md:56` | §20 是**脚本里复现**的调用（`drawable.lua` 脏区域裁剪、`textbox.lua` draw 等），不是真跑 awesome 窗口。真正的默认 rc.lua 全流程 09-25 才首次执行，一执行就炸出 2 个真缺陷（record3:110-113）。**此前所有"全绿"都建在没跑过 rc.lua 的二进制上** |
| **C5 ⚠️** | **Rsvg 空表守卫修好了（B1 级阻塞"已解"）** | `2026-09-23-record2.md:1409-1451`；commit `1c0f142b` | 只做了纯 Lua 探针 + `loadfile` 语法检查。**真实路径 `imagebox.image = "icon.svg"` 从未在 shim 下跑过**（spec 未在 shim 下跑，见 C1/A3）。`lib/vendor/lgishim.lua:35` 的 `Rsvg = {}` 至今未动 |
| C6 | **pango 借用 UAF 修复**（`d_unref`→`d_noop`，一行改动） | `2026-09-25-record3.md:37-62`；`59b4cf73` | 独立复现 20000 次 + 冒烟 830 checks + 默认 rc.lua。**未重跑**：part12_test.lua 32 项、Ubuntu 22.04 glibc 交叉验证（record2:1062-1116 的"可移植性确认"底层数据全在旧二进制上采的） |
| C7 | **`cairo.Surface()` lightuserdatum 分支 + 顺带堵漏** | `2026-09-25-record3.md:140-180`；`fb67b582` | 只在默认 rc.lua 冒烟路径上验证；full/light 双分支的所有权正确性未做压力测试 |
| C8 | **textclock interval 0 上游 bug 修复**（改 Lua 不改 shim） | `2026-09-25-record3.md:182-199`；`fb67b582` | 只靠 `grep timeout = 0` + rc.lua 跑通；grep 只能证明没有 `timeout = 0` 字面量，覆盖不了其它传 0 的路径 |
| C9 | **gio 回调接 traceback**（`push_traceback_h`） | `2026-09-25-record3.md:201-226` | 这是**加探针不是修 bug**，原 transient 错误从未复现（A4）；handler 本身只在"没报错"时验证过 |
| C10 | **出包脚本 preflight 重写（按用户指令不拆层，只做依赖预检）** | `2026-09-25-record3.md:66-89`；`7ced7e76` | 只做了 `bash -n` + 一次负例（藏掉 Xvfb）。**没在干净容器上端到端验证过**，而 record2:1301-1305 恰好承认没有可复现环境 |
| C11 | **placement.lua:100 / wallpaper.lua:141 两处 `require("lgi")` → lgishim** | `2026-09-23-record2.md:1024-1026,1051` | 仅在冒烟层面跑过 |
| C12 | **CMakeLists.txt:143-147 `_REQUIRED_INCLUDE_DIRS` 变量名修复** | `2026-09-23-record2.md:1188-1199` | 只在**干净 build dir** 验证过；旧 build dir（带 stale CMakeCache）仍是坏的 |

### D.【文档过期/自相矛盾，非功能项但会误导】

| # | 项 | 出处 |
|---|---|---|
| D1 | `2026-09-23-record1.md` §3-4 待办列表仍写"Spike 2/3/R6 核心改造"未勾选，实际全部完成 | `2026-09-23-record1.md:313,322-324` |
| D2 | impl-plan §五"xdg-basedir API **待确认**"未更新（record1.md 已确认无阻塞） | `2026-09-23-impl-plan.md:364` vs `2026-09-23-record1.md:318` |
| D3 | spike4-phase2-01 状态快照整体过期：`lgishim_cairo.c` 2379 行（现 **2396**）、产物 9,539,912（现 **9,597,256**）、冒烟 424 项（现 **830**）、`lgishim_pango.c` 887 行（现 **1013**） | `spike4-phase2-01-scope.md:330-358` vs 实测 `wc -l` |
| D4 | spike4-phase2-07 仍把 `M.Gio={}`/`M.Rsvg={}` 列为"真正的阻塞"，**未标记已解**；且其"修法建议"（把 `lgishim.lua:34` 改成 `Rsvg = nil`）**与实际采纳方案冲突**（实际改的是 imagebox.lua 守卫） | `spike4-phase2-07-...md:18,226,464,654,662,694`；`spike4-phase2-06-...md:149,327,339` |
| D5 | spike4-phase2-01 §六 人天可信度表仍把 GLib/Gio/GdkPixbuf/测试回归标"⚠️ 推测"，实际已交付 | `spike4-phase2-01-scope.md:220-236` vs `2026-09-23-impl-plan.md:318` |
| D6 | Part 7 原计划"4 个 layout API"名字全错（是 textbox 级方法不是 `Pango.Layout` 方法），计划过期但未走正式关闭流程 | `2026-09-23-record2.md:621-624`；`2026-09-23-impl-plan.md:302,310` |

---

## 二、如果只干 3 件事

**1. 重跑 out 出包流水线（step 4 UPX 7 轮 + step 7 打包），并确认新 tarball 的 sha 不是 `4c31690`。**
理由：这是唯一"已经修好、但交付物还是坏的"项。当前磁盘上唯一的 tarball 里装的是 pango UAF 修好**之前**的二进制（record3:29,253；实测 `/_ext/tmp/awe-package/` 只有 `4c31690-*.tar.gz`，`upx.done` 时间戳 00:04 而修复 commit 在 02:13/02:57 之后）。工作量是纯重放已知步骤，风险最低、收益最高，同时直接关掉 record3 §五 的第 1、2 项。

**2. 让 spec 在 shim 环境下真跑一次（掐掉 lgi 解析顺序：`LUA_PATH` 前置 + `spec/preload.lua` 改走 `vendor.lgishim`）。**
理由：这是全部文档里**最容易被当成"验证完了"、实际上什么都没验证 shim 的一条**。Part 13 被记为"✅ 完成、97.96%、新增回归 0、0.25 人天"（impl-plan:308），但 record2:1370 写明 `LGI_SHIM_FLAG=false`，跑的是系统 lgi 0.9.2 —— 它证明的是"lib/ 迁移在真后端下没变"，shim 的 8512 行 C 代码全部只靠手写冒烟背书。spec 是唯一独立的第三方信号源。record2:1381-1384 也自认"没做，而且有一定代价"，说明代价已知且被接受过，不是不可行。

**3. 用新的 T2b 口径重跑 32 项运行验证（Alpine + Ubuntu glibc 各一遍）。**
理由：T2b 是这一轮最有价值的工具改进（能区分"进程没死"和"跑起来了"，record3:130-132），但它只装在出包冒烟的 step 6 里。而 `30/32` 这份数据是它诞生前、在 UAF 二进制上采的 —— 也就是说 record2:1116 那句"**musl-static 二进制在 musl 和 glibc 两种 libc 环境下行为完全一致，可移植性确认**"的底层数据已经作废。impl-plan:366 把这句写进了"全部批次完工"的总结里，是全文档最强的完成性表述，却最不可信。

**落选但值得说**：`D-Bus/naughty 端到端验证`（A6）覆盖面最大 —— `lgishim_gio.c` 2427 行是整个 shim 里最大的一块，而最新的冒烟日志仍然在报 `Could not connect to D-Bus session bus`，意味着 DBus 全栈（`DBusProxy`/`bus_own_name`/`DataInputStream` 等）在生产上**一行都没跑过**。没选它是因为需要先装 dbus-daemon + 造 machine-id，是一次基建工作而不是重放；A1/A2/A3 都是重放已有产物。另外 `SVG 主题降级`（A12）成本只有 0.25 人天，可以顺手带上 —— 它现在的实际效果是 Papirus/elementary 这类 100% SVG 主题会静默全空白。
