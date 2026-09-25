

### 0925 14:50验跑1|genM.hdmi-52089下跑

- 程序包 `awesome-static-musl-4.3-1850-bcadd60-260925-125003.tar.gz` /_ext/tmp/awe-package/$pkg@tenvm2; 已fix:89M/8s 狂刷log爆容器overlay盘的问题`ref docs/sam/2026-09-25-record3.md`; 

```bash
# test01
  1 ./awesome -r 启动日志，1条ERR 见run01.log
  2 tray托盘没得图标 (新起flameshot/asbrucm,确定没得tray图标:无新位置)
  3 desk右键菜单，没得图标
  4 deskBG无，全黑

# test02
  1 换ws/右键tag多工作区ok;
  2 alt+t/f/e可开term/thunar/geany: obox/xfwm在跑??

```

- 程序包 `awesome-static-musl-4.3-1880-d1fcc59-260925-191632.tar.gz`; record3.md--四、并行批次产出：Part 12 T2c + Spec Shim化--2.1、🔴 2P0修正后==>trayIcon问题也即解决;

```bash
# test03
  1 ./awesome -r 启动日志，1条新ERR 见run02.log
  2 tray托盘没得图标==>有了(有占位+有图标) #OK
  3 desk右键菜单，没得图标; 默认menu本身就没?
  4 deskBG无，全黑

```

- 程序包 `awm-static-musl-4.3-1910-5247a66-260925-204338.tar.gz`; 改awm名字
  - 壁纸OK (shim.gdk_pixbug带上XPM/PNG解码)
  - 右键菜单:菜单项比较少:与dyn版对不上,另只有Pstree有图标

### 0927周日 24:40验跑2|27号上午commit整理变基后,下午:调试右键菜单

- 程序包 `awm-static-musl-4.3-1812-7dd9243-260925-204338.tar.gz`
  - 桌面邮件菜单图标svg OK (librsvg+shim)

```bash

# dbus
export DBUS_SYSTEM_BUS_ADDRESS=/var/run/dbus/system_bus_socket
export DBUS_SYSTEM_BUS_ADDRESS=unix:path=/var/run/dbus/system_bus_socket
# headless@hdmi-desktop:/usr/local/static/awm/bin$ ./awesome -r
  2026-09-28 00:39:05 W: awesome: a_dbus_connect:618: Could not connect to D-Bus system bus: Failed to connect to socket /usr/local/var/run/dbus/system_bus_socket: No such file or directory
  2026-09-28 00:39:06 W: awesome: `awful.layout.layouts` was set before `request::default_layouts` could be called. Please use `awful.layout.append_default_layouts` to  avoid this problem

```