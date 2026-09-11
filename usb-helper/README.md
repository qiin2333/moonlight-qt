# moonlight-usbd

macOS 客户端的本地 USB/IP 导出 helper，是 Remote USB 反向隧道
（`docs/remote-usb-reverse-tunnel.md`）在 macOS 平台的本地服务器。基于
[usbipdcpp](https://github.com/yunsmall/usbipdcpp)（LGPLv3，静态链接）+
[libusb](https://github.com/libusb/libusb)（LGPL-2.1+）。许可组合说明见
`docs/remote-usb-reverse-tunnel.md`：LGPL-3.0 可单向升入 GPL-3.0，与
moonlight-qt 的 GPL-3.0 兼容；发布需保留 usbipdcpp 署名（About 页已有）。

只在 macOS 上构建（C++23 + CMake ≥ 3.24 + Xcode 16 起；`pkg-config` 需在
PATH —— GitHub macOS runner 自带，本机开发 `brew install pkg-config`）。
qmake 主工程不感知本目录；`scripts/generate-dmg.sh` 会在打包时构建并拷入
`Moonlight.app/Contents/MacOS/`。

## 依赖

`third_party/` 下 4 个 git submodule（`.gitmodules` 锁定 tag）：

| submodule   | pin          | 许可        | 用途                          |
|-------------|--------------|-------------|-------------------------------|
| usbipdcpp   | v1.0.9       | LGPL-3.0    | USB/IP 服务器（LibusbServer） |
| libusb      | v1.0.29      | LGPL-2.1+   | USB 设备访问（静态）           |
| asio        | asio-1-36-0  | BSL-1.0     | 异步 IO（header-only）         |
| spdlog      | v1.15.3      | MIT         | 日志（header-only）            |

asio/spdlog 官方 config 包只在安装后存在，submodule 场景用
`cmake/packages/` 下的自写极简 config 包指过去；libusb 需要 pkg-config，
`cmake/libusb-1.0.pc.in` 生成的 .pc 会注入 `PKG_CONFIG_PATH`。

## 构建

```sh
git submodule update --init --recursive -- usb-helper/third_party
cmake -S usb-helper -B usb-helper/build -DCMAKE_BUILD_TYPE=Release
cmake --build usb-helper/build
```

开发调试时主程序按 `MOONLIGHT_USB_HELPER` 环境变量指定的路径拉起 helper，
不依赖 bundle。

## CLI

```
moonlight-usbd --version
moonlight-usbd list --json
moonlight-usbd serve --bind <busid> [--bind <busid>...] --listen <host:port>
```

- `list --json`：一次性枚举（stdout 一行 JSON 数组）。每项含 `busId`
  （libusb 拓扑路径，`1-2` / 经 hub `1-2.3`）、`vid`/`pid`/`vidPid`、
  `serial`/`manufacturer`/`product`（字符串描述符，读不到为空）、
  `claimable`（占用探测：逐接口 claim/release，被 macOS 系统驱动持有的
  HID/存储/摄像头为 false）。
- `serve`：stdout 先打 `READY <port>\n`（`--listen` 端口传 0 由系统分配），
  失败打 `ERROR {json}\n` 后以非零码退出（`device_not_found` / `device_occupied` /
  `bind_failed` / `listen_failed`）。READY 之后 stdout 永不再写（日志全在
  stderr）。父进程关闭 stdin 或发 SIGTERM 优雅退出。
- busid 生成算法与 usbipdcpp `get_device_busid` 字节级一致（`find_by_busid`
  字符串匹配反查设备），改动任一侧都要同步另一侧。
