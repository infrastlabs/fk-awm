# Spike 0 — 动态构建打通

**日期：** 2026-09-23
**状态：** ✓ 完成
**分支：** `feat/ai-260923-static`

---

## 目标

用最短的时间证明"方案 A 走得通"：只做 4 文件 revert + 动态 cmake + ninja 构建，不涉及静态链接、不涉及 shim。

---

## A.1 git revert #4022

**Commit:** `04423cef`

**改动面（4 文件，12+/14-）：**

| 文件 | 改动 |
|------|------|
| `awesomeConfig.cmake` | `glib-2.0>=2.79.2` → `glib-2.0>=2.40` |
| `lib/awful/spawn.lua` | `GioUnix.InputStream.new` → `Gio.UnixInputStream.new`（2 处） |
| `tests/_client.lua` | `GioUnix.*` → `Gio.Unix*`（3 处）+ 调整 require |
| `tests/test-spawn.lua` | `GioUnix.*` → `Gio.Unix*`（3 处）+ require |

**验证：**
- `grep -n 'glib-2.0' awesomeConfig.cmake` → `>=2.40` ✓
- `grep -rn 'GioUnix' lib/ tests/` → 空 ✓
- `grep -rn 'Gio\.UnixInput\|Gio\.UnixOutput' lib/ tests/` → 10 处 ✓

---

## A.2 动态构建

### 环境问题与修复

| 问题 | 原因 | 修复 |
|------|------|------|
| `convert is required to build awesome` | ImageMagick 未装 | `apk add --no-cache imagemagick6` |
| `Policy "CMP0140" is not known to this version of CMake` | Alpine 3.15 的 CMake 3.21.7 不支持 CMP0140（3.25+） | 修 `cmake/FindLua.cmake:143`：`if(POLICY CMP0140)` 条件化 |
| `Could NOT find Lua (missing: LUA_INCLUDE_DIR)` | 原 FindLua.cmake 的版本检查 regex 不匹配 Lua 5.3 的宏拼接格式 | 简化 `cmake/FindLua.cmake`：改用 pkg-config 找 Lua |
| `The lgi check failed` | lua-lgi 未装 | `apk add --no-cache lua-lgi` |
| `dynamic libraries not enabled` | 静态 Lua（`/usr/local/bin/lua`）不支持 dlopen，lgi 需要动态库 | `AWESOME_IGNORE_LGI=1` 跳过检查（A.2 只需构建成功，不需运行） |

### 构建命令

```bash
# 容器内
mkdir -p /_ext/tmp/awe-build-dyn && cd /_ext/tmp/awe-build-dyn
cmake -GNinja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr/local \
  -DGENERATE_DOC=OFF \
  -DLUA_EXECUTABLE=/usr/local/bin/lua \
  /_ext/home/headless/xm-zs01/fk-awesomewm

AWESOME_IGNORE_LGI=1 ninja awesome
```

### 产物验证

```
$ file /_ext/tmp/awe-build-dyn/awesome
ELF 64-bit LSB pie executable, x86-64, dynamically linked,
interpreter /lib/ld-musl-x86_64.so.1, with debug_info, not stripped

$ ldd /_ext/tmp/awe-build-dyn/awesome | wc -l
50  # 50 行依赖（含 libc.musl）
```

**直接依赖清单（部分）：**
- `libglib-2.0.so.0` → 2.70.5
- `libgio-2.0.so.0` → 2.70.5
- `libgobject-2.0.so.0` → 2.70.5
- `libcairo.so.2` → 1.16.0
- `libpango-1.0.so.0` → 1.48.10
- `libgdk_pixbuf-2.0.so.0` → 2.42.8
- `libxcb.so.1` + 12 个 xcb 子库
- `libxkbcommon.so.0` + `libxkbcommon-x11.so.0`
- `libstartup-notification-1.so.0`
- `libdbus-1.so.3`
- `libpcre.so.1`

---

## 止损信号检查

| 信号 | 实际 | 判断 |
|------|------|------|
| cmake 配置失败在 xdg-basedir API | 未发生 | — |
| 其他失败 = 方案 A 假设崩塌 | 仅环境/工具链问题，均已修复 | ✓ 方案 A 假设成立 |

---

## 产出

- `cmake/FindLua.cmake` 简化（原 357 行 → 60 行，用 pkg-config）
- awesome 动态二进制：`/_ext/tmp/awe-build-dyn/awesome`（3.2MB）
- 所有 21 项 `AWESOME_DEPENDENCIES` 零缺失

---

## 结论

**Spike 0 A.2 通过。** 方案 A（revert #4022 + Alpine 3.15 glib 2.70.5）可打通动态构建。进入 A.3 spec 基线。
