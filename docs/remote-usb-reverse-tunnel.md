# Remote USB 反向隧道

Moonlight 把客户端本机 USB/IP 服务器通过 TLS 反向转发给 Sunshine；Sunshine 启动 usbip-win2，将设备导入游戏主机。两端隧道只转发字节，不解析 USB/IP，不运行 RUSB broker、framing、Rust core 或独立 usb-agent。

```
USB 设备 → usbipd-win → Moonlight Tunnel → TLS → Sunshine reverse_tunnel_service → usbip-win2 → Windows 设备
```

## 当前支持范围

- Windows 客户端：`UsbForwardingBackend` 管理 usbipd-win 的设备共享与枚举。
- Windows 主机：`usbip_host_controller` 调用 usbip-win2。
- Linux、Android 的 USB/IP 服务器可复用相同传输协议，但本 PR 没有实现相应客户端平台集成；不能据此宣称已支持。
- Linux 主机 controller 当前返回 unsupported。macOS 没有本 PR 可用的 USB/IP 后端。

## 鉴权与配置

Moonlight 使用配对时保存的客户端证书/私钥，并验证 Sunshine 的证书与配对 pin 完全一致。通过 IP 连接时不要求证书 CN 等于 IP；任何不同的证书仍被拒绝。Sunshine 要求客户端出示已配对证书，并校验 JSON 中的共享 token。

目前端口和 token **尚未通过 RTSP 协商**，开发测试需要在启动对应进程前设置：

| Sunshine | Moonlight |
|---|---|
| `SUNSHINE_USB_TUNNEL_PORT`（默认 47996） | `MOONLIGHT_USB_TUNNEL_PORT` |
| `SUNSHINE_USB_TUNNEL_TOKEN` | `MOONLIGHT_USB_TUNNEL_TOKEN` |

两端值必须一致。不要把 token、私钥或用户配对状态提交到仓库。当前环境变量 token 不是自动生成的逐会话凭据；未来 RTSP 协商和主机会话生命周期绑定需要单独实现。

## 建立连接

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
