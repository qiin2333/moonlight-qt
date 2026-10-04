# 实验传输策略接入

本分支接入 Sunshine 的逐包反馈、配对 HTTPS 会话策略及可靠状态通知，公共库依赖见 [PR #31](https://github.com/qiin2333/moonlight-common-c/pull/31)。主机总预算包含源视频、FEC、音频、控制和反馈成本；客户端滑块在新控制模式下表示总网络上限。

## 权限与状态

2026 年 10 月 4 日主机移除历史突发回放的自动 FEC。客户端仅在 `experimentalAutomaticFecAvailable` 是 JSON boolean true 时允许开启；缺失、false 或错误类型均禁用开关并拦截启用请求。自动码率和手动预算仍可操作，旧状态里的 FEC 意图不会阻断这些操作。手动 FEC 和现有 RS 编解码保留。

观测、反馈和控制分别配置；反馈或状态通知不授予控制权。只有原连接明确协商控制能力、配对查询新鲜且主机允许实时控制时才开放策略写入。只读观测维持旧码率交互。策略请求、主机接受、SDK 应用、首包提交分别展示；首发回执不证明送达、解码或体验改善。

按 sessionId、connectionEpoch、controlEpoch、revision 对账。拒绝旧连接、倒退和不完整回复；64 位身份按十进制字符串传递。通知用于提前唤醒原连接的查询，策略值与权限继续由配对 HTTPS 确认，保留周期查询补偿。过期、失败和缺失反馈不展示为零丢包。手动接管明确撤销自动控制；连接停止取消旧任务及迟到回调，重连采用已确认的预算。

## 当前验收边界

本轮能力收敛通过普通/开发 Qt/MSVC 完整构建、31 项策略测试、10 项菜单测试、23 项兼容码率测试及导航检查。新增覆盖验证 FEC 禁用、请求拒绝、能力变化后菜单重建，以及旧状态的 FEC 意图不阻断自动码率。下文自动 FEC 的实际会话证据仅描述旧版本。

`911021ad` 在全新构建目录生成开发客户端，与 Sunshine `c2d80af7` 完成八次实际菜单操作和 32 份独立 HTTPS 采样；自动 FEC 始终禁用，自动码率、预算和手动接管仍可用。三次配对启用请求均返回 `400: Automatic FEC is unavailable`，各次捕获的策略、接受版本及控制代次不变。手动 RS 三项均保持 50%；客户端实际接收、解码、渲染并正常退出。首次旧增量产物未进入串流的失败，以及第二次日志分隔符解析错误的原始结果均保留；复核第二次终态日志后修正解析检查。菜单图只验证生产绘制函数，IPv6 loopback 和未冻结桌面不证明同预算体验或桌面合成验收。

`9c9fc7cd` 的普通与开发 Qt/MSVC 完整构建、62 项 Qt 回归及导航检查通过；与 `6e179e966` 主机的实际配对 Session 分别验证真实 409 冲突和接受后回复丢失，两次均经重新查询及菜单重试完成 13 项操作。独立审计各 27 个不可变策略版本及实际 SDK/首包回执，详见主机[联合验证记录](https://github.com/AlkaidLab/foundation-sunshine/blob/codex/adaptive-fec-control/docs/adaptive-fec-validation.zh-CN.md)。默认功能开关保持关闭，完整控制/通知故障矩阵、设备生命周期、共享预算/公平性、参考链与期限、同预算画质/冻结/延迟及资源成本仍需逐项验收。

`70173f17` 修复 1 FPS 时渲染历史窗口为零而从空队列取值的崩溃，两个窗口各保留至少一项；普通/开发 Qt/MSVC 在全新目录完整构建通过，后续低 FPS 会话正常退出。1 FPS 会触发 CLI 的推荐范围警告，其 SDK 场景目标未通过，不能作为整体支持范围扩大。

1280×720 H.264、60 FPS、显式 AMF CBR、手动 FEC 0 的独立会话验证了真实 SDK 越界拒绝、属性回滚与编码器重建。配对兼容请求 800000 Kbps 的 revision 2 保持 backend_failure、未应用且无首包；随后 6000 Kbps 的 revision 3 实际应用并提交首包。PC 只读查询保留同一失败历史，83 份主机采样及三个不可变版本独立对账，客户端解码/渲染并正常退出。此前夹具失败和最终检查器的空 QString/布尔解析、提交资格假设错误均保留；只读查询不授予控制权，不证明活动新控制的 SDK 故障交互或失败时重连。详细证据见主机[验证记录](https://github.com/AlkaidLab/foundation-sunshine/blob/codex/adaptive-fec-control/docs/adaptive-fec-validation.zh-CN.md)。

`f79613f1` 的全新完整开发构建另完成活动新控制的七次生产菜单操作：800000 Kbps 请求的 revision 5 得到真实 AMF backend_failure，未应用、未首发，随后菜单提交 6000 Kbps 的 revision 6 实际应用并首发，再调整自动码率/上限和手动接管。164 份独立 HTTPS 采样、13 个不可变版本、31 项运行检查通过；失败历史保留，实际接收/解码/渲染并正常退出。两张生产菜单绘制图分别显示 Failed (r5) 与 First packet sent (r6)，已确认 SDK 目标分别为 7.2/5.2 Mbps；滑块预算不等同于编码目标。新增 SDK 失败观察仅进入开发驱动，普通应用源码与已完整构建的 `70173f17` 相同。

此次成功对照显式使用 Qt Quick basic/D3D11；默认 threaded 的三次启动未进入 Session.initialize，启动页与 CLI 参数正确，诊断启动器已进入 StateStartSession。两次私有进程清理及一次调试命令失败后的非零退出均保留，不能计为 SDK 拒绝或产品崩溃证明。basic 对照未挂调试器；默认启动差异未修复，不能据此宣称桌面合成、完整启动、SDK 在途重连、其他编码器或 QoE 验收通过。

## PC 配置与复验

`9a220f05` 修复 Wayland 串流窗口包装失败后的启动路径：先删除未包装成功的 Qt 窗口，再尝试既有 SDL 窗口创建及平台标志重试。普通与开发 Windows Qt/MSVC 完整构建通过；Windows 不包含原生 Wayland 分支，Linux 构建及实际 Wayland 包装失败场景分别验证，不以 Windows 构建证明其运行结果。

`83fb3372` 合入主分支 `46650e13` 的统一配对证书校验，保留旧传输接口的连接作用域。普通与开发完整构建、62 项策略/菜单回归、导航与上游 shared_helpers 回归通过；实际配对 Session 重新完成真实 409 冲突、查询、菜单重试及 13 项操作，独立核对 50 份样本与 27 个策略版本。`4ca17394` 为独立 NvHTTP 消费者的共用构建清单补齐策略解析源文件；上游实际 TLS 回归在此前因缺失三个策略符号链接失败，补齐后构建与运行通过。仅修正构建依赖，未改证书校验或增加生产模块。

普通构建支持显式环境选项 `MOONLIGHT_VIDEO_PACKET_FEEDBACK=1` 和 `MOONLIGHT_VIDEO_PACKET_CONTROL=1`，请求控制会同时请求反馈；未获主机协商确认时保持原路径。`MOONLIGHT_VIDEO_NETWORK_OBSERVATION=1` 仅请求观测。控制专用会话不请求视频控制。

策略工作线程独立持有 HTTP 与状态镜像，UI/串流线程读取副本。JSON 主机接口强制配对证书、禁止重定向，查询/提交有两秒超时。生产菜单由主机回执驱动，不按一次点击假定编码器已应用。

Windows 的现有 `scripts/run-test.bat` 支持 `transport_policy`、`transport_policy_menu`、`legacy_bitrate` 和 `overlay_menu_navigation`，各目录名也是可执行文件名，菜单测试使用 `QT_QPA_PLATFORM=offscreen`。新增三组测试已接入现有 CI。完整 Session 驱动见 [测试说明](../tests/transport_policy_session/README.md)，仅 `MOONLIGHT_ENABLE_FUNCTION_TESTS=1` 开发构建包含该驱动；普通应用不暴露驱动入口。
