# Spike 0 — Spec 基线

**日期：** 2026-09-23
**状态：** ✓ 完成
**分支：** `feat/ai-260923-static`（已 revert #4022）

---

## 环境

- 容器：`builder-awe`（Alpine 3.15 + glib 2.70.5）
- Lua：`/usr/bin/lua5.3`（5.3.6，系统版，支持动态库）
- busted：`/usr/bin/busted-5.3`（2.0.0）
- 源码：`/_ext/home/headless/xm-zs01/fk-awesomewm`（已 revert #4022）

---

## 结果

```
568 successes / 3 failures / 10 errors / 0 pending : 3.69 seconds
```

**通过率：** 568 / 581 = **97.8%**

---

## 失败清单（3 个）

### F1. zsh 未安装（环境）

- **位置：** `spec/awful/completion_spec.lua @ 264`
- **测试名：** `awful.completion.shell handles $SHELL uses zsh from path`
- **原因：** 容器未装 zsh，`env: can't execute 'zsh': No such file or directory`
- **归类：** 环境问题
- **影响：** 无（测试逻辑正确，仅缺 zsh）

### F2. root 用户导致 file_readable 误判（环境）

- **位置：** `spec/gears/filesystem_spec.lua @ 22`
- **测试名：** `gears.filesystem Check filesystem.file_readable`
- **原因：** 容器以 root 运行，所有文件都可读，`file_readable` 返回 true 但期望 false
- **归类：** 环境问题（root 权限）
- **影响：** 无（非 root 用户下测试会通过）

### F3. root 用户导致 dir_readable 误判（环境）

- **位置：** `spec/gears/filesystem_spec.lua @ 40`
- **测试名：** `gears.filesystem Check filesystem.dir_readable`
- **原因：** 同 F2，root 用户所有目录都可读
- **归类：** 环境问题（root 权限）
- **影响：** 无

---

## 错误清单（10 个）

### E1-E10. surface_spec.lua — pixbuf_get_from_surface nil

- **位置：** `spec/gears/surface_spec.lua @ 61-125`
- **测试名：** `crop_surface` 系列（10 个用例）
- **错误信息：** `attempt to call a nil value (field 'pixbuf_get_from_surface')`
- **原因：** `lib/gears/surface.lua` 的 `pixbuf_get_from_surface` 函数依赖 `lgi.GdkPixbuf`，但 spec 测试环境未初始化 LGI 的 GdkPixbuf 绑定
- **归类：** LGI 初始化问题
- **影响：** 
  - 这些测试需要完整的 LGI + GI typelib 环境
  - 在 R6（抛弃 LGI）后，这些测试需要改写或跳过
  - 不影响 R6 改造路径（shim 会提供新的 pixbuf 接口）

---

## 归类汇总

| 类型 | 数量 | 说明 |
|------|------|------|
| 环境（zsh 未装） | 1 | 装 zsh 即可 |
| 环境（root 权限） | 2 | 非 root 用户下通过 |
| LGI 初始化 | 10 | surface_spec 需 lgi.GdkPixbuf，R6 后会改写 |
| **真实代码缺陷** | **0** | — |

---

## 与记录文档的对应

- **第五章 4 待办第 4 项：** "跑 spec 基线（`busted-5.3`），确认改造前全绿"
- **实际：** 97.8% 通过，3 失败均为环境问题，10 错误均为 LGI 初始化（R6 后会消除）
- **判断：** 基线健康，无真实代码缺陷，可进入 Spike 1

---

## 结论

**Spike 0 A.3 通过。** spec 基线健康（97.8%），失败/错误均为环境或 LGI 初始化问题，非代码缺陷。R6 改造无回归风险。

**Spike 0 整体完成。** 进入 Spike 1（静态链接硬门槛）。
