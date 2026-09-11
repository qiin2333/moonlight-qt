# Remote USB 反向隧道

Moonlight 把客户端本机 USB/IP 服务器通过 TLS 反向转发给 Sunshine；Sunshine 启动 usbip-win2，将设备导入游戏主机。两端隧道只转发字节，不解析 USB/IP，不运行 RUSB broker、framing、Rust core 或独立 usb-agent。

```
USB 设备 → usbipd-win → Moonlight Tunnel → TLS → Sunshine reverse_tunnel_service → usbip-win2 → Windows 设备
```

## 当前支持范围

- Windows 客户端：`UsbForwardingBackend` 管理 usbipd-win 的设备共享与枚举。
- Windows 主机：`usbip_host_controller` 调用 usbip-win2。
- macOS 客户端：捆绑 `moonlight-usbd` helper（usbipdcpp v1.0.9 + libusb v1.0.29，`usb-helper/`），详见下文「macOS 客户端」。
- Android 客户端在 moonlight-vplus 工程中集成独立的 USB/IP 导出后端，使用同一能力接口和隧道协议；本 Qt 工程不提供 Linux 客户端 USB 后端。
- Linux 主机 controller 当前返回 unsupported。

## macOS 客户端（moonlight-usbd）

macOS 没有常驻 USB/IP 服务，客户端在 app bundle 内自带 `moonlight-usbd`
（`Contents/MacOS/moonlight-usbd`，源码 `usb-helper/`，CMake 构建，与 qmake 主
工程隔离；开发时可用 `MOONLIGHT_USB_HELPER` 环境变量指定路径）。基于
usbipdcpp（LGPL-3.0）的 `LibusbServer` + vendored libusb v1.0.29（LGPL-2.1+，
直接编译源文件），全部静态链接。许可组合：LGPL-3.0 条款允许单升入 GPL-3.0，
与 moonlight-qt 的 GPL-3.0 兼容；usbipdcpp README 要求显著署名（About 页法律
卡已列出）。

行为与 Windows 后端的差异：

- 设备枚举：`moonlight-usbd list --json` 一次性输出（busId/vid/pid/vidPid/
  serial/manufacturer/product/claimable），`UsbForwardingBackend::refresh()`
  的 macOS 分支解析（`parseHelperDevices`，tests/usb_forwarding_backend_list
  覆盖）。busId 是 libusb 拓扑路径（`1-2`，经 hub `1-2.3`），与 usbipdcpp
  `find_by_busid` 的生成算法保持字节级一致。
- 绑定持久化在 Moonlight 偏好（`usbforwardingbound`，busid 列表）；Windows
  上这一状态由 usbipd 自己的注册表承担。bind/unbind 无提权。注意 busid 是
  拓扑地址：换 USB 口重插会失配，绑定显示为消失（v1 已知限制）。
- 本地服务器：转发时由 Session 经 `UsbForwardingLocalServer` 拉起
  `moonlight-usbd serve --bind <busid> --listen 127.0.0.1:0`，读 stdout 的
  `READY <port>` 行把隧道 `TunnelConfig.localPort` 指到该临时端口；关闭 stdin
  或 SIGTERM 优雅退出。隧道本体/能力接口/证书校验与 Windows 完全一致。
- 平台限制（本质，无法在普通权限下绕过）：macOS 系统驱动（HID 手柄/键盘、
  存储、摄像头）持有的接口 libusb 无法 claim。helper 逐设备做占用探测
  （claim/release 探测），被占用的设备在列表中标注「In use by macOS」且不可
  共享。让这类设备可转发需要 root（libusb 1.0.27+ 的 Darwin detach），预留给
  未来的特权 helper（SMAppService），不在当前范围。

## 鉴权与配置

Moonlight 使用配对时保存的客户端证书/私钥，并验证 Sunshine 的证书与配对 pin 完全一致。通过 IP 连接时不要求证书 CN 等于 IP；任何不同的证书仍被拒绝。Sunshine 要求客户端出示已配对证书，并校验 JSON 中的共享 token。

Qt 和 Android 统一通过已配对、固定主机证书的 HTTPS 连接请求
`GET /api/v1/usb-forwarding`，不再读取端口/token 环境变量。接口版本为 1，
响应上限 4096 字节，包含 `enabled`、`available`、`reason`；只有可用时才返回
`port`（1–65535 的整数）和 `token`（64 位十六进制字符串）。
`available` 表示主机隧道服务可接入，不代表驱动已正常导入设备；设备可用性须在实际导入后验证。
能力请求不跟随重定向，5 秒内结束；即使证书受系统 CA 信任，也必须与配对证书完全匹配。

Sunshine 默认关闭转发；在 Web 设置的输入页启用 `usb_forwarding_enabled`，
保存并重启后生效。`usb_forwarding_port` 默认 0（自动使用主端口 +7，通常为 47996）；
显式填写 1024–65535 可覆盖，客户端始终使用能力接口公布的实际端口。主机必须安装 usbip-win2
及匹配驱动，跨网络使用还需允许/转发对应 TCP 端口，不自动配置路由器。

Qt 用户先在设置中启用 USB 转发，并通过设备管理将要使用的外设共享（绑定）；
开始串流后，在 USB 菜单中明确选择设备才获取凭据、启动隧道。
关闭共享或结束串流会释放导入；取消期间未完成的凭据请求不能重新启动共享。
主机重启后再次选择设备会重新获取凭据。

token 仅保存在内存中，按主机进程生命周期生成和轮换，不是逐串流令牌。
接口要求客户端仍在配对列表中，返回 `Cache-Control: no-store`；实际隧道再次校验
配对证书和 token。不要把 token、私钥或用户配对状态写入日志或提交到仓库。
本功能信任获准转发的已配对客户端，不承诺任意 USB 类别均可安全或可靠使用。

## 建立连接

Windows 客户端在设置页以及每次连接前，通过只读 SCM 查询同时检查 `usbipd`
服务和 `VBoxUSBMon` 驱动；查询失败或任一未运行时不启动隧道。该检查只是必要条件，
不能证明实际 import 成功。服务运行、设备已共享、TLS ready、设备导入成功是不同状态。

### 2026-09-09 本机复测（未完成验收）

正常 PIN 配对、重启 Qt 后配对保留、运行时能力获取及 K380 导入均已实测；
主机记录 hub port 1，11 个相关 PnP 节点正常。实际按键、停止回收、重连和退出清理
尚未在这轮正式配置流程中完成验收；悬浮菜单还存在待定位的关闭/交互问题。

测试环境曾缺少 `usbipd` 对 `VBoxUSBMon` 的服务依赖，导致驱动不启动及
`CreateFile` 错误；随后出现原生 attach 超时、主机退出超时和服务启动文件占用。
重启系统、补回依赖并启动驱动后导入成功。这些环境操作不由客户端自动执行，
也不能据此宣称此前所有超时根因已解决。客户端只增加只读预检和准确的失败提示。

### 连接步骤

1. Moonlight 连接本机 USB/IP 服务器（默认 `127.0.0.1:3240`），并连接 Sunshine TLS 端口。
2. 验证配对证书后发送一行 JSON：
   `{"op":"forward","token":"<token>","busid":"1-2"}\n`。
3. Sunshine 验证证书、token、busid 并占用设备槽，然后监听临时 loopback 端口。
4. Sunshine 先挂起异步 accept，再启动 `usbip --tcp-port <port> attach --remote 127.0.0.1 --bus-id <busid> --once --terse`。
5. helper 连接后，Sunshine 返回 `{"op":"ready"}\n` 并立即开始双向转发。**ready 表示字节隧道就绪，不表示设备已经完成导入。** usbip-win2 必须先通过这条隧道完成 USB/IP import 才能报告 attach 成功。
6. helper 返回 hub port 后，Sunshine 记录本次绑定并取消启动超时。

ready 之前的拒绝用一行 `{"op":"error","reason":"..."}` 返回。ready 之后所有字节都属于 USB/IP，attach 失败只能关闭连接，不能插入 JSON。

## 生命周期与资源限制

- Sunshine 端每个 busid 一条活动隧道，重复请求被拒绝；Moonlight 客户端当前限制为全局一条活动隧道。
- 客户端启动超时 15 秒（须覆盖主机侧完整窗口），Sunshine 启动超时 12 秒；主机 attach 仍受 controller 超时约束。
- JSON 握手行限制 4 KiB；握手后的剩余字节必须继续转发。
- 客户端采用 4 MiB 读取缓冲/写队列高水位，主机每方向按 64 KiB 异步读写，依赖 TCP 背压。
- 用户释放、串流结束或任一 socket 断开会关闭本端两条连接；主机取消未完成 attach，并 detach 已接受的绑定。
- controller 生成本地 `binding_id` 区分先后两次 attach，防止临时端口与 hub port 复用后旧 detach 误拆新设备。该编号不在隧道协议上传输，也不依赖客户端 RUSB token。
- Sunshine 用 Asio 的证书验证回调 API，不能覆盖 Asio 所拥有的 `SSL_CTX` app_data。

## 代码与验证入口

- 客户端：`app/backend/usbforwardingtunnel.{h,cpp}`；`Session` 管理 UI 与串流生命周期。
- 主机：[Sunshine PR #1034](https://github.com/AlkaidLab/foundation-sunshine/pull/1034)。
- 客户端：[Moonlight Qt PR #209](https://github.com/qiin2333/moonlight-qt/pull/209)。
- `tests/usb_forwarding_tunnel/usb_forwarding_tunnel.pro` 构建无视频会话的测试驱动，直接使用正式 `Tunnel` 类。
- Sunshine 的 `reverse_tunnel_probe` 和 `tests/tools/test_reverse_tunnel.py` 覆盖 TLS/token 拒绝、先转发后完成 attach、断开重连以及双端隧道对拍。合成 helper 测试不等同于真实 USB 设备 E2E。
- 主机原有 `loopback_usbip_bridge` 仍服务虚拟触摸屏 POC，不参与这条反向隧道。

## Windows 实机验证（2026-09-06）

Windows 本机通过 usbipd-win 5.3.0 导出真实 Android 手机，正式 Qt `Tunnel` 经 SSH 端口转发连接 Win10 Hyper-V 虚拟机中的正式 Sunshine `reverse_tunnel_service`，由 usbip-win2 0.9.7.8 导入设备。

- 无需手机点击授权：使用 WinUSB 标准控制请求，读取并校验设备 VID/PID 与序列号，每轮完成 20 次 `GET_STATUS`。
- 两轮“导入 → 控制传输 → 释放”通过，第二轮可复用 hub port 1；每轮结束后导入端口为空，最终手机恢复本机 ADB 可用。
- 初次测试与参数化脚本复跑均通过两轮。Sunshine 的 `tests/tools/run_usb_control_vm_e2e.py` 和 `usb_control_probe.cpp` 提供复现入口，详见其 `tests/tools/README-remote-usb.md`。
- 此结果验证真实 USB 控制传输和断开重连。VM 中 ADB 仍需手机授权，未验证 ADB shell、持续 bulk/isochronous 吞吐、其他设备类别或完整视频串流/UI 生命周期。

### 与实际视频串流联合验证

随后使用完整 Moonlight 和虚拟机中的完整 Sunshine 连续完成两轮会话：1024×768 H.264 桌面画面可见，实际串流 USB 菜单导入手机，每轮校验序列号并执行 20 次 WinUSB `GET_STATUS`，然后直接退出串流。两次退出均自动清空导入端口，重开串流后可再次导入，最终手机恢复本机 ADB 可用。客户端退出统计的接收/解码/呈现帧率分别为 30.0/30.0/30.0 和 30.1/30.1/30.0 FPS；观察到的网络丢帧为 0%。这两轮未使用独立隧道 probe，USB 与视频直接连接同一 VM。

通过配置为登录会话内 WGC 采集、Sunshine 软件编码与 Moonlight 软件解码。测试环境硬件解码报 hwframes context 初始化失败（-22）；VM 没有音频端点，因此硬件解码、音频、手柄和 USB bulk/isochronous 吞吐不计入此次通过范围。

测试部署需使用与 VM 驱动匹配的 usbip-win2 0.9.7.8 工具及配套 DLL。初次 CLI 配对在主机登记成功但客户端 pin 为空，成功测试前通过已认证 SSH 取得主机证书并固定到该测试主机；全新配对的 pin 持久化仍待单独验证。具体步骤与证据说明见 Sunshine 的 `tests/tools/README-remote-usb.md`。
