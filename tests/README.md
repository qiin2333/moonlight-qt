# tests/

每个子目录是一个独立可执行测试,`qmake && make` 后直接运行,退出码非零即失败。
CI 统一经 `scripts/run-test.bat`(Windows)调用,法规约见脚本头注释;新测试加入
CI 时在 build.yml 对应步骤加一行 `call scripts\run-test.bat <目录> <TARGET>`。

断言风格尚未统一(require 累积 / CHECK 返回行号 / qFatal 混用),新测试建议用
`require(bool, QString)` 累积风格(clipboard_payload_routing 是范本),保持
main() 短、被测逻辑走生产源文件直编(见各 .pro 的 SOURCES)。

## CI 覆盖(.github/workflows/build.yml)

| 目录 | 验证内容 | CI 平台 |
|---|---|---|
| derive_version | scripts/derive-version.py 版本号推导 | Ubuntu(unittest) |
| cursor_shape_classification | 光标位图 → 形状分类器的判定矩阵 | Windows |
| clipboard_payload_routing | 剪贴板状态、复合内容、文件保护、IPC 分帧和本地 TLS 传输回归（offscreen） | Windows |
| clipboard_helper_lifecycle | helper 异常退出、响应超时和进程管道背压 | Windows |
| overlay_button_position | 悬浮按钮归一化/还原坐标的跨分辨率往返 | Windows |
| file_mapping_websocket_framing | 文件映射 WS 帧解析(fin/分片/ping-pong) | Windows |
| file_mapping_mirror_e2e | 主机文件镜像端到端(FakeRemoteVfs + 挂载提供方) | Windows |
| usb_forwarding_capability | helper 能力 JSON 解析(端口/token/畸形输入) | Windows |
| usb_forwarding_environment | USB 转发就绪态 → 错误文案映射(--require-ready 走活探测) | Windows |
| usb_forwarding_backend_list | moonlight-usbd `list --json` → 设备表解析 | Windows |
| usb_forwarding_backend_sysfs | Linux sysfs 枚举 → 设备表解析（夹具目录，跳过 hub/接口/根 hub） | Windows/Linux |
| ds5_ir_renderer | DualSense 触觉 IR 渲染 | Windows |
| pen_history_selection | 手写笔历史选择 | Windows |
| stylus_replay | 手写笔录制回放 | Windows |
| overlay_event_wake_state | 悬浮菜单事件唤醒状态 | Windows |
| overlay_button_native_wake | 悬浮按钮原生唤醒 | Windows + macOS + Linux(xvfb) |
| overlay_toast_event_state | 悬浮 toast 事件状态 | Windows |
| overlay_menu_navigation | 悬浮菜单键盘/鼠标导航(QTest 事件,需 offscreen) | Windows |
| settings_localization | 中文翻译 .qm + 设置页布局(qmltestrunner) | Windows |
| linux_display_event_monitor | Linux 显示事件唤醒 | Linux |

## 仅本地/手动

| 目录 | 说明 |
|---|---|
| usb_forwarding_tunnel | USB/IP 隧道手动联调探针:需要 10 个 CLI 参数和真实 usbipd 服务端,不能进 CI。用法看 main.cpp 头注释 |
| `file-mapping/smoke/` | 文件映射冒烟二进制,复用 app 源码,手工构建运行。刻意**不注册**进 moonlight-qt.pro 的 SUBDIRS:它链接 app 后端与 OpenSSL,注册会给全部平台构建(含 SteamLink 老工具链)平添失败面,而它没有自动化验证价值 |

## 约定

- 测试**不得**把 Makefile/.o/可执行文件提交进仓库(.gitignore 已覆盖
  `tests/*/{Makefile*,.qmake.stash,*.o,release,debug}`),历史上曾误提交过
  Linux 二进制毒化 Windows 构建(.qmake.stash 被 MSVC qmake 加载直接报错)。
- 被测生产代码有平台分支的(如 clipboardsync.cpp 的 macx 段),跨平台跑法在
  .pro 里用条件 SOURCES 处理,别在测试里复制逻辑。

剪贴板 TLS 回归仅访问本机随机端口。`clipboard_payload_routing/fixtures/` 中的证书和私钥是公开测试素材，不能用于实际配对；证书有效期至 2036 年，届时需要更新。测试不会访问用户配对凭据，也不使用系统剪贴板。macOS 的原生复合写入路径仍需实机验证。
