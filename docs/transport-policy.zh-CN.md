# 实验传输策略接入

本分支接入 Sunshine 的逐包反馈、配对 HTTPS 会话策略及可靠状态通知，公共库依赖见 [PR #31](https://github.com/qiin2333/moonlight-common-c/pull/31)。主机总预算包含源视频、FEC、音频、控制和反馈成本；客户端滑块在新控制模式下表示总网络上限。

## 权限与状态

观测、反馈和控制分别配置；反馈或状态通知不授予控制权。只有原连接明确协商控制能力、配对查询新鲜且主机允许实时控制时才开放策略写入。只读观测维持旧码率交互。策略请求、主机接受、SDK 应用、首包提交分别展示；首发回执不证明送达、解码或体验改善。

按 sessionId、connectionEpoch、controlEpoch、revision 对账。拒绝旧连接、倒退和不完整回复；64 位身份按十进制字符串传递。通知用于提前唤醒原连接的查询，策略值与权限继续由配对 HTTPS 确认，保留周期查询补偿。过期、失败和缺失反馈不展示为零丢包。手动接管明确撤销自动控制；连接停止取消旧任务及迟到回调，重连采用已确认的预算。

## 当前验收边界

`9c9fc7cd` 的普通与开发 Qt/MSVC 完整构建、62 项 Qt 回归及导航检查通过；与 `6e179e966` 主机的实际配对 Session 分别验证真实 409 冲突和接受后回复丢失，两次均经重新查询及菜单重试完成 13 项操作。独立审计各 27 个不可变策略版本及实际 SDK/首包回执，详见主机[联合验证记录](https://github.com/AlkaidLab/foundation-sunshine/blob/codex/adaptive-fec-control/docs/adaptive-fec-validation.zh-CN.md)。默认功能开关保持关闭，完整控制/通知故障矩阵、设备生命周期、共享预算/公平性、参考链与期限、同预算画质/冻结/延迟及资源成本仍需逐项验收。

## PC 配置与复验

`9a220f05` 修复 Wayland 串流窗口包装失败后的启动路径：先删除未包装成功的 Qt 窗口，再尝试既有 SDL 窗口创建及平台标志重试。普通与开发 Windows Qt/MSVC 完整构建通过；Windows 不包含原生 Wayland 分支，Linux 构建及实际 Wayland 包装失败场景分别验证，不以 Windows 构建证明其运行结果。

普通构建支持显式环境选项 `MOONLIGHT_VIDEO_PACKET_FEEDBACK=1` 和 `MOONLIGHT_VIDEO_PACKET_CONTROL=1`，请求控制会同时请求反馈；未获主机协商确认时保持原路径。`MOONLIGHT_VIDEO_NETWORK_OBSERVATION=1` 仅请求观测。控制专用会话不请求视频控制。

策略工作线程独立持有 HTTP 与状态镜像，UI/串流线程读取副本。JSON 主机接口强制配对证书、禁止重定向，查询/提交有两秒超时。生产菜单由主机回执驱动，不按一次点击假定编码器已应用。

Windows 的现有 `scripts/run-test.bat` 支持 `transport_policy`、`transport_policy_menu`、`legacy_bitrate` 和 `overlay_menu_navigation`，各目录名也是可执行文件名，菜单测试使用 `QT_QPA_PLATFORM=offscreen`。新增三组测试已接入现有 CI。完整 Session 驱动见 [测试说明](../tests/transport_policy_session/README.md)，仅 `MOONLIGHT_ENABLE_FUNCTION_TESTS=1` 开发构建包含该驱动；普通应用不暴露驱动入口。
