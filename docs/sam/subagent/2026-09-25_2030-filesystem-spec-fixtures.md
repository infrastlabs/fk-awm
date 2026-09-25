# 子任务回执 · filesystem_spec 夹具副作用修复（2026-09-25 20:30 +08）

> **性质：** 并行子任务返回的完整回执原文，未经裁剪。
> **任务：** 审计 P2 第 15 项。`spec/gears/filesystem_spec.lua` 两处问题：
> (A) 权限恢复写在 `it()` 体内，断言失败就跳过；且 `a-r`/`a+r` 非对称，
> 恢复后落成 666 而非 644。(B) `:26`/`:44` 两个断言在「跑测者即夹具属主」时
> 必然失败（`chmod a-r` 后 600，属主仍有 rw）。
> **产物：** `spec/gears/filesystem_spec.lua`（+49/−6），夹具文件零改动。
> **模型：** 子任务用主任务模型（`model_tier=strong`）。

---

## 完成

只改了 `spec/gears/filesystem_spec.lua`（+49/−6），夹具零改动，无 git 操作。

### 改动内容

**`:11-12` 局部变量** — 提出 `no_read_file` / `no_read_dir`，消除 4 处路径字符串重复。

**`:20-35` `untestable_without_others(path)`** — 返回原因字符串或 `nil`。

**`:41-44` `after_each`** — 显式模式恢复：`chmod 644` 文件、`chmod 755` 目录（不是对称的 `a+r`）。放在 `describe` 顶部，作用于全部 15 个 `it`。

**`:62-65`、`:84-87` 条件 skip** — 严格模式 `chmod g-r,o-r,a-r` 保留（前置条件）；`it()` 体内两行恢复代码已删。

### ⚠️ 两处与任务描述不符，需你知晓

**1. 这个 busted 没有 `skip()`** — `skip` 在 `it` 体内是 `nil`（已实测 `type(skip)=nil`）。用的是 **`pending(reason)`**，它抛 `pendingMt` 错误、终止 `it` 余下语句，TAP 输出为 `ok N - # SKIP <reason>`。仓库已有先例 `spec/gears/color_spec.lua:255`。

**2. 本容器跑测者不是属主，是 root** — `id` 显示 uid=0，夹具属主 uid=1000。真实原因是 **root 绕过 DAC**，`chmod a-r` 降到 600 后 root 照样可读（实测确认）。所以我只检查「跑测者是属主」在这容器里永不触发、测试仍会红。守卫加了两个分支：先 root（`test "$(id -u)" -eq 0`），再属主（`test "$(id -u)" -eq "$(stat -c %u …)"`），原因文案各自不同。

`$EUID`/`$UID` 在 `os.execute` 的子 shell 里未设置，所以用 `id -u`。另注意 Lua 5.3 的 `os.execute` 失败时第一个返回值是 `nil` 而非 `false`，故用 `local ok = …; if ok then` 形式。

### 验证结果（`busted-5.3 --output=TAP`，`SOURCE_DIRECTORY` 已设置）

```
1..15   →  passed 13 | skip 2 | failure 0
ok 3 - # SKIP …:66: root ignores mode bits, assertion untestable without a non-root runner
ok 5 - # SKIP …:88: root ignores mode bits, assertion untestable without a non-root runner
```

夹具权限跑完后：`NoRead` = **644**，`filesystem_tests/y` = **755**（跑前是 666 / 333）。连跑两次稳定。

### 额外做了跨身份矩阵验证

用 `setpriv --reuid` 实测三分支：

| 跑测者 | 结果 |
|---|---|
| root（本容器默认） | 2 skip，文案 "root ignores mode bits" |
| uid 1000（夹具属主） | 2 skip，文案 "owner keeps rw after chmod a-r…" |
| uid 12345（非属主非 root） | **无法 chmod 他人文件** → `Operation not permitted`，测试失败 |

第三行说明：真正能跑通这断言的跑测者不存在——非属主无权收紧权限，属主/root 又始终可读。断言未被改成无条件通过，属主分支是真正的 skip。顺带证明 `after_each` 在断言抛错后确实执行（uid 12345 全失败场景下权限仍被还原）。

已记录 gotchas 到 memory（`busted-gotchas.md`）。

**未提交**；`git status` 里的 `lib/vendor/lgishim*.c`、`main.c` 是并行子任务的改动，与本次无关。

---

## 主任务复核（追加，非子任务原文）

- **纠正 record3 的根因判断**：我在派发时说「跑 spec 的用户 `headless` 就是夹具属主」，
  是错的。宿主机 `id -un` 是 `headless`，但 busted 在容器里跑、容器里是 **root**（uid=0），
  夹具属主是 uid=1000。真实机制是 **root 绕过 DAC**，不是「属主仍有 rw」。
  两种说法都会导致断言失败，但机制不同，record3 §4.1.4 已改。
- **跨身份矩阵是本轮最有价值的产出**：它证明了这三个断言在本仓库**结构性不可测**
  （非属主无权收紧权限，属主/root 始终可读），所以 skip 是正确的最终形态，
  不是「等配好 non-root 用户就能跑」。
- `busted 2.0.0-0` 无 `skip()`、`os.execute` 失败返回 `nil` 而非 `false` —— 两个非显然坑，
  已进 memory。
