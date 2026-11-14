# 手柄 UI 导航开关设计

日期：2026-11-12

## 背景

Issue [#210](https://github.com/qiin2333/moonlight-qt/issues/210) 要求关闭 Moonlight 桌面界面的手柄导航，同时保留进入串流后的手柄透传。典型场景是 Windows 上使用 JoyXoff：JoyXoff 把手柄映射为系统鼠标和键盘，Moonlight 又把同一只手柄转换成 Qt UI 按键，于是一次物理操作可能同时触发两套输入，产生双滚动、重复确认和误启动。

官方 Moonlight Qt 有相同问题的开放 Issue [#1519](https://github.com/Moonlight-stream/moonlight-qt/issues/1519)。截至本设计日期，该 Issue 没有评论、关联 PR 或现成配置项。上游当前仍使用 [SdlGamepadKeyNavigation](https://github.com/Moonlight-stream/moonlight-qt/blob/master/app/gui/sdlgamepadkeynavigation.cpp) 在桌面 UI 中生成 Qt 按键。

## 当前实现事实

桌面 UI 在 [main.qml](../app/gui/main.qml) 启动时调用 `SdlGamepadKeyNavigation.enable()`。导航类每 50 ms 轮询 SDL 手柄状态，并从 SDL 队列取出手柄按钮事件；方向键、摇杆、A/B/X/Y 和肩键随后被转换为 Qt `QKeyEvent`，发送给当前焦点窗口。

进入串流前，[StreamSegue.qml](../app/gui/StreamSegue.qml) 会调用 `SdlGamepadKeyNavigation.disable()`。串流期间由 [session.cpp](../app/streaming/session.cpp) 的 `SdlInputHandler` 消费 SDL 控制器事件并发送给主机。串流结束后 UI 导航对象重新启用。因此 UI 导航和主机手柄透传已经有清晰的生命周期边界，开关应放在 UI 导航层，不能修改 `SdlInputHandler` 或 common-c 的协议输入路径。

现有的 `NO_GAMEPAD_QUIT=1` 只禁用串流退出组合键；`gamepadMouse` 只控制串流内手柄鼠标模拟，两者都不能解决 UI 导航冲突。

## 目标

- 提供一个持久化的“启用手柄 UI 导航”设置，默认开启，保持现有用户行为。
- 提供一个命令行临时覆盖，方便 HTPC、JoyXoff 和自动启动场景。
- 关闭后不再由 Moonlight 为桌面 UI 注入方向键、Tab、Enter、Escape 或菜单键。
- 关闭后仍可查询手柄在位状态，避免破坏页面提示和串流启动流程。
- 串流开始后继续使用现有的手柄透传、退出组合键、手柄鼠标和触觉反馈逻辑。
- Windows、Linux、macOS 使用同一套开关语义和实现。

## 非目标

- 不禁用 SDL 手柄设备发现。
- 不改变串流中的手柄按键、轴、传感器或触摸板协议。
- 不替 JoyXoff、Steam Input 或其他系统级映射器实现输入仲裁。
- 不修改 Qt 的全局键盘焦点规则。

## 方案

### 设置项

在 `StreamingPreferences` 增加布尔属性：

```text
enableGamepadUiNavigation = true
```

建议使用正向命名，避免设置页出现双重否定。该值加入 `Q_PROPERTY`、`reload()` 和 `save()`，使用新的 QSettings 键。缺少旧键时默认为 `true`，已有配置无需迁移。

设置页具体放在 `LegacySettingsPage.qml` 的 `Gamepad Settings` 卡片中，作为第一项，排在现有 `Swap A/B and X/Y gamepad buttons` 之前。它与摇杆死区、多手柄和手柄鼠标属于同一组；不要放进串流内的 `Gamepad Mouse` 选项，因为后者只控制会话中的鼠标模拟。

建议显示为：

```text
Use gamepad to navigate Moonlight UI
关闭后，手柄不会控制 Moonlight 桌面界面，但串流中的手柄输入不受影响。鼠标、键盘和外部映射器仍可操作桌面界面。
```

### 导航对象的生命周期

`SdlGamepadKeyNavigation::enable()` 和 `disable()` 继续负责 SDL 控制器子系统以及手柄句柄的生命周期。新增一个独立的导航启用状态，不把设置项直接映射为 `enable()` 是否调用。

这样处理有两个原因：

1. `StreamSegue.qml` 在禁用 UI 导航前会调用 `getConnectedGamepads()` 来生成退出提示；当前实现要求导航对象已经启用。
2. `PcView.qml`、`AppView.qml` 和 `SettingsView.qml` 需要知道是否有手柄，但“发现手柄”和“注入 UI 按键”是两个不同能力。

推荐行为：

- SDL 子系统已启用且窗口获得焦点时，如果设置开启，启动轮询计时器。
- 设置关闭时停止轮询，清空摇杆和 D-pad 连发状态，忽略后续 UI 导航事件。
- 设置重新开启时从下一次轮询重新建立按住状态，不补发关闭期间积压的按钮事件。
- `getConnectedGamepads()` 继续返回真实连接数量。
- 新增 `gamepadUiNavigationEnabled()` 给 QML 使用，避免手柄关闭 UI 导航后仍被自动聚焦。

### 自动焦点

当前 `PcView.qml` 和 `AppView.qml` 在检测到手柄时自动选中第一项，`SettingsView.qml` 在检测到手柄时把焦点放到分类栏。开关关闭时，这些自动焦点行为也应关闭，否则用户虽然看不到按键注入，界面仍会因为手柄存在而改变焦点。

条件改为同时检查：

```qml
SdlGamepadKeyNavigation.gamepadUiNavigationEnabled() &&
SdlGamepadKeyNavigation.getConnectedGamepads() > 0
```

`SettingsView` 仍可设置和清理 `uiNavMode`，但导航计时器停止后不会产生任何 Qt 手柄按键。

### 命令行覆盖

增加临时选项：

```text
--disable-ui-gamepad
```

普通启动和 `stream` 子命令都接受这个选项。`Moonlight.exe --disable-ui-gamepad` 会在界面加载前关闭本次进程的 UI 导航；`moonlight stream ... --disable-ui-gamepad` 保持原有行为。

它只修改当前进程的内存值。用户没有在设置页主动改动时，退出设置仍保存原来的开关，不把这次启动覆盖写回 QSettings；如果用户在本次会话里显式切换了这个开关，新值会照常保存，本次覆盖随之失效。这个选项不改变串流内的手柄透传。

如果后续需要支持脚本显式恢复默认行为，可以增加对称的 `--enable-ui-gamepad`；第一版只提供禁用选项即可，因为默认值已经是启用。

命令行覆盖优先级为：

```text
命令行覆盖 > 持久化设置 > 默认值 true
```

## 三个平台处理方式

### Windows

这是 JoyXoff 主要影响的平台。Moonlight 的 SDL 手柄轮询和 Qt 按键注入在 Windows 上与 JoyXoff 的系统级鼠标/键盘注入并行发生。关闭 UI 导航后，JoyXoff 仍可操作 Moonlight 窗口；进入串流后，SDL 手柄仍按现有路径发送给主机。

不需要 Windows 专属的输入拦截或 Raw Input 改动。

### Linux

SDL 和 Qt 的 UI 导航路径与 Windows 相同，Steam Input、桌面自动化工具和 Wayland/X11 辅助输入也可能产生相同冲突。设置应保持可用。

Wayland/X11 的窗口系统差异不影响方案，因为开关控制的是 `SdlGamepadKeyNavigation` 是否调用 Qt `sendEvent()`，不依赖窗口系统的底层按键注入接口。

### macOS

macOS 的手柄 UI 导航同样由 SDL 轮询和 Qt 合成按键实现。当前 [main.cpp](../app/main.cpp) 还会设置 `Qt::TabFocusAllControls`，保证合成的 Tab 能遍历设置控件。关闭 UI 手柄导航时，这个设置仍应保留，因为它也改善了物理键盘 Tab 导航，不应与手柄开关绑定。

macOS 不需要额外的 Cocoa 手柄路径改动。

## 输入流程

```text
物理手柄
   ├─ SDL 手柄状态
   │    ├─ UI 导航开启 → SdlGamepadKeyNavigation → Qt QKeyEvent → Moonlight UI
   │    └─ UI 导航关闭 → 不生成 Qt UI 按键
   │
   └─ 串流会话启动后 → SdlInputHandler → common-c → 主机

JoyXoff / Steam Input / 外部映射器
   └─ 系统鼠标键盘事件 → Moonlight UI
```

开关只切断第一条分支中的 Qt 合成按键，不处理第二条分支，也不阻止外部映射器产生的系统事件。

## 兼容性与风险

- 默认值为 `true`，升级后现有用户行为不变。
- 关闭后使用纯手柄操作 Moonlight 桌面 UI 会失去导航能力，这是设置项本身的预期结果。
- `getConnectedGamepads()` 必须继续工作，否则会影响 StreamSegue 的退出提示和页面手柄检测。
- 停止轮询时必须清空 D-pad/摇杆状态，避免重新开启后补发过期的 KeyRelease 或重复导航。
- 设置变化发生在手柄按住期间时，重新开启应从新轮询周期建立状态，不能重放旧 SDL 事件。
- 串流会话中的退出组合键仍由 `SdlInputHandler` 控制，不能误用 UI 导航开关替代 `NO_GAMEPAD_QUIT`。

## 验证计划

### 自动化

- `StreamingPreferences`：缺少设置时默认为 `true`，保存/重新加载保留值。
- `SdlGamepadKeyNavigation`：关闭时 D-pad、摇杆、A/B/X/Y 和肩键不生成 Qt 按键；开启时保留现有行为。
- 关闭期间按住 D-pad，再重新开启，不产生过期连发；重新按下后可正常导航。
- `PcView`、`AppView`、`SettingsView`：关闭时不因手柄连接自动抢焦点。
- QML 类型检查和 Windows x64 编译。

### 三平台手工回归

每个平台至少验证：

1. 设置开启：手柄可以浏览主机列表、应用列表和设置页。
2. 设置关闭：手柄不改变 Moonlight UI 焦点，鼠标/键盘仍正常。
3. 外部映射器产生鼠标/键盘事件时，Moonlight UI 可以正常响应。
4. 启动串流后，手柄按键仍能传给主机。
5. 结束串流返回 UI 后，设置状态仍正确。

Windows 额外使用 JoyXoff 验证一次物理操作不会同时触发 Moonlight 合成的确认/返回动作。

## 实施顺序

1. 在 `StreamingPreferences` 增加属性、QSettings 键和命令行覆盖。
2. 在 `SdlGamepadKeyNavigation` 增加状态查询和轮询门控，处理状态清理。
3. 更新 `PcView.qml`、`AppView.qml`、`SettingsView.qml` 的自动焦点条件。
4. 在手柄设置页加入开关和翻译文本。
5. 增加偏好、导航门控和 QML 行为测试。
6. 运行 Windows 构建，并在 Windows、Linux、macOS 做真实手柄回归。

该功能只涉及 Qt UI、SDL 手柄轮询和设置层，不需要修改 moonlight-common-c，也不需要新增协议能力或主机端改动。
