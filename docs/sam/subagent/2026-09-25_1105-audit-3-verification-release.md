# 审计报告三 · 验证与发布收口盘点（2026-09-25）

> **性质：** 独立只读调查的原始报告，正文未经裁剪。
> **范围：** 「验证与发布」这块到底收没收口 —— 还没跑的验证和流水线断点。
> **写于：** HEAD `bf6ee42c`（工作区 clean）。此后主任务又提交了 `d37c1bec`（纯文档），
> 故下文「落后 11 个 commit」在当前 HEAD 上应为 12。
>
> 由主任务归纳进 `2026-09-25-record3.md` 篇五。本报告保留完整原文。

---

## 一句话结论

**当前唯一的 tar.gz 不可用**（sha 标的是 `4c31690`，比 HEAD 落后 11 个 commit，其中 5 个是功能提交，缺 pango UAF 修复、rc.lua 启动修复、Rsvg 守卫）。**prefix 里装的二进制是对的、但没 UPX、也从来没被打包过。** 出包流水线没收口。

---

## 一、产物实际状态（宿主机直读）

| 位置 | 内容 | 关键事实 |
|---|---|---|
| `/_ext/tmp/awe-package/*.tar.gz` | **只有 1 个**：`awesome-static-musl-4.3-4c31690-20260924-160352.tar.gz`，4,389,035 B，sha256 `389c28a0…`，346 条目 | 二进制 3,402,220 B = 已 UPX（`-9 --lzma`，9,597,256 → 64.6%↓）。**sha 是 `4c31690`，不是 `1b03351`** |
| `1b03351-…log` / `.smoke.log` | 只有 step 6 日志，02:57 | **`1b03351` 从没跑到 step 7，没有对应 tar.gz** |
| `59b4cf7-…log` / `.smoke.log` | 02:13，**T2 FATAL rc=1** | 刷屏 UAF 暴露后、rc.lua 修之前的失败现场（89,502,335 B 冒烟日志） |
| 容器 prefix `…/bin/awesome` | 9,597,256 B，md5 `e4bb95fd…`，`upx -t` → **Tested 0 files（未压缩）**，`v4.3-1841-g1b033515-dirty` | 装于 02:49。**含 fb67b582 的全部 C 改动**（worktree dirty 时编的）；`share/doc/awesome/README.txt` **不存在**（step 7 从没跑） |
| prefix `textclock.lua` | **已修**（`w._private.refresh`）；`rc.lua` md5 `d67701c7` == tarball 里的 | rc.lua 文件本身从 `4c31690` 起就没变过——「rc.lua 修好了」改的是 C shim + textclock.lua，不是 rc.lua |
| `awe-build-release/awesome` | 9,597,256 B，md5 `e4bb95fd…`，02:49，未压缩 | 与 prefix 完全一致 |
| `awe-build-release/awesome.pristine` | 9,597,256 B，**md5 `5a7a5463…`**，00:04 | **与当前 `awesome` 不是同一个文件**——是 `4c31690` 那轮的副本 |
| `upx.done` | `pristine=9597256` / `args=-9 --lzma size=3402220` | **陈旧**：二进制现在是 9,597,256（未压缩），tag 说 3,402,220 |

### ⚠ 最严重的流水线断点：step 4 现在跑会静默装回旧二进制

`step_upx()` 逻辑（`:273-282`）：pristine 存在 → **跳过**「pristine 缺失 + upx.done 存在则跳过」那个分支，直接拿 pristine 当输入跑 7 轮阶梯，然后 `cp -a "$BEST_CAND" "$BINARY"`。而 pristine 是 4c31690 时代的产物。

⇒ **现在直接跑 step 4，会把 4c31690 的旧二进制压缩后写回 `$BUILD_DIR/awesome`**，step 5 再把它装进 prefix、step 7 打包，全程无任何报错。`upx.done` 里那个「已完成」短路分支实际上只在 pristine 缺失时才生效——恰好是应该重跑的场景，逻辑是反的。

---

## 二、验证项 → 状态 → 要不要重跑 → 成本

| 验证项 | 当前状态 | 要重跑？ | 成本 |
|---|---|---|---|
| **step 1 configure / 2 build** | 02:49 已跑，二进制为静态（`file` 校验过）、含全部修复 | 不必（源码已 clean，产物正确） | — |
| **step 3 checkpaths** | 未重跑（只在 4c31690 轮跑过）；prefix/config.h 路径没变过 | 建议重跑，纯 grep | 快 |
| **step 4 UPX 7 轮** | **失效状态**：pristine/upx.done 都是 4c31690 的；prefix 里二进制未压缩 | **必须先删 `awesome.pristine` + `upx.done` 再跑**，否则装回旧码 | 中（round 7 上轮 180s 超时，约 5–10 分钟） |
| **step 5 install** | 已跑（02:49），prefix 有对的二进制+修复后的 lib；**无 README**（step 7 才写） | 是——要装 UPX 后的二进制 | 快 |
| **step 6 smoke T1/T2** | ✅ 已跑，rc=137、838 B | 是（装在 UPX 后产物上） | 快（10 s） |
| **step 6 T2b（错误行 grep）** | ✅ 已跑且过：`lua/运行时错误行=0`；另有 `flaky10.txt` 10× rc=137 / gio=0（02:55），`step6.txt`（02:57） | 是（跟 step 5 一起） | 快 |
| **step 7 package** | **未跑过任何一轮 1b03351 之后的出包**；唯一 tar.gz 是 4c31690 的 | **是** | 快 |
| **10-spike4-cairo-smoke（830 checks / 0 failures）** | ✅ 绿，record3 篇一记载（基线 829/0，+1） | 不必（该 shim 代码之后没再动，fb67b582 改的 `lgishim_cairo.c` 已在其中——但**严格说 fb67b582 改了 `lgishim_cairo.c`，冒烟只跑了 record3 记载的那次，之后没复跑**） | 中（重编 smoke host） |
| **Part 13 spec 回归（577/589 = 97.96%）** | ✅ 绿，**新增回归 0**；`LGI_SHIM_FLAG=false`，跑的是真 lgi，**没验证 shim** | **不必为了发版重跑**；但 fb67b582 之后从未复跑（低风险：改动是 shim + textclock） | 快（3.8 s） |
| **篇十 §6 的 32 项运行验证（30/32）** | 从未按新标准重跑 | 见下方「疑点 3」 | 中 |
| **chown 收尾** | **脚本里没有**（见隐患 §4） | 是 | — |

---

## 三、12 个 spec 未通过的具体清单

`2 failures + 10 errors`（`577 successes / 2 failures / 10 errors / 0 pending`，record2 篇十二 §3）：

| 编号 | 位置 | 类型 | 具体断言 / 报错 |
|---|---|---|---|
| F-a | `spec/gears/filesystem_spec.lua` @22 `it('Check filesystem.file_readable')` | failure | `:26` `assert.is_false(gfs.file_readable(.../x/NoRead))` |
| F-b | `spec/gears/filesystem_spec.lua` @40 `it('Check filesystem.dir_readable')` | failure | `:44` `assert.is_false(gfs.dir_readable(.../y))` |
| E-1…E-10 | `spec/gears/surface_spec.lua` @ **61, 68, 76, 84, 92, 101, 107, 113, 119, 125** | error ×10 | 同一处 `:50` `gdk.pixbuf_get_from_surface(...)` → `attempt to call a nil value (field 'pixbuf_get_from_surface')` |

**为什么不算回归**（三条独立依据，都对得上账）：
1. **基线本来就更差**：`spike0-spec-baseline.md` 是 `568/581 = 97.8%`，即 3 failures + 10 errors = **13 个**没过。本轮 12 个，**少了 1 个**（基线 F1 `completion_spec.lua:264` `env: can't execute 'zsh'`，因容器装上了 zsh 而转绿）。**新增失败 0，新增错误 0。**
2. **报错文本一字未变，位置一字未变**：F-a/F-b 与基线 F2/F3 同处同断言；E×10 与基线 E1–E10 同处同报错。
3. **spec 源码与基线逐字节一致**：`git log -- spec` 只有两个 0 字节 `fix win`（2026-09-22，改 fixture 名）+ 最后一个 spec 改动 `c5f2b4bb`（2026-08-23）。差异 100% 来自环境。

根因归类：F-a/F-b 是 **root 误判**（root 读什么都可读，负向断言必挂——篇十二 §7.2 记录了给容器配 non-root 用户即可全绿，本轮没做）；E×10 是**环境缺 `pixbuf_get_from_surface`**（shim 和 spec 环境都没有这个函数）。

**两个必须记住的前提**：
- 这篇跑的是 **`LGI_SHIM_FLAG=false` 的真 lgi 0.9.2**，验证的是 `lib/` Lua 层语义没变，**没有验证 lgishim**。shim 的验证在冒烟侧（830 项）。两半合起来才覆盖「迁移没改坏语义」。
- **spec 从未在 shim 环境下跑过**（篇十二 §7.1），fb67b582 之后也从未复跑。

---

## 四、脚本层隐患（可复现，非猜测）

1. **chown 缺失（已踩雷一次，未修）** —— `grep -n 'chown' 11-package-static-release.sh` 全文件**只有 1 处**：`:529 chown -R 1000:1000 "$OUT_DIR"`，在 **step 7 末尾**。step 5（`cmake --install`，容器内以 root 跑）**之后没有任何收尾 chown**。所以现在「只跑 step 5/6、不跑 step 7」时，root 改坏的文件没人收拾——`lib/vendor/lgishim_gio.c` 就是这样变成 root:root 444 的。
   *当前状态：已手动恢复*（`headless:headless 664`，全仓库 `find -user root` 为空，`git status --porcelain` 为空）。
2. **`SMOKE=0` 是无效开关（脚本注释撒谎）** —— 头部 `:63` 和 preflight `:206` 都说「SMOKE=0 可跳过冒烟」，但 **`step_smoke()` 里没有任何一处读 `SMOKE`**；`:394` 无条件 `command -v Xvfb || fail`。`SMOKE=0` 在 preflight 只是少报一个错，`all` 模式照样在 step 6 挂掉。要跳冒烟只能手工调 `… 7`。
3. **sha 漂移（本次实际发生）** —— `SHA=$(git rev-parse --short=7 HEAD)` 每次实时取，`TS` 却从 `$BUILD_DIR/RELEASE_TS` 读且**只写一次**（`:134-139`）。所以单跑 step 7 会得到 `awesome-static-musl-4.3-bf6ee42-20260924-160352.tar.gz`——**同一个 TS、不同 sha**，跟旧包 `4c31690-20260924-160352` 并排存在，肉眼极易拿错。
4. **README 里的 UPX 数字会被 upx.done 骗** —— `:435` `awk -F'[= ]' '/^args=/{print $2}'` 从 `args=-9 --lzma size=3402220` 取 `$2` = **`-9`**，丢掉了 `--lzma`；再配合 `:448` 用 `BEFORE → AFTER` 打印，现在跑 step 7 会得到「9597256 -> 9597256 bytes（upx -9）」——**声称压缩了，实际没压**。
5. **`:434` 是死代码** —— `[ -f "$BUILD_DIR/upx.done" ] || UPX_STATE="未压缩"` 下一行无条件覆盖 `UPX_STATE`。
6. `:316` 的 `[ "$CAND" = "$BEST_CAND" ] || rm -f "$CAND"` 是 `7ced7e76` 之后才有的清理；`awe-build-release/` 里还留着 round1–5 五个候选文件（4.3–5.0 MB 各一份），是 `4c31690` 那轮的残留。
7. 小注（低优先级）：`step6.txt`（02:57）里 bash 报的是 `line 378`，而当前脚本的 T2 timeout 在 `:401`；脚本最后改动是 `7ced7e76`（02:16），此后 git clean。说明那份输出的行号与当前脚本版本对不上，无法从文件确认它是同一版本跑出来的。

---

## 五、三个疑点的核实结论

**疑点 1 — chown：确认缺失。** 脚本里仅 `:529` 一处，在 step 7 末尾，且只 chown `$OUT_DIR`（不覆盖 `$BUILD_DIR`、`$PREFIX`、`$REPO`）。step 5 后无任何收尾。当前工作区已手动恢复干净。

**疑点 2 — sha 漂移：确认，而且比你说的更严重。**
- tarball 的 sha 实际是 **`4c31690`**，不是 `1b03351`（`1b03351` 从未出过包，只有 step 6 日志）。
- `4c31690c` 是 HEAD 的祖先，`4c31690c..HEAD` 有 **11 个 commit**，其中**功能性 5 个**：

| commit | 内容 | tarball 是否含 |
|---|---|---|
| `5e4ba79b` | `CMakeLists.txt` pkg_check_modules `_REQUIRED_INCLUDE_DIRS` | ✗ |
| `1c0f142b` | `imagebox.lua` Rsvg 空表守卫 | ✗（tarball 里仍是旧 `pcall(function() Rsvg = lgi.Rsvg end)`） |
| `59b4cf73` | `lgishim_pango.c` 借用 font map（刷屏 UAF 根因） | ✗ |
| `fb67b582` | `lgishim_cairo.c` surface_wrap lightuserdata 分支 + `lgishim_gio.c` traceback handler + `textclock.lua` interval 0 | ✗（tarball 里 `textclock.lua:160` 仍是裸 `refresh`） |
| `7ced7e76` | 出包脚本 preflight / T2b / KILLT / PREFIX guard | ✗ |

⇒ **装 tarball 会装回 pango UAF 刷屏 + rc.lua 起不来 + imagebox SVG 抛异常** 三个已知坏状态。

**影响**：`ls /_ext/tmp/awe-package/*.tar.gz` 只有一条结果，任何人拿到「最新包」都会拿到最坏的包。而 prefix 里的二进制是好的——**两者现在是矛盾的**，这是本次盘点最大的风险。

**疑点 3 — 30/32 重判：结论是「概念搞混了」，且 T2b 确实重跑过。**
- **30/32 本来就不是 rc-only。** 它出自 record2 篇十 §4/§6 的 **32 项断言运行验证**（Alpine 与 Ubuntu 22.04 各 30/32），2 条 FAIL 是**测试脚本用错 API 名**（用了 `destroy` 而非 `finish`、`.width` 字段而非 `get_width()`），不是 shim bug。它本身就是断言级的，**T2b 无从介入**。
- record3「现状 #2」和「遗留 #1」把 30/32 当成 rc-only 要按 T2b 重判——这是**文档层的概念串线**。真正需要按 T2b 重判的只是「默认 rc.lua 能启动」这条，而它**已经重跑且通过了**：
  - `/_ext/tmp/step6.txt`（02:57）：`T2 ok rc=137`、`冒烟日志 838，lua/运行时错误行=0`
  - `/_ext/tmp/flaky10.txt`（02:55）：`r1…r10 全部 rc=137 / size=530 / gio=0`
  - 另发现两条值得记的：`smoke_run2.txt`（02:48）`ERRN=1`（1 条错误行，修之前）；`dirty.txt`（02:52）脏显示环境下 `rc=1 / acquire_WM_Sn`——证明 T2b 真能抓出假通过，不是摆设。
- **但是：32 项断言那套 harness 本身，从 fb67b582 之后从未重跑过。** 如果它跟 rc.lua 路径有交叉，现在没有证据。

---

## 六、要让发布产物可用，按顺序跑这 5 步

```bash
# 0. 必须先清掉这两个陈旧标记，否则 step 4 会装回 4c31690 的旧二进制
rm -f /_ext/tmp/awe-build-release/awesome.pristine /_ext/tmp/awe-build-release/upx.done
rm -f /_ext/tmp/awe-build-release/awesome.upx-round*   # 顺手清 4.3-5.0MB×5 残留

# 1. 建议同时清 RELEASE_TS，否则新包名与旧包共用 TS，极易拿错
rm -f /_ext/tmp/awe-build-release/RELEASE_TS

# 2. 建议先删旧包（或移到 archive/），别让它和新包并排
mv /_ext/tmp/awe-package/*4c31690* /tmp/  (或改名留档)

# 3. 顺序跑：3 → 4 → 5 → 6 → 7
bash docs/sam/scripts/11-package-static-release.sh 3   # checkpaths（必须早于 UPX）
bash docs/sam/scripts/11-package-static-release.sh 4   # UPX 7 轮
bash docs/sam/scripts/11-package-static-release.sh 5   # install（UPX 后）
bash docs/sam/scripts/11-package-static-release.sh 6   # smoke + T2b
bash docs/sam/scripts/11-package-static-release.sh 7   # package
```

不需要重跑 step 1/2：当前 `awesome` 二进制 md5 `e4bb95fd` 与 prefix 一致、是静态的、且含 fb67b582 全部 C 改动（worktree 在 `1b033515-dirty` 时编的）。**step 1/2 重跑也可以，只是没必要。**

预计耗时：step 4 约 5–10 分钟（round 7 上轮 180 s 超时属正常，脚本会跳过），其余全部秒级。

**另外三件值得单独排期的事**（不在上面这条链上）：
1. `10-spike4-cairo-smoke.sh` 复跑一次——`fb67b582` 动了 `lgishim_cairo.c`，而当前 830/0 是修之前那次 run 的数字。
2. 补 `filesystem_spec` 的 non-root 运行（消掉 F-a/F-b 两个 root 误判），注意 **`filesystem_spec.lua` 失败带副作用**（会把 `spec/gears/filesystem_tests/y/` 留成 702、`x/NoRead` 留成 666），CI 里要么跳过要么用 non-root。
3. 脚本层面：step 5 末尾加 chown、`step_smoke()` 读 `SMOKE`、step 7 的 UPX 参数解析带全 `--lzma`、sha 与 TS 绑定校验。
