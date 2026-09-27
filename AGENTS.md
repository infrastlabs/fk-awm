# AGENTS.md

AwesomeWM 全静态构建项目（Phase 2）的智能体工作手册。给后续 AI 智能体（及协作者）看：
**项目是什么、改了哪些、怎么构建调试、操作时遵守什么规则**。

---

## 一、项目介绍

- **项目**：AwesomeWM **upstream master** 的静态构建 fork（仓库根），产物是一个 **100% 静态链接**的
  `awesome` 窗口管理器二进制 + 完整配置/资源树，部署在 X11 桌面容器
  （`docker-hdmi-desktop-desktop-1`，Ubuntu 22.04.5）的 `/usr/local/static/awm/`。
- **为什么静态**：目标环境没有/不想依赖动态库闭包；二进制 `file` 必须是
  `statically linked`，`ldd` 报 `Not a valid dynamic program`。
- **核心部件**：
  - `lib/vendor/lgishim_*.c`（`lgishim_pango/cairo/glib/gobject/gio/pixbuf.c` +
    `lgishim.lua`）：**lgi 的静态替代 shim**，由 `luaa.c` 注册进 `package.preload`，
    编译进 awesome 二进制。
  - `lgishim_pixbuf.c`：gdk-pixbuf 自解码（PNG/JPEG/XPM 魔数嗅探 + **SVG 经 librsvg**）。
  - 前缀 `/usr/local/static/awm`（编译期烤进 `config.h` 四条路径，见下）。
- **对照基准**：apt 安装的 `awesome 4.3-5build1`（Ubuntu 22.04 源），行为差异用它 A/B 验证。
- **文档（记录系列，`docs/sam/`）**：
  - `2026-09-23-record1.md`（起步决策/Part 1-9/12/13，原 `record.md` 改名）
  - `2026-09-23-record2.md`
  - `2026-09-25-record3.md`（章一~四：rc 修复/审计/产出 + 章五已迁出 + 遗留）
  - `2026-09-25-record4.md`（**真实环境实测反馈**：章一 0-13，含黑屏全链、
    菜单图标 freedesktop 定案、**13 追记：SVG 缺口 → rsvg 落地 → Greybird 配置**）
  - `docs/sam/scripts/`：构建/出包/冒烟脚本（01-12）

## 二、改造项（相对上游 awesome master）

1. **lgishim shim**（`lib/vendor/`）：静态 lgi。`_native` 在 property getter 返回
   lightuserdata（不在 metatable 里包闭包——`luaa.c:322` 会 `lua_touserdata`）。
2. **gdk-pixbuf 自解码**（`lgishim_pixbuf.c`）：`pixbuf_new_from_file` 魔数嗅探
   PNG/JPEG/XPM（12.8.11）；**SVG 走 `decode_svg()`**：`rsvg_handle_new_from_data` →
   cairo image surface → `rsvg_handle_render_cairo` → `surface_to_pixbuf`；
   `is_svg()` 跳 BOM/空白/`<?xml?>` 扫前 1KB 找 `<svg`；`HAVE_LIBRSVG` 宏保护。
3. **librsvg/libcroco 静态链**：librsvg 2.40.21 + libcroco 0.6.13，用 x11base
   `v-tint2/build.sh` 的 `STATIC=yes` 构建；链接补
   `-l:librsvg-2.a -l:libcroco-0.6.a -lxml2 -lexpat`（其余闭包链接行已有）。
4. **CMake**（`CMakeLists.txt`）：`AWESOME_LGISHIM` 开关把 shim .c 编进二进制；
   `AWESOME_REQUIRED_LDFLAGS` 静态闭包；librsvg 头目录 + `-DHAVE_LIBRSVG`。
5. **前缀/打包**：prefix `awm`（`config.h` 四条：
   `AWESOME_LUA_LIB_PATH / THEMES_PATH / ICON_PATH = .../share/awesome/{lib,themes,icons}`、
   `AWESOME_DEFAULT_CONF = .../etc/xdg/awesome/rc.lua`）；包名 `awm-static-musl`，
   程序名/配置目录名仍是 `awesome`。
6. **已知边界（别当成缺陷重查）**：
   - D-Bus：静态 libdbus 编译期 `localstatedir` 烤成 `/usr/local/var`，system bus
     socket 连不上（1 行无害警告，`naughty` 降级）——要修得重编 libdbus
     `--sysdir=/run/dbus`（未排期）。
   - `awful.layout.layouts was set before request::default_layouts` 告警来自 **apt
     原样 rc.lua:67**，是 rc 写法问题，不是 shim 缺陷。
   - **`lib/freedesktop/` 不在仓库**（裁剪树从未收录）；菜单要它时是**部署侧手工
     复制** apt 的三件套到 `share/awesome/lib/freedesktop/`（用户 2026-09-27 拍板
     「不加入仓库」）。`pcall(require, "freedesktop")` 失败 → apt rc.lua 走 Debian
     静态菜单 else 分支。
   - 图标主题：`beautiful.icon_theme`（默认 nil）→ `menubar.utils.get_icon_lookup_path()`
     （`lib/menubar/utils.lua:136`）是**缓存闭包**，必须在**首次 lookup 之前**设置；
     格式只认 png/xpm/svg。

## 三、构建与调试指引

### 环境
- **构建容器**：`builder-awe`（镜像 `x11-base:alpine-builder-gtk224`，Alpine musl +
  xx-clang 交叉链）。`builder-obox` 同镜像（openbox 构建）。
- **运行容器**：`docker-hdmi-desktop-desktop-1`（Ubuntu 22.04.5，Xvnc/TigerVNC 1.13.1 +
  Xvfb 均可用；**验证一律用 Xvnc，不再用 Xvfb**——用户 2026-10-24 拍板，理由与命令见下）。
  `:1` 是用户 live 会话（xrdp → Xvnc 后端，`Xvnc -ac :1 -listen tcp
  -rfbauth=/etc/xrdp/vnc_pass`，一直在跑），**探针一律开全新 display 号，别碰 :1**。
- **构建目录（精确路径，一律复制勿手敲——AI 模型常拼错 `_ext`/`_extc`、`awe-build-*`）**：

  | 目录 | 用途 |
  | --- | --- |
  | `/_ext/tmp/awe-build-release` | **正式出包目录**（`11-package-static-release.sh` 的 `BUILD_DIR` 默认值，前缀烤对，出包用它） |
  | `/_ext/tmp/awe-build-static` | 临时验证用（前缀可能不对，**别直接拿它的产物部署**） |

- **仓库根**：`/_ext/home/headless/xm-zs01/fk-awesomewm`（首段是 `_ext`，后面没有多余的 `c`）。
- **出包目录**：`/_ext/tmp/awe-package/`（历史包在 `archive/`）。

### 出包（正式流程，7 步）
```sh
# 宿主机直接跑会自动 docker exec 进 builder-awe
bash docs/sam/scripts/11-package-static-release.sh all
# 单步：bash ... 1（configure）/ 2（build）/ 3（checkpaths）/ 4（upx）/ 5（install）
#       / 6（smoke）/ 7（package）
```
- **`SMOKE` 默认 0（跳过冒烟）**，要冒烟 `SMOKE=1`。
- 必须 `AWESOME_IGNORE_LGI=1`（脚本内置）；`-G Ninja`；
  `-DCMAKE_EXE_LINKER_FLAGS="-static -Wl,--as-needed -Wl,--strip-all"`。
- 产物 `awm-static-musl-<ver>-<sha7>-<ts>.tar.gz`，ver 取 `git describe`
  （最近 tag `v4.3`（2019 发布）作基数；**变基会改计数**，别拿旧包号当基准；
  实际代码基于 upstream master，见 record5 五·5.1）。
- 安装：`tar -xzf <pkg>.tar.gz -C /`；启动 `/usr/local/static/awm/bin/awesome`。
- UPX 7 轮阶梯取最小（默认开；`UPX=0` 关）。

### 调试套路
- **验证环境一律用 Xvnc（TigerVNC 1.13.1），不用 Xvfb**：Xvfb 与真机 RandR/shape
  行为有差异（Xvfb `--setmonitor` 无效、单 CRTC），Xvnc 与真实 xrdp 会话同源。
  决策来源：主题仓 `fk-awesome-wm-theme/docs/2026-10-17-taglist-tasklist-impl2-multiScreen.md`
  **4.5.6.3**（2026-10-18 用户拍板，原仅限多屏类）→ **2026-10-24 用户拍板扩展到全部
  验证流程**。Xvfb 保留作纯静态截图备用。
  **探针一律开全新 display 号，禁止复用 `:1`**（见「环境」，live 会话）。
- **A/B 探针**：写 `/tmp/probe.lua`（`--config` 模式），Xvnc 新 display 上分别跑
  apt 动态版与静态版对比。参数以主题仓
  `fk-awesome-wm-theme/docs/script/xvnc-multi/lib.sh` 的 `xvnc_spawn` 为准（同源）：
  ```sh
  DISP=311   # 每次换号：root 残留壁纸会污染结果
  # 免密码 = -SecurityTypes None；-ac 关访问控制；TigerVNC 用 -geometry 不是 -screen
  Xvnc ":$DISP" -geometry 1280x800 -depth 24 -SecurityTypes None -localhost -ac \
    </dev/null >/tmp/xvnc-$DISP.log 2>&1 &
  sleep 2
  DISPLAY=:$DISP xprop -root >/dev/null || echo "!! Xvnc 没起好"   # 连通性门
  DISPLAY=:$DISP HOME=/root timeout -s KILL 8 /usr/local/static/awm/bin/awesome \
    --config /tmp/probe.lua >/tmp/out.txt 2>&1; echo "rc=$?" >> /tmp/out.txt
  pkill -f "Xvnc :$DISP"; rm -f /tmp/.X$DISP-lock /tmp/.X11-unix/X$DISP
  ```
  - **`-nopw` / `-forever` 在 TigerVNC 1.13.1 不存在**，加了直接启动失败
    （报 `Unrecognized option`）；免密码只能用 `-SecurityTypes None`。
  - `-depth 24`（live `:1` 是 16，截图像素分析统一用 24）。
  - 现成脚本封装：`fk-awesome-wm-theme/docs/script/xvnc-multi/`
    （`1_spawn.sh` 起 Xvnc+awesome / `2_multi.sh` 扩屏 / `3_shot.sh` 截图 /
    `4_kill.sh` 清理，共用 `lib.sh`；环境变量 `DISP/W/H/N/MODE/WAIT` 可覆盖）。
    **多屏/屏变化类验证一律走它 + `xrandr --setmonitor/--delmonitor`**（见 4.5.5.8）。
- **静态性/符号验证**：`file -b <bin>`（必须 statically linked）；
  二进制是 **strip 过的**，`nm` 看不到符号，用 `strings` 找符号串。
- **librsvg 重编（若改了 shim）**：builder-awe 里
  `STATIC=yes bash /src/v-tint2/build.sh libcroco` + `librsvg`；
  **别信 `err 0, pass`**（deps 阶段无质量门），看
  `/tmp/librsvg/.libs/librsvg-2.a` 是否出现，装不上就手工装
  （.a + 头文件 + pc + 保留 `.la`；libcroco 的 pc 要改名 `libcroco-0.6.pc`）。
- **图标/菜单调试**：`beautiful.icon_theme` 一行配置（rc.lua `beautiful.init` 之后、
  `freedesktop.menu.build` 之前）；`ls /usr/share/icons/<主题>/index.theme` 判断主题是否完整。

### 运维坑（实测）
- `docker cp` **多源静默失败**，须一个一个拷。
- `pkill -f Xvnc` 会杀掉外层 `bash -c`（自匹配），必须带 display 号限定：
  `pkill -f "Xvnc :$DISP"`；杀 awesome 用 `pkill -x awesome`（精确匹配进程名）。
- Xvnc 起的 display 用完要清 `/tmp/.X<NN>-lock`（单文件，非 `-lock-*`）和
  `/tmp/.X11-unix/X<NN>` socket，否则同号重起报 "Another server is already running"；
  **每次换 DISP 号**（root 残留壁纸会污染对比结果），先 `ls /tmp/.X11-unix/` 挑空号。
- `docker exec ... '脚本; cat 输出'` 常**吞掉尾部输出**——脚本输出重定向到文件，
  单独一次 exec `cat`。
- 构建/容器输出被吞时：`nohup bash -c '... > /tmp/x.log 2>&1' &` 再轮询 log。
- 探针/临时文件放 `/tmp`，**不要落进仓库**。

## 四、智能体操作行为规则（必须遵守）

1. **提交粒度**：**每个 md 文档单独一个 commit**，禁止多个 md 混在一个提交里；
   代码/脚本改动同样**一个一个单独提交**，禁止串一起。改文档+改代码必须拆开。
   **提交信息首行头部必须附带文件路径**（可适当简写，但须明确标识便于认知），
   例如 `docs/sam/2026-09-25-record4.md: 一·6.1 重新编号为 13`、
   `lib/vendor/lgishim_pixbuf.c: SVG 解码`；单文件改动就以该文件路径开头，
   多文件提交（尽量拆开）则列全。 
2. **文档历史不动**：历史正文只加 ⚠️/🔶 标注与「最新为准」块，**不删不改旧结论**；
   状态约定：`✅ 已定案/已关`、`🔶 半解/待回报`、`⬜ 未修/未做`、`⚠️ 推翻/存疑`。
3. **先落文档再给用户命令/结论**：新结论、命令、决策先写进 record3/record4 相应小节，
   再回复用户。
4. **文件名日期 == 标题日期**（如 `2026-09-25-record4.md` 标题（2026-09-25））；
   正文里的**事件日期不动**（如定案日期）。
5. **改完必须实跑验证**：二进制改动要跑探针/冒烟出证据；**别信 `err 0, pass`**。
   **验证环境一律用 Xvnc（新 display，不碰 :1），禁用 Xvfb**——用户 2026-10-24 拍板；
   命令模板见「三、构建与调试指引 / 调试套路」。
6. **分支被外部强推重建**（`+docs/...` 拆分历史）：本地 `reset --hard`
   `origin/<分支>` 前**先建 backup 分支**（如 `backup/<时间>-pre-reset-<分支>`），
   复位后 `git diff --stat <旧HEAD> HEAD` 应为空（树一致才算无损）。
7. **路径一律复制，禁止凭记忆手敲**：仓库根
   `/_ext/home/headless/xm-zs01/fk-awesomewm`（`_ext` 后无多余字符）、构建目录
   `/_ext/tmp/awe-build-{release,static}`、出包目录 `/_ext/tmp/awe-package`、
   容器名 `builder-awe` / `docker-hdmi-desktop-desktop-1` 都是精确值——拼错
   （`_extc`、少字母、`release`/`static` 混淆）会白跑，一律从本文件/文档复制。
8. **部署侧 vs 仓库侧**：用户拍板的部署侧改动（如 freedesktop 三件套、rc.lua 配置）
   只落在容器，**不进仓库**；仓库改动要有明确需求。
9. **不擅自动用户会话**：运行容器里用户 live 的 awesome 进程（`awesome -r`）别杀；
   部署新二进制前先备份（`cp awesome awesome.bak-<日期>`）。
10. **版本/出包**：新二进制走 `11-package-static-release.sh` 正式出包（前缀、UPX、
   version），**别直接拿裸 build 产物部署**（路径对不上）。
