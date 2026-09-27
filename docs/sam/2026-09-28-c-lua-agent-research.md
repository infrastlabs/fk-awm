# C+Lua Agent Research（C+Lua 智能体资料采集分析）

> **独立调研文档**，不属于 record 系列（不按 record 编号，与实现记录解耦）。
> 检索日期 2026-09-27，外部候选核验 2026-09-28。数据来自 GitHub REST API（`/repos/{owner}/{repo}/languages`、`/search/repositories`），star 数与语言占比为当日快照，会漂。
> 章一为原始检索结果，章二为对一份外部候选清单的逐条核验。

## 一、C+Lua 智能体实现盘点

### 1、结论先行：真 C+Lua 双语言实现只有 3 个

| 项目 | ★ | 语言构成 | 定位 |
|---|---|---|---|
| **[mischief/clm](https://github.com/mischief/clm)** | 12 | C 85% / Lua 8% | embeddable llm agent in c with lua plugins |
| **[theStrangeAdventurer/capstan](https://github.com/theStrangeAdventurer/capstan)** | 16 | C 63% / Lua 21% | 终端编码智能体，C 壳 + embedded Lua |
| **[espressif/esp-claw](https://github.com/espressif/esp-claw)** | 2184 | C 主语言 | 乐鑫官方，ESP 芯片上的 chat-coding agent |

这三个就是"C 自己实现内核、Lua 自己实现策略层"的全部主流候选。
编码方向（clm / capstan）仍在两位数 star，嵌入式方向已有官方两千星级项目。

⚠️ **esp-claw 占比行取自个人 fork** `liunix61/esp-claw`（C 90% / Python 3% / Lua 2%，1★）——
两个仓库的 `description` 逐字相同，官方仓库的 `/languages` 接口本轮 403，占比未取到。
仓库与 star 数已按官方值修正，只有占比这一项仍借 fork 的数据。

### 2、clm — 最贴题的一个

`https://github.com/mischief/clm` — C 85% / Lua 8% / Meson 6%

- **C17 内核**，依赖只到 cJSON。`libclm` 是可裁的：`-Dlua=disabled` 编出完全无 Lua 的核心（连 `libclmlua` 和 `clm` 二进制一起省掉），配 `esp32/README.md` 的从零 embedder 示例
- 核心声明的依赖面：transport + timers 走一个薄 host interface 进来，所以 agent engine 本身不依赖 libuv/libcurl
- **Lua 5.4 插件系统**：沙箱 VM，禁 `os` / `io` / `require`，8 MiB 内存上限 + CPU timeout
- **双协议**：OpenAI 兼容（llama.cpp / Ollama / OpenAI / Groq）+ Anthropic 原生 Messages API，请求/响应层透明翻译，Anthropic 侧自动 prompt caching
- 内置 shell exec、file read/write；MCP 客户端（stdio + HTTP）
- ncurses TUI + headless CLI 两种模式
- 构建：meson ≥ 1.1、C17、libcurl、libuv、cJSON、ncursesw、md4c、Lua 5.4

### 3、capstan — 分工写得分明

`https://github.com/theStrangeAdventurer/capstan` — C 63% / Lua 21% / Python 12%

- C99 + `-Wall -Wextra -Werror`，vendored ncurses/Lua
- **分工**：C 管 terminal/runtime 边界，embedded Lua 管 agent policy 和扩展行为
- **Lua 插件可加**：slash 命令、autocomplete、model tools、OAuth provider、lifecycle hooks，且 **TUI hot reload 不用重编核心**
- 支持 ACP、MCP、显式 tool 权限、profiles、并行 subagent、headless 自动化
- 外部依赖 ncurses + libcurl

### 4、esp-claw — 嵌入式场景

**[espressif/esp-claw](https://github.com/espressif/esp-claw)** — 主语言 C，2184★（乐鑫官方，当日快照）

⚠️ **占比与下列细节取自个人 fork** [`liunix61/esp-claw`](https://github.com/liunix61/esp-claw)
（C 90% / Python 3% / Lua 2%，1★）：两个仓库的 `description` 逐字相同，官方仓库的
`/languages` 接口本轮 403 取不到占比。占比这一项借用 fork 数据，架构描述仍适用。
详见章二 §4。

- ESP 芯片上跑，LLM 决策 + **Lua 确定性规则**双轨：LLM 可动态决策，Lua 走确定性规则
- 生成的 Lua 脚本验证后**持久化成本地规则**，LLM 服务挂了或模型换了也能稳定执行
- MCP 双 Server/Client 身份
- Lua 模块：display / lcd_touch / button / camera / audio / led_strip / gpio / mcpwm / storage / delay / event_publisher
- 结构：`application/basic_demo` + `components/`（claw_modules / claw_capabilities / lua_modules）

### 5、对照组：容易被算进来但不在"C+Lua 自己实现"这一层

**纯 C，没 Lua（这层才是主流体量）**

| 项目 | ★ | 说明 |
|---|---|---|
| `sqliteai/adam` | 124 | C 99%。"SQLite of agent frameworks"，13 个内置 tool，可嵌进 SQLite / PostgreSQL 当 SQL 扩展 |
| `Rakumoji/AgenC` | 0 | C 92%，感知/认知/规划/动作/多智能体协作 |
| `NeverSight/NeverCode` | — | 纯 C 编码智能体，2MB 二进制，58 个内置 tool，408 个测试文件 |
| `lzbgt/agent-c` | — | 名字带 C，实际 TS 31% + C++ 22%，不是 C |

**Lua 写 agent、C 只是宿主**

| 项目 | ★ | 说明 |
|---|---|---|
| `skitsanos/ironcrew` | 2 | agent/task/tool/orchestration 全写 Lua，但宿主是 **Rust 80%**，不是 C |
| `avante-corp/avante.nvim` | 18164 | Lua 83%，Neovim 里的 AI 编码智能体 |
| `folke/sidekick.nvim` | 2777 | Lua 100%，Neovim AI sidekick，接 claude-code / codex-cli / copilot / gemini-cli |
| `Robitx/gp.nvim` | 1320 | Lua 100%，GPT prompt，多 provider |
| `frankroeder/parrot.nvim` | 792 | Lua 100%，Neovim 里的随机鹦鹉 |

Neovim 系是 Lua-agent 的实际体量所在（万星级），但那是宿主红利，跟"C 自己实现"不是一回事。

### 6、观察：三个 C+Lua 项目分工完全一致

**C 占 runtime 边界（terminal / transport / timers），Lua 占策略与扩展。**

clm 里那句"transport + timers come in through a small host interface"是同一个设计意图：让 Lua 层不依赖 libuv/libcurl，只对着薄接口编程。capstan 表述得更直白："C hosts the terminal/runtime boundary; embedded Lua owns agent policy and extension behavior."

和 fk-awesomewm 里 lgishim 是同一个套路：C 侧做 shim 层把系统 API 收口成窄面，Lua 侧只管业务。差别在方向——那边是 **C 追 Lua**（补 Lua 缺的系统绑定），这几个是 **Lua 追 C**（用 Lua 换掉 Python 运行时）。

### 7、检索的局限

- 只按 GitHub REST API 的 `language` 字段判定，占比低于 1% 的语言会被截掉（ironcrew 的 Lua 就是这种情况，实际有 Lua 脚本层但占比显示 0%）
- GitHub 搜索噪声大，关键词组合覆盖不全，可能有漏网的小项目
- star 数是当日快照
- `/repos/{o}/{r}/languages`（逐项目占比）比 `/repos/{o}/{r}`（主语言字段）更容易撞匿名限流 403；本轮核验占比接口全 403，只有主语言字段可用

可继续深挖的方向：C+Lua 的 LLM **推理**层（llama.cpp 的 Lua 绑定那几个）、或放宽语言纯度只看 star 数。

---

## 二、外部候选清单核验（2026-09-28）

> 来源：另一渠道整理的一份 C+Lua 智能体清单（编码 / 嵌入式 / 安全 / 游戏 / 机器人 六类，约 25 项）。
> 逐条打 GitHub REST API `/repos/{owner}/{repo}` 复核存在性与 `language` 主语言字段。
> 未做任何采信——**结论以 API 返回为准**。

### 1、核验方式

请求 `/repos/{owner}/{repo}`，取 `stargazers_count` 与 `language`。注意 `language` 是 GitHub 自己的
"主语言"启发式（按代码行数占比最大者），**不是** Lua/C 双层结构判定；要确认双层结构仍需翻代码，
本轮未做。占比接口 `/repos/{o}/{r}/languages` 本轮全部 403，故下文占比一律不写，只写主语言。

### 2、结果：新增可用项目 0 个，可用修正 1 个

| 项 | 状态 |
|---|---|
| 清单里的 25 项 | 真实存在 18 项，404 查无 8 项 |
| 真实且主语言为 C、可入"C+Lua 自己实现"层 | **0 项** |
| 真实但主语言不是 C（Rust/Zig/TS/Lua/C++） | 11 项 |
| 需修正本文件既有内容的 | 1 项（esp-claw，见章一 §4） |

章一的核心判断——**真 C+Lua 双语言实现极少且体量都很小**——核验后**不变**，
只是因为 esp-claw 的仓库认错，"都很小"这一半被削弱（官方 2184★）。

### 3、404 清单：查无此仓库，不可采信

以下全部返回 `HTTP 404`，架构描述无法从公开仓库核实，视为不存在或不公开：

| 声称的项目 | 尝试的仓库路径 |
|---|---|
| Realtek Ameba-Claw | `realtek/ameba-claw`、`Realtek/Ameba-Claw` |
| TuyaOpen TClaw | `TuyaOpen/TClaw`、`TuyaOpen/tclaw` |
| Inanimate Resident | `Inanimate/Resident`、`inanimate/resident` |
| Virgo Agent Toolkit | `virgo/virgo-agent-toolkit` |
| Luai | `shellntel/Luai` |

另外两项状态特殊：`doomspork/luagents` 返回 403（仓库存在但匿名不可读）；
`xmake ai` 自称 xmake addon 非独立仓库，无 star，不构成候选。

这批 404 的有一个共同点：都是**厂商名 + Claw/Agent 后缀**的嵌入式项目。
这种命名模式配合"约 1.3K Stars"这类带约数的描述，是典型的记忆错构，不是检索遗漏。

### 4、主语言不是 C 的，逐条列出

| 项目 | ★ | GitHub 主语言 | 声称 |
|---|---|---|---|
| `SanMuzZzZz/LuaN1aoAgent` | 1315 | **TypeScript** | Lua |
| `tontinton/maki` | 1127 | **Rust** | Zig |
| `Lommix/blitzdenk` | 11 | **Zig** | Zig（吻合） |
| `leonardcser/smelt` | 52 | **Rust** | C+Lua |
| `caioaao/onetool` | 5 | **Rust** | Lua 运行时 |
| `zentinelproxy/zentinel-agent-lua` | 1 | **Rust** | Lua |
| `2aius/d2ai` | 27 | **C++** | C++ 服务器 + Lua |
| `timn/ros-luaagent` | 4 | **Lua** | Lua agent |
| `snidercs/luabot` | 0 | **Lua** | LuaJIT + C++ FFI |
| `Sysmic-Robotics/engine` | 1 | C | C++/Qt6 + Lua |
| `ziyao233/a1ex` | 2 | **Lua** | Lua |

硬反例值得单独记一条：**LuaN1aoAgent 名字带 Lua，主语言是 TypeScript**（1315★，描述吻合）。
这是本轮唯一一个"star 数不小 + 名字骗人"的样本，说明用关键词搜 Lua 智能体会被
LuaJIT 绑定、Lua 插件层、Lua 命名致敬三类项目污染。

### 5、唯一勉强可用的：`Ugbot/boggart`

4★，描述字面吻合（"A tiny coding agent with a C core and an embedded Lua r…"），
但本轮 `/languages` 全 403，**无法确认 C 占比**，只能确认主语言字段是 Lua。
按章一 §7 的截断规则，主语言显示 Lua 时 C 占比低于阈值的可能性很大，
所以它**不并入**章一的"C+Lua 自己实现"层，留作待复核。

### 6、结论

- 章一结论维持：**3 个真 C+Lua 实现**（clm 12★ / capstan 16★ / esp-claw 官方 2184★）。
- 需要修正的只有量级描述：esp-claw 不是"很小的嵌入式项目"，是乐鑫官方、两千星级。
  章一 §4 已加 ⚠️ 标注，旧结论未删。
- 外部清单的价值在于提供了一个**反例库**：Rust/Zig 宿主 + Lua 插件层，
  与章一 §6 的"Lua 追 C"观察正好互补——主流路线是**宿主用 Rust/Zig，Lua 只做插件扩展**，
  而不是 C 做内核 Lua 做策略。真 C 内核 + Lua 策略层这个组合，目前只有这 3 个。
