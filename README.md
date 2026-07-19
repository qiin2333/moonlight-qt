# Moonlight PC - Foundation 客户端 fork

[English](README.en.md)

[![Build](https://img.shields.io/github/actions/workflow/status/BuffPlum/moonlight-qt/build.yml?branch=master)](https://github.com/BuffPlum/moonlight-qt/actions/workflows/build.yml?query=branch%3Amaster)
[![Downloads](https://img.shields.io/github/downloads/BuffPlum/moonlight-qt/total)](https://github.com/BuffPlum/moonlight-qt/releases)

> [!WARNING]
> **这是 BuffPlum 独立维护的非官方版本。** 它不由 Moonlight 或 Foundation Sunshine 上游提供支持。全盘文件传输会让已配对客户端访问 Sunshine 进程能够读取的全部磁盘，仅建议在可信局域网和个人设备间使用。安装前请阅读 [安全说明](SECURITY.md)。

这是基于 [moonlight-stream/moonlight-qt](https://github.com/moonlight-stream/moonlight-qt) 维护的下游客户端 fork，主要搭配 [BuffPlum/foundation-sunshine](https://github.com/BuffPlum/foundation-sunshine) 使用。

本项目继续兼容上游 Moonlight / 标准 Sunshine，同时进一步完善 Foundation Sunshine 的客户端体验：能力协商更明确，画质与性能控制更细，串流中的常用操作效率更高。

## 下载

推荐从本 fork 的 [GitHub Releases](https://github.com/BuffPlum/moonlight-qt/releases) 下载构建产物。Windows 是 BuffPlum 版本的主要支持平台，Release 工作流会生成 x64、ARM64 便携包和通用安装程序。

如果你需要上游 Moonlight 的官方发行渠道、移动端客户端、Flatpak、Snap 或发行版软件源，请参考 [Moonlight 官方网站](https://moonlight-stream.org) 和 [上游仓库](https://github.com/moonlight-stream/moonlight-qt)。这些渠道不一定包含本 fork 与 Foundation Sunshine 配套的扩展能力。

## Foundation Sunshine 协同

[BuffPlum/foundation-sunshine](https://github.com/BuffPlum/foundation-sunshine) 是本 fork 的主要服务端配套项目。客户端会在连接时探测服务端能力；当服务端支持对应扩展时启用增强协议，不支持时回退到标准 Moonlight / Sunshine 行为。

这意味着你可以把它当作普通 Moonlight 客户端使用，也可以在 Foundation Sunshine 主机上获得更完整的剪贴板、音频输入、显示控制、码率控制和文件夹映射体验。扩展能力不可用时，客户端会回退到标准兼容行为。

文件传输的具体配置和目录规则见 [文件传输说明](docs/file-transfer.md)。

## 主要增强

### 协议与串流能力

- **双向剪贴板同步**：支持文本与 PNG 图像在客户端和 Foundation Sunshine 主机之间同步，并兼容常见浏览器与系统剪贴板格式。
- **高品质麦克风**：基于 `moonlight-common-c` 的麦克风扩展，支持持续音频输入链路和多声道场景。
- **远程分辨率解耦**：允许串流分辨率独立于本地显示器分辨率，并支持自定义远程分辨率和帧率。
- **AppView 显示控制**：支持目标显示器、虚拟屏组合、远程分辨率和远程帧率选择，让客户端和 Foundation Sunshine 对齐同一套显示意图。
- **全盘双向文件传输**：配套 Foundation Sunshine 自动显示两台电脑的所有可访问磁盘，在独立双栏窗口中浏览目录并双向传输文件或文件夹；传输在后台分块执行且默认拒绝覆盖。

### 画质与性能

- **Sunshine ABR**：在服务端支持时，Foundation Sunshine 可根据客户端反馈动态调整会话码率，在清晰度和稳定性之间取得更合理的平衡。
- **高码率局域网串流**：保留面向 Sunshine 主机的高码率选项，适合有线局域网、桌面串流和高分辨率画面。
- **硬件解码与现代视频格式**：继承上游 Moonlight 的硬解能力，支持 H.264、HEVC、AV1、HDR 和 YUV 4:4:4 等能力组合，具体可用性取决于客户端 GPU 与服务端编码能力。
- **帧节奏与延迟控制**：保留 V-Sync、frame pacing、全屏/无边框窗口等选项，方便在低延迟、顺滑度和桌面工作流之间取舍。
- **性能覆盖层**：展示实时串流指标，并补充渲染时间等观测信息，让串流调优更可量化。

### 客户端体验

- **Win11 风格悬浮菜单**：包含动画、图标和可选浮动月亮按钮。
- **悬浮菜单快捷控制**：串流中可快速切换全屏、性能统计、鼠标模式、光标显示、麦克风、主机文件访问等常用动作。
- **手柄体验增强**：可配置退出组合键，支持手柄鼠标即时切换，并在有手柄时展示相关选项，减少无关设置干扰。
- **远程桌面鼠标模式**：保留游戏指针捕获和远程桌面直接鼠标控制两种模式，适合游戏、桌面办公和轻量维护场景。
- **窗口边缘自动释放鼠标**：窗口模式下可在悬浮菜单中控制鼠标到达窗口边缘时是否自动移出串流窗口。
- **串流时自动禁用 IME**：Windows 下基于 Win32 IMM hooks 降低输入法干扰，提升键盘输入稳定性。
- **AppView 信息展示**：应用列表中展示运行状态和显示器选项，帮助用户更明确地选择要启动的位置。

### 自动化与发布

- **本 fork 更新检查**：客户端更新检查指向 `BuffPlum/moonlight-qt` 的 GitHub Releases。
- **基于 Git tag 的版本号**：`scripts/derive-version.py` 支持 CI 与本地构建产物使用一致的版本命名。
- **自动翻译构建**：`.github/workflows/build-translate.yml` 用于周期性更新 `.ts` / `.qm` 翻译资源。
- **CI 构建矩阵**：Windows、macOS、Linux AppImage 和 Steam Link 均有独立工作流维护。
- **独立 Windows Release**：发布 `vX.Y.Z-buffplum.N` 版本时自动生成 Windows 安装程序与便携包。

## 兼容性

- 标准 Moonlight / Sunshine 协议保持兼容，普通主机不需要额外配置。
- 连接标准 Sunshine 或上游 Moonlight 兼容服务端时，剪贴板图片、HQ Mic、ABR、文件夹映射等 Foundation 扩展会自动降级或关闭。
- NVIDIA GameStream 相关兼容性继承自上游 Moonlight，但新功能主要围绕 Sunshine 生态维护。

## 构建

### 通用步骤

```powershell
git submodule update --init --recursive
```

Windows 和 macOS 构建前还需要安装预构建依赖：

```powershell
.\setup-deps.ps1
```

macOS 可使用：

```bash
python3 setup-deps.py
```

### Windows

要求：

- Qt 6 SDK，建议使用与 CI 接近的 Qt 6.11.x。
- Visual Studio 2022，使用 MSVC 工具链。
- 生成面向普通用户的安装包时需要 7-Zip。
- 调试 DirectX 相关问题时可安装 Windows Graphics Tools。

常用 release 构建脚本：

```cmd
scripts\build-arch.bat x64 release
scripts\generate-bundle.bat
```

### macOS

要求：

- Qt 6 SDK，建议使用与 CI 接近的 Qt 6.11.x。
- Xcode 14 或更新版本。
- 生成 DMG 时需要 `create-dmg`。

常用构建命令：

```bash
qmake6 moonlight-qt.pro
make release
scripts/generate-dmg.sh
```

### Linux

要求：

- 推荐 Qt 6；Qt 5.12 或更新版本仍保留兼容。
- GCC 或 Clang。
- FFmpeg 4.0 或更新版本。
- Vulkan renderer 需要 `libplacebo` 至少 v7.349.0，并建议 FFmpeg 6.1 或更新版本。

Debian / Ubuntu 依赖示例：

```bash
sudo apt install libegl1-mesa-dev libgl1-mesa-dev libopus-dev libsdl2-dev libsdl2-ttf-dev libssl-dev libavcodec-dev libavformat-dev libswscale-dev libva-dev libvdpau-dev libxkbcommon-dev wayland-protocols libdrm-dev qt6-base-dev qt6-declarative-dev libqt6svg6-dev qt6-wayland
```

开发构建：

```bash
qmake6 moonlight-qt.pro
make debug
```

### Steam Link

要求：

- 克隆 [Steam Link SDK](https://github.com/ValveSoftware/steamlink-sdk)。
- 设置 `STEAMLINK_SDK_PATH` 环境变量。

构建：

```bash
scripts/build-steamlink-app.sh
```

Steam Link 原始硬件限制：

- 最大分辨率 1080p。
- 最大帧率 60 FPS。
- 最大视频码率 40 Mbps。
- 不支持 HDR 串流。

## 贡献

这个仓库是 BuffPlum 版本的主要开发仓库，不再把 Fork 专属功能提交给上游。上游仅作为同步兼容性修复和安全更新的来源；Fork 专属问题请提交到 [BuffPlum/moonlight-qt Issues](https://github.com/BuffPlum/moonlight-qt/issues)。

版本号使用 `vX.Y.Z-buffplum.N`，例如 `v6.2.92-buffplum.1`。发布流程见 [.github/workflows/README.md](.github/workflows/README.md)。

提交 issue 或 PR 时，请尽量说明：

- 客户端系统和版本。
- Foundation Sunshine 或标准 Sunshine 的版本。
- 是否使用本 fork 的 release 构建。
- 是否涉及扩展能力，例如剪贴板、HQ Mic、ABR、文件夹映射或远程分辨率。

## 上游与许可

本项目基于 [Moonlight Qt](https://github.com/moonlight-stream/moonlight-qt)，遵循仓库内 [GPLv3 License](LICENSE)。

上游 Moonlight 项目链接：

- [Moonlight 官网](https://moonlight-stream.org)
- [Moonlight Qt](https://github.com/moonlight-stream/moonlight-qt)
- [Moonlight 文档](https://github.com/moonlight-stream/moonlight-docs/wiki)
- [Moonlight Discord](https://moonlight-stream.org/discord)
- [Moonlight Weblate](https://hosted.weblate.org/projects/moonlight/moonlight-qt/)

感谢 Moonlight、Sunshine 以及相关开源依赖项目的长期维护。
