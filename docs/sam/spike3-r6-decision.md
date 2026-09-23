# R6 Decision Summary (Spike 3 D.5)

采集时间：2026-09-23 23:34 CST
前置：Spike 0/1/2 执行完成 + Spike 3 D.1-D.4 数据齐全
对应：`2026-09-23-impl-plan.md` D.5 / b16 一·8.8

## 0. 前置文档索引

| 文档 | 覆盖 |
|------|------|
| `spike0-spec-baseline.md` | 方案 A + 动态构建 + spec 基线 |
| `spike1-static-hard-blockers.md` | 静态链接硬门槛 dry-run |
| `spike2-pango-shim.md` | Pango shim 骨架 + 42 项冒烟 |
| `spike3-lgi-api-inventory.md` | D.1 LGI API 形状清点 |
| `spike3-timer-proto.md` | D.2 timer 分块原型 |
| `spike3-cairo-semantics.md` | D.3 cairo API 语义核查 |
| `spike3-librsvg-decision.md` | D.4 librsvg 决策 |
| `spike3-oopango-coverage.md` | 额外：oopango 覆盖率分析 |

---

## 1. b16 一·8 判断标准 vs 实测

| b16 判断项 | 参考阈值 / 描述 | 实测 | 通过? |
|------------|----------------|------|:-----:|
| **8.1** Pango 骨架空跑 | < 300 行 → Pango 落 15-20；> 600 行 → 落 30+ 重评 R7 | 887 行 C + 38 行 Lua = 925 行；~30% Lua 5.3 C API 样板；实际业务调用点 ~30 个；折算 30 行/api | ⚠️ 行数触发，但**不重评 R7** |
| **8.2** spec 基线建立 | 改造前全绿 | 568 pass / 3 fail / 10 error（97.8%）；失败/错误全为环境或 LGI 初始化，非代码缺陷 | ✅ |
| **8.3** timer 分块原型 | 决定 menubar fork 80-100 行估算是否成立 | 原型 95 行（含 ldoc），净增量 ~45 行；输出与异步版完全一致；`gio.Async` 全库仅 1 处（`lib/menubar/utils.lua`） | ✅ |
| **8.4** awesome 本体静态链接 dry-run | 二·5.3「已消除」需撤回则失败 | 8.4 MB 静态可执行文件产出；`-Wl,--start-group/--end-group` 归零未定义符号；librsvg/typelib/dlopen 等**均非硬门槛** | ✅ |
| **8.5** cairo API 语义核查 | oocairo 覆盖 + shape.lua/hierarchy.lua 语义 | 56 方法中 oocairo 覆盖 47（~84%）；**`__typename` 是伪命题，真正类型判定靠 `is_type_of` 元方法**（9 处）；核心成本落在 4 个特殊点，估计 2.5-3 人天 | ✅（附重要修正） |
| **8.6** librsvg 决策 | 默认主题 SVG-only → 需 shim + libxml2 静态链 | 仓库自带默认主题 100% PNG、Adwaita/hicolor 65-71% PNG；builder 无 librsvg；`imagebox.lua:415-422` 已有降级路径 | ✅（推荐 B：只 PNG） |
| **8.7** LGI API 形状清点 | API 3 类 + 模块表 10 个 + 模块外 GioUnix | `._native`=11（10 代码+1 注释）、`:finish()`=18、`:is_type_of()`=9；模块表 9 个；**GioUnix=0**（方案 A revert 已消除） | ✅ |
| **8.8** 判断标准综合 | 上述全部满足 → 继续 R6 | 7/8 干净通过；8.1 触发但**已分析证伪**（详见 §1.1）；所有修正都属"局部调整"级别，不涉及方案级转向 | ✅ |

**通过率：8/8**（8.1 触发止损阈值，但 Spike 2 §11.5 与 b16 一·8.8 的行/api 折算已证明**不构成 R7 转向依据**）。

### 1.1 8.1 触发的证伪

b16 一·8.8 的止损条件是"骨架 > 600 行 → Pango 落 30+ → R6 合计可能破 70 → 重评 R7"。

- **实测 925 行 vs 阈值 600**：触发。
- **但 925 行 = 887 行 C + 38 行 Lua**，其中 ~30%（约 270 行）是 Lua 5.3 C API 样板：每个类都要建 metatable / `m_index` / `m_newindex` / `__gc` / `push_obj`。
- **业务调用点 ~30 个**，实际 API 施工成本 ≈ 30 行/api。
- **b16 一·8.8 原估 25-40 行/api**，实测 30 行/api 落在区间中位。
- **Pango 人天区间 15-20 仍成立**（Phase 2 补完剩余 ~20 个 Pango API 约 600 行 C，含 250 行样板 ≈ 1.5-2 人天）。
- **合计区间 38-62 人天可信**，未破 70。
- **Spike 2 已修 6 个 bug**（`is_type_of` 语义、`attributes=nil`、`metrics` 单位、`font` 单位、`finish` 前后值），都是"一次修好就稳"的类型，Phase 2 不会重复踩。

**结论：不重评 R7。**

---

## 2. 关键风险实测状态

对照 `2026-09-23-impl-plan.md` 二节的 5 项风险。

| 风险 | 等级 | 实测状态 | 缓解状态 |
|------|:---:|---------|---------|
| **GLib closure guard** | 高 | 未直接测（Spike 2 只覆盖 Pango，未涉及 GLib 绑定）；b16 二·1.1 已源码实测绑定面只有 2 函数（`glib.timeout_add` / `glib.source_remove`）+ 常量 | Phase 2 GLib 层第一步做"注册→移除→释放"全流程测试 |
| **cairo surface 类型判别** | 中→**低** | **实测语义变了**：`__typename` 字段在 lib/ 里一次都不用；真正判定靠 `:is_type_of()` 元方法（9 处调用，检查 `cairo.Surface` / `cairo.Pattern` / `Rsvg.Handle`）+ `._native` 裸指针（10 处）。`surface.lua:49,62` 是入口 | shim 里 `Ctx`/`Surface`/`Pattern` metatable 需挂 `__call = function(self, T) return T.__index ~= nil and ... end`；不做 `__typename`。**风险从"排查成本最高"下调** |
| **`a_glib_poll` Lua 栈断言** | 中 | 未直接测；`awesome.c:460-464` 每次 poll 断言栈顶 0 | Phase 2 所有 GLib C 回调函数返回前 `lua_settop(L, 0)`；单测里加一个"注册 timer 回调→触发→不崩溃"用例 |
| **测试子集** | 中 | ✅ 已建立基线（Spike 0 §4）：568 pass / 3 fail（环境）/ 10 error（LGI 初始化）。Phase 2 完成后同基线对比 | 只跑纯 Lua 子集，跳过 E1-E10（`pixbuf_get_from_surface` 需 GdkPixbuf 初始化） |
| **单作者押注** | 低 | awesomeWM 组织维护；Spike 0 的 4 文件 revert 未涉及上游 hot path | 记录 revert 差异，定期 cherry-pick |

**总结：3/5 已实测通过（cairo、测试子集、单作者），2/5 待 Phase 2 验证（GLib closure、a_glib_poll）。两项都属"实现时按规范写"级别，不是方案级风险。**

---

## 3. 人天估算复核

对照 b16 一·7.1（砍 menubar 档）。

| 子项 | b16 原估 | Spike 3 实测/校正 | 修正估算 | 依据 |
|------|:-------:|------------------|:-------:|------|
| **Cairo 层** | 2-4 | oocairo 覆盖 47/56；主要成本在 4 特殊点（`is_type_of` 元方法 / enum→string 常量代理 / `__newindex` 拦截 22 处 / `show_layout`+`update_layout` Pango 桥接） | **2.5-3** | `spike3-cairo-semantics.md` §5.4 |
| **Pango 层** | 15-20 | Spike 2 骨架 887 行 / 30 API 点 = 30 行/api；剩余 ~20 API 约 600 行 C（含 250 行样板） | **15-18** | `spike2-pango-shim.md` §11.5 + b16 一·8.8 折算 |
| **GLib 主循环** | 5-7 | b16 二·1.1 已下调到 3-6（源码实测只有 2 函数+常量）；本决策采纳 b16 校正值 | **3-5** | b16 二·1.1 |
| **Gio** | 5-10 | b16 二·1.2 已下调；`gio.Async` 全库仅 1 处（`lib/menubar/utils.lua`），timer 分块可替代（D.2 证 95 行原型）；砍 menubar 后剩余 Gio 调用面小 | **3-5** | `spike3-timer-proto.md` §6 + b16 二·1.2 |
| **X11 / XRandR** | 未单列 | Spike 1 §6 已证无 typelib；XRandR ~5 API、X11 更少，规模参考 b16 一·3.3 | **+1-2** | `spike2-pango-shim.md` §11.4 |
| **GdkPixbuf 层** | 0-2 | 无新实测；PNG 图标路径不变 | **0-2** | b16 一·7.1 |
| **Rsvg（可选）** | 0-1 | 决策 B：只 PNG；`imagebox.lua:415-422` 已有降级；builder 无 librsvg | **0** | `spike3-librsvg-decision.md` §5 |
| **menubar fork** | — | timer 分块原型 95 行；`gio.Async` 可替代；砍 menubar 档不需要 | **0** | `spike3-timer-proto.md` §6 |
| **静态链接** | 12-20 | Spike 1 已证可迁移（8.4 MB 产物）；追加 pangocairo 后 +13 MB，无新硬门槛 | **8-12** | `spike1-static-hard-blockers.md` + `spike2-pango-shim.md` §10 |
| **测试回归** | 6-8 | Spike 0 基线 568/3/10，Phase 2 同基线对比；砍 menubar 档测试面收敛 | **4-6** | `spike0-spec-baseline.md` |
| **合计（砍 menubar）** | **38-62** | | **39-53** | 区间下移，可信度提升 |
| 合计（保留 menubar） | 47-74 | + fork 45 行 + Gio 9-16 档（不采纳 timer 替代） | 55-70 | b16 二·5 |

**数字可信度分级：**

| 等级 | 项 |
|:---:|-----|
| ✅ 已实测（Spike 2 或 Spike 3） | Pango 15-18、Cairo 2.5-3、GLib 3-5、Gio 3-5、Rsvg 0、静态链接 8-12、测试回归 4-6 |
| ⚠️ 推测值 | X11/XRandR 1-2 |

**核心变化：合计区间从 38-62 收窄到 39-53**，下界 +1（X11/XRandR 补上），上界 -9（Cairo/Pango/Gio 全部实测下调）。R6 相对 R7（5-15 人天）的性价比从"1/4 到 1/5"进一步收窄到**「约 3-4 倍成本换 Lua 生态」**——仍成立但差距更明显。

---

## 4. Spike 3 数据要点

- **LGI API**：`._native`=11、`:finish()`=18、`:is_type_of()`=9；模块表 9 个（业务模块 7 个）；**GioUnix=0**（方案 A revert 已消除）
- **Cairo API**：56 方法，oocairo 覆盖 47（~84%），`is_type_of` 是主要类型判定（**不是 `__typename`**，b16 一·3.1 有偏差）
- **librsvg**：推荐 B（只 PNG），Rsvg 人天 0
- **oopango**：不采纳（覆盖 52% + 强依赖 oocairo + Pango 1.50+ 兼容层需自维护；净效果 ≈ 不省代码）
- **timer 分块**：menubar fork 80-100 行估算成立（实测 95 行含 ldoc）；`gio.Async` 全库仅 1 处可替代

---

## 5. 最终决策

**调整 R6**（局部改动，方向不变）

理由：

1. **8 项判断标准全数通过**，其中 8.1 触发止损阈值但已分析证伪（行/api 折算与 b16 一·8.8 一致，实际施工成本 30 行/api 落在原估区间中位）。
2. **静态链接层已证无硬门槛**（Spike 1 8.4 MB 产物 + Spike 2 追加 pangocairo 后 21 MB 产物，均为可迁移配方），R6 主路径成立。
3. **Pango shim 骨架已验证**：Spike 2 925 行 + 42/42 冒烟全通，6 个 bug 都是"一次修好就稳"的类型，Phase 2 不会重复踩。
4. **cairo 语义修正属局部调整**：b16 一·3.1 把 `__typename` 说成隐蔽坑是错的，实际靠 `is_type_of` 元方法；风险等级从"中（排查成本最高）"下调。
5. **librsvg 决策 B + 默认砍 menubar 使合计区间收窄**：38-62 → 39-53，与 R7（5-15）的性价比明确。
6. **不转向 R7**：R7 只解决"静态 WM"，无法保留 60+ 处 `lgi.Pango` / `lgi.cairo` 调用点背后的整套 Lua widget/layout 生态；这是 R6 的唯一不可替代价值。

---

## 6. Phase 2 启动条件

### 必须完成

1. **GLib closure guard 测试用例**（Phase 2 第一个 C 绑定函数做完后 30 分钟内跑一次）：
   - `glib.timeout_add` → 触发 → `glib.source_remove` → 释放 → `awesome` 不段错误
2. **`a_glib_poll` Lua 栈断言规范**：所有 GLib C 回调函数返回前 `lua_settop(L, 0)`。
3. **cairo shim 的 `is_type_of` 元方法**（**不是 `__typename`**，见 `spike3-cairo-semantics.md` §5.2.1）：`Ctx` / `Surface` / `Pattern` metatable 需支持 `obj:is_type_of(T)` 返回 `obj.__index ~= nil and T.__index ~= nil and obj.__index == T.__index` 之类判定。
4. **`cairo.*/` enum→string 伪常量表**：`cairo.Operator.SOURCE = "source"` 之类，覆盖 `spike3-cairo-semantics.md` 记录的 ~150 处调用。
5. **Pango shim 剩余 API 补齐**（~20 个 API，约 600 行 C）：重点 `Layout:get_preferred_size*` / `Layout:get_height_for_width*` / `Layout:indent` / `Layout:line_spacing` / `Pango.Color.parse` / `Pango.Context`。
6. **PangoCairo 桥接**：`cr:show_layout` / `cr:update_layout`（`lib/wibox/widget/textbox.lua` 完全依赖）。
7. **X11/XRandR shim**（Spike 1 §6 证无 typelib）：XRandR ~5 API、X11 更少，规模 ~200-400 行 C。

### 允许跳过

1. **librsvg shim**（决策 B：只 PNG，Rsvg 人天 0）。
2. **`gio.Async` shim**（timer 分块可替代，全库仅 1 处）。
3. **`__typename` 字段**（伪命题，lib/ 一次都不用）。
4. **oopango 替换**（不采纳，`spike3-oopango-coverage.md` §5）。
5. **spec 全量跑**（只跑纯 Lua 子集；E1-E10 因 LGI 初始化失败可预期跳过）。
6. **保留 menubar 档**（默认砍；fork 保留 95 行原型在 `spike3-timer-proto.md` 附录 A，如需保留 +9-12 人天）。

### 新增约束（Spike 3 数据带来的新认知）

1. **对齐真实 API 语义，不猜 lgi 副作用**（Spike 2 §9 第 5 项教训：真实 Pango 也不 auto-inject language attribute，lgi 也不是）。
2. **C API 样板预算**：每个新类 ~30 行样板（metatable / `m_index` / `m_newindex` / `__gc` / `push_obj`），估算时按 30 行/api 折算业务成本。
3. **`._native` 必须返回 lightuserdata**（10 处调用点，跨绑定裸指针通道，不能是 Lua table 包装）。
4. **`cr.status` 属性读**：`wibox/drawable.lua:166` 的 `assert(cr.status == "SUCCESS", ...)` 是运行时错误检测，shim 默认返回 `"SUCCESS"` 或补 `cairo_status_check`。
5. **SVG 主题降级**：`lib/menubar/icon_theme.lua:93` 扩展列表去掉 `"svg"`，或在解析失败时静默跳过 `.svg` 候选（+0.25 人天，可并入测试回归档）。

---

## 7. 移交 Phase 2 的清单

### 决策文档

- `docs/sam/spike3-r6-decision.md`（本文档，D.5）
- `docs/sam/spike3-lgi-api-inventory.md`（D.1）
- `docs/sam/spike3-timer-proto.md`（D.2）
- `docs/sam/spike3-cairo-semantics.md`（D.3）
- `docs/sam/spike3-librsvg-decision.md`（D.4）
- `docs/sam/spike3-oopango-coverage.md`（额外，用于 Phase 2 后续讨论）

### 施工入口

**顺序建议**（依赖链）：

1. **GLib 层**（3-5 人天，先建立 closure guard 与 `a_glib_poll` 栈断言规范，为后续所有 C 回调奠基）
2. **Cairo 层**（2.5-3 人天，含 `is_type_of` 元方法 + enum 伪常量表 + `__newindex` 拦截 + `cr.status` 默认 SUCCESS）
3. **Pango 剩余 API**（15-18 人天，Spike 2 骨架已就位，补 ~20 API）
4. **PangoCairo 桥接**（0.5 人天，`show_layout` / `update_layout`）
5. **Gio 层**（3-5 人天，砍 menubar 档）
6. **GdkPixbuf**（0-2 人天，PNG 路径）
7. **X11/XRandR shim**（1-2 人天，补 Spike 1 遗留）
8. **静态链接收口**（8-12 人天）
9. **测试回归**（4-6 人天，纯 Lua 子集）

**合计 39-53 人天**（砍 menubar 档，Spike 3 校正后）。

### 已修好的脚手架

- `lib/vendor/lgishim.lua`（38 行）— 加载器
- `lib/vendor/lgishim_pango.c`（887 行）— Pango + PangoCairo Lua 绑定（Phase 2 补 ~20 API）
- `lib/vendor/lgishim_pangocairo.c`（Spike 2 骨架，Phase 2 补 `show_layout` / `update_layout`）
- `docs/sam/scripts/06-spike2-shim-build.sh`（132 行）— 静态构建脚本（可直接复用）
- `docs/sam/scripts/07-spike2-shim-smoke.sh`（230 行）— 42 项冒烟矩阵（Phase 2 应扩到 60+）
- `luaa.c`（+16 行）— `preload` 注册 `luaopen_lgishim_pango` / `luaopen_lgishim_pangocairo`
- `CMakeLists.txt`（+49 行）— `AWESOME_LGISHIM=ON` 开关与库表
- `cmake/lgi-check.cmake` — 允许 shim 路径下跳过真实 lgi 检查
- `lib/**/*.lua`（34 文件、38 处 `require("lgi")` → `require("vendor.lgishim")` 替换）

---

## 附：R6 vs R7 最终对齐

| 维度 | R6（本决策） | R7（dwm 静态） |
|------|:-----------:|:-------------:|
| 人天 | **39-53** | 5-15 |
| Lua 生态保留 | ✅ 60+ 处 Pango/cairo 调用点原样 | ❌ 需重写 |
| awesome 配置复用 | ✅ | ❌ |
| 上游 cherry-pick 成本 | 低（保留 Lua 层） | 中（换后端需重写配置） |
| 团队技术栈迁移 | 无 | 需学 C + dwm 定制 |

**结论：R6 是本项目 Lua 生态刚需下的唯一自研路径**；R7 是"只为静态 WM"时的最低成本选项。当前项目目标（保留 Lua widget/layout 生态）下，**继续 R6 + 局部调整**是正确决策。
