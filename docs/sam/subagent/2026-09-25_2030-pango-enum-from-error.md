# 子任务回执 · Pango 枚举回落改报错（2026-09-25 20:30 +08）

> **性质：** 并行子任务返回的完整回执原文，未经裁剪。
> **任务：** 审计 P2 第 13 项。`lgishim_pango.c` 的 `enum_from()` 未命中就静默
> 回落默认值，配置拼错会静默产生错版式。改成「没传参 → 用默认；传了但拼错 →
> `luaL_error`」，与同仓库 `lgishim_glib.c:172 enum_arg()` 已有的报错语义对齐。
> **产物：** `lib/vendor/lgishim_pango.c`，`enum_from()` 内一个 hunk。
> **模型：** 子任务用主任务模型（`model_tier=strong`）。

---

改完了。

## 改动
仅 `lib/vendor/lgishim_pango.c`，`enum_from()` 内（`:217-226`，diff 只有这一个 hunk）：

```c
    const char *s = luaL_optstring(L, idx, NULL);
    size_t i;
    if (s) {
        for (i = 0; i < n; i++)
            if (!strcmp(s, t[i].name))
                return t[i].value;
        /* A missing argument is the documented "use default" path; a supplied
         * name that matches nothing is a typo and must not fall back to defv. */
        return luaL_error(L, "lgishim.Pango: unknown %s value", s);
    }
    return defv;
```

- `luaL_optstring(L, idx, NULL)` 原样保留 → `s == NULL`（没传参数）走 `return defv`，绝不报错。
- `s != NULL` 且遍历完未命中 → `luaL_error`。错误信息照抄 glib 的 `enum_arg()` 风格，仅换前缀：`lgishim.Pango: unknown %s value`（与 `lgishim.GLib: unknown %s value` 一致，注意 glib 那条只有一个 `%s`，是原样镜像而非我补了字段名）。
- 风格：8 空格 tab、C89 声明前置（`const char *s` / `size_t i` 仍在块首）、注释沿用 glib 块注释语气，未改其他函数。

## 单 TU 编译
原命令缺 `-I/usr/lib/glib-2.0/include`（`glibconfig.h` 找不到），用 `pkg-config --cflags glib-2.0 pango` 补齐后：

```
cc -c -Wall -Wextra -o /tmp/lgishim_pango.o lib/vendor/lgishim_pango.c -I. ... → EXIT=0
```

**零告警零错误**（`-Wall -Wextra` 下该 TU 连既有告警都没有），产物 33472 字节。未链接，未跑 ninja。

## 3 个调用点默认值语义（`git diff` 确认调用点零改动）
| 行 | 函数 | 默认值 | 无参时 |
|---|---|---|---|
| `:338` | `l_set_wrap` → `WrAP` | `PANGO_WRAP_WORD_CHAR` | 不变，返回默认 |
| `:353` | `l_set_ellipsize` → `ELLIPSIZE` | `PANGO_ELLIPSIZE_END` | 不变，返回默认 |
| `:380` | `l_set_alignment` → `ALIGN` | `PANGO_ALIGN_LEFT` | 不变，返回默认 |

三处都是「省略参数 = 用默认」的路径，行为完全不变；只有显式传入拼错的名字（如 `"LEFTT"`）从静默回落变成 Lua 层 error。

未 commit/push/add。工作区里另有 `spec/gears/filesystem_tests/x/NoRead` 修改和 `main.c` untracked，是并行子任务留下的，我没碰。

---

## 主任务复核（追加，非子任务原文）

- 调用点数纠正：子任务按 `grep -c enum_from` 算 4，含定义行本身，**实际调用点 3 处**。
  record3 §4.1.2 原稿误写「4 处」，已改 3 处。
- `strings <binary> | grep -c "lgishim.Pango: unknown"` = 1，确认进了二进制。
- 全套 spec 回归无新增失败（见本章「主任务验证」）。
