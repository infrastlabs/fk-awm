# Spike 1 — 静态链接硬门槛

**日期：** 2026-09-23
**状态：** ✓ 完成
**分支：** `feat/ai-260923-static`
**结论：** **无硬门槛。** 全静态链接成功，`awesome` 8.4 MB 静态可执行文件已产出。

---

## 目标

不做 shim，仅用「环境已就绪的静态库 + awesome 原始源码」尝试全静态链接，找出**真正的硬门槛**（无法静态化的依赖，如 `dlopen` / typelib 动态加载）。这是 R6 能否推进的关键数据。

---

## 结论（先给答案）

| 问题 | 答案 |
|------|------|
| 有真硬门槛吗？ | **没有。** 238 个未定义符号 100% 由现存 `.a` 提供 |
| 失败性质 | 纯「静态链接传递依赖缺失」，非架构性障碍 |
| 是否需要转 R7 | **不需要。** 静态链接层面 R6 可行 |
| 剩余风险 | 运行时（`dlopen` typelib、`g_module_open` 加载 image loader）→ 归 Spike 2 |
| 静态产物 | `awesome` 8,423,736 B，`statically linked, stripped` |

---

## 方法

参考 `docker-x11base/compile/src/v-pcmanfm/build.sh` 的 `OB_LIBS` 配方，逐轮：

1. 把 transitive 静态库追加到 `build.ninja` 的 `awesome` 目标 `LINK_LIBRARIES`
2. `ninja awesome` 链接
3. `spike1_extract.py` 抽取未定义符号并对照 `/usr/lib`、`/usr/local/lib` 下 144 个 `.a` 逐个验证可解析性

---

## 迭代记录

（时间 2026-09-23，容器 UTC + 8h，取自产物 mtime）

| 时间 | 轮次 | 做法 | undefined-ref 行数 | 去重符号数 | 可解析 |
|------|------|------|------------------:|-----------:|:------:|
| 18:37 | baseline | cmake 原样 `-static` | 502 | 238 | 238/238 |
| 18:40 | round 0 | + OB_LIBS（pcmanfm 同源） | 50 | 27 | 27/27 |
| 18:44 | round 1 | + `gmodule/mount/uuid/blkid` | 33 | 20 | 20/20 |
| 18:46–18:50 | round 2–4 | 修正 ninja patch 脚本（见末节踩坑） | — | — | — |
| **18:50:41** | **round 5** | + `-Wl,--start-group / --end-group` 包裹 | **0** | **0** | — |

**18:51:27 冒烟运行：** `./awesome` → `main:685: cannot open display (error 5)`，只失败在无 DISPLAY。

**关键转折是 round 5**：前几轮靠「加库 + 手工排序」只能从 238 降到 20，剩余 20 个符号全部来自**静态库顺序问题**（`-lexpat` 排在 `-lgio-2.0` 之前，`--as-needed` 下单向扫描直接漏掉）。用 `--start-group`/`--end-group` 让 `ld` 反复扫描直到符号收敛，一次性归零。

---

## baseline 的 238 个符号按库归类

| 提供库 | 符号数 |
|--------|-------:|
| `/usr/lib/libpixman-1.a` | 52 |
| `/usr/lib/libpng.a` | 40 |
| `/usr/lib/libgio-2.0.a` | 36 |
| `/usr/lib/libfreetype.a` | 27 |
| `/usr/lib/libjpeg.a` | 22 |
| `/usr/lib/libfontconfig.a` | 19 |
| `/usr/lib/libffi.a` | 10 |
| `/usr/lib/libxcb-shm.a` | 10 |
| `/usr/lib/libpcre.a` | 9 |
| `/usr/lib/libgmodule-2.0.a` | 3 |
| `/usr/lib/libxcb-image.a` | 3 |
| `/usr/lib/libXau.a` | 2 |
| `/usr/lib/libX11-xcb.a` | 1 |
| `/usr/lib/libXdmcp.a` | 1 |
| `/usr/lib/libxcb-render-util.a` | 1 |
| **合计** | **238（0 个无解）** |

来源分布：`libxcb.a`(Xau/Xdmcp)、`libgdk_pixbuf-2.0.a`(gio/gmodule/png/jpeg)、`libgobject-2.0.a`(ffi)、`libcairo.a`(pixman/freetype/fontconfig)、`libfontconfig.a`(uuid)、`libgio-2.0.a`(expat/libmount/blkid)、`libstartup-notification-1.a`(`XGetXCBConnection`)。

**符号来源全部是常规 C 静态库，没有一个是 `dlopen` / `dlsym` / `typelib` 相关** —— 这就是「无硬门槛」的直接证据。

---

## 可迁移配方

### 追加库表

```
-Wl,--start-group
  <cmake 原有 LIBRARIES 原样保留>
  -lxcb-image -lxcb-shm -lxcb-render-util -lX11-xcb
  -lXdmcp -lXau -lXext -lXft -lfreetype -lpng -lXrender
  -lexpat -lxml2 -lz -lbz2 -llzma -lbrotlidec -lbrotlicommon
  -lfribidi -lharfbuzz -lgraphite2
  -lgio-2.0 -lgobject-2.0 -lglib-2.0 -lgmodule-2.0
  -lpcre -lffi -lpixman-1 -ljpeg -lfontconfig
  -lmount -luuid -lblkid
  -lXinerama -lXfixes -lXrandr -lXcomposite
  -lm -lc -ldl -lpthread -lrt -lutil -lcrypt
-Wl,--end-group
```

### 链接标志

```
CMAKE_EXE_LINKER_FLAGS="-static -Wl,--as-needed -Wl,--strip-all"
```

（ninja 自身还会追加 `-Wl,--export-dynamic -rdynamic`，静态链接下无害。）

---

## 产物验证

```
$ file awesome
awesome: ELF 64-bit LSB executable, x86-64, version 1 (SYSV),
         statically linked, stripped
$ ls -la awesome
-rwxr-xr-x 8423736
$ ldd awesome
/lib/ld-musl-x86_64.so.1: awesome: Not a valid dynamic program   ← 确认无动态依赖
```

### 冒烟运行

```
$ timeout 10 ./awesome
2026-09-23 10:51:27 E: awesome: main:685: cannot open display (error 5)
```

只失败在打开 X 显示（容器无 DISPLAY）——**静态链接本身零问题**，代码路径走到了 X11 初始化。

---

## 附带发现

### 1. 自编译 lua 的 LUA_PATH 不含 `/usr/share/lua/5.3`

`/usr/local/bin/lua` 的模块搜索路径只有 `/usr/local/share/lua/5.3`、`/usr/local/lib/lua/5.3`、`./`，**不含 `/usr/share/lua/5.3`**。而 `lgi` 实际装在 `/usr/share/lua/5.3/lgi/`（含 `version.lua`）。

这就是 `lgi-check` 每轮报 `module 'lgi.version' not found` 的原因——**不是 lgi 没装，是搜索路径缺失**。影响：

- 静态构建期：只能靠 `AWESOME_IGNORE_LGI=1` 绕过（当前做法）
- **运行时**：awesome 会同样找不到 `lgi`，必须修复

修复选项（择一）：① lua 重编时 `--with-lua-prefix=/usr`；② 运行时设 `LUA_PATH`；③ 把 `/usr/share/lua/5.3` 软链到 `/usr/local/share/lua/5.3`。**推荐 ③**，零重编。

### 2. Alpine 的 glib 2.70.5 用 PCRE1 而非 PCRE2

未定义符号是 `pcre_compile2` / `pcre_exec` / `pcre_config`（PCRE1 API），由 `/usr/lib/libpcre.a` 提供。与 pcmanfm `OB_LIBS` 中的 `-lpcre` 一致（其注释即 `ref tmux-3`）。**不是编译不一致的隐患，是 Alpine aports 的现状。**

### 3. 静态库盘点

容器内 `/usr/lib` + `/usr/local/lib` 共 **144 个 `.a`**（之前记录的 21 项之外，pixman/png/jpeg/freetype/fontconfig/expat/harfbuzz/uuid/mount/blkid 等也齐备），`nm` 全量索引得 59,425 个导出符号。

---

## 止损判定

按实施计划的止损信号「硬门槛 > R6 消化范围（如 lgi 需要 dlopen typelib 且不能静态嵌入）→ 停止，转向 R7」：

**未触发。** 静态链接层面完全没有硬门槛，无需止损。

Spike 1 承诺的关键产出「知道真正的边界在哪」已达成：**边界不在链接期，在运行期**。真正待验证的风险转移到 Spike 2：

1. `g_module_open`（gdk-pixbuf 动态加载 image loader）—— 已静态链接 `libgmodule-2.0.a`，但 musl 静态 libc 的 `dlopen` 行为需实测
2. lgi typelib 加载（`libffi` 已在，typelib 文件运行时按路径查找）
3. Lua 静态嵌入（已完成，`/usr/local/lib/liblua.a`）

---

## 产出物

| 文件 | 内容 |
|------|------|
| `docs/sam/scripts/05-spike1-static-link.sh` | host 侧驱动（轮次计数 + docker 转发） |
| `docs/sam/scripts/05b-spike1-round.sh` | 容器内单轮：patch ninja → 链接 → 分析 |
| `docs/sam/scripts/spike1_extract.py` | 未定义符号抽取 + 静态库可解析性判定 |
| `/tmp/spike1/*.log` | 每轮完整链接日志 |
| `/tmp/spike1/*.undef` | 每轮未定义符号清单 |
| `/tmp/spike1/analysis*.txt` | 每轮符号可解析性分析 |
| `/tmp/awe-build-static/awesome` | **8.4 MB 静态可执行文件** |

---

## 备注（过程中踩的坑，已固化到脚本）

1. **必须定位 `build awesome:` 边**——`build.ninja` 里第一个 `LINK_LIBARIES` 属于 `lgi-check` 目标，盲替换会打偏。
2. **ninja 标记不能以 `@` 开头**——`@@MARK@@` 会被 `cc` 当响应文件（`error: @SPIKE1_OB_LIBS@@: No such file or directory`），改用 `# SPIKE1_OB_LIBS`（ninja 注释，不进链接命令）。
3. **正则跨行会吞边**——`re.sub(r"-Wl,--start-group[^\n]*" + mark, ...)` 里的 `\n` 若在 Python 源码写成 `\\n` 会变成「非反斜杠且非 n」，能匹配真换行，把 `build awesome:` 整行删掉。
4. **不能重复定义变量**——ninja 同边内重复 `LINK_LIBRARIES` 取后者，追加式修改会让 patch 静默失效。
5. **共享目录是 `/_ext/tmp`**——容器 `/tmp` 与 host `/tmp` 不是同一个，产出必须落 `/ _ext/tmp` 才可见。
