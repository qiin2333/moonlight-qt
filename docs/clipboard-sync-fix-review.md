# 剪贴板同步复核与修复

本轮按生产调用链和可执行回归重新确认问题，没有把所有风险都当作已发生的故障。

| 原检查项 | 复核结论 | 本轮处理 |
| --- | --- | --- |
| 会话退出时 helper 被接收线程访问 | 确有生命周期竞态；并非队列加锁就能保证对象存活 | 接收回调与对象摘除共用互斥锁，锁外停止并销毁子进程 |
| 复制图片文件会发送文件内容 | 已复现：只提供本地图片 URL 就会读文件并发送 | 删除文件 URL / HTML 本地文件读取路径；Shell 文件格式提前阻断；保留聊天应用自带位图，去掉其路径文字 |
| Blob 证书固定缺口 | 条件型真实问题：正常受信任但非配对的证书绕过 sslErrors 检查 | TLS encrypted 阶段检查配对证书，禁止重定向，主机身份改变时销毁连接池；完成阶段再次校验 |
| helper 非法尾部输出使主进程崩溃 | 已由故障注入覆盖：异常处理删除 QProcess，外层随后解引用 | 清理后重新检查对象，避免重复处理已删除进程 |
| 网络与图片内存上限失效 | 上限在下载或解码后检查，不能防止此前的资源消耗 | 下载分段限额、响应头限额、30 秒截止、最多 4 个活动请求；图片先读尺寸再解码 |
| 异步大内容覆盖新内容 | 上行、下行均是真实的时序问题 | 每次真实更新递增版本并取消旧请求；回调检查版本；停止或重新配置同一主机也使旧请求失效 |
| A → B → A 漏发 | 已复现 | 回声识别改为当前完整剪贴板快照，不以五秒历史内容去重 |
| 非零 token 文本丢失、同图换文字不更新 | 已复现 | 每个有效片段立即应用；支持反序聚合；按完整状态发送复合内容，拒绝近期已被替代的 burst |
| 60000–65525 字节复合文本走异步 REF | 条件与分流逻辑不一致 | 非零 token 文本保持内联；同时为加密控制头预留 24 字节，实际内联文本上限为 65501 字节，超限仍按既有策略仅发图片 |
| IPC 限额漏掉 QProcess / GUI 队列 | 真实的背压缺口 | 纳入 bytesToWrite；stdin 读取线程每次等待 GUI 接收，限制在途投递 |
| 多条合法消息被当作一条超长行 | 已由测试覆盖 | 逐行判断限制，保留未完成的尾行 |
| helper 活着但不响应时不恢复 | 真实的监督缺口 | Ping/Pong、响应超时与单独的 READY 超时 |
| 全局双向同步的隐私策略 | 属于产品行为选择，不等于实现故障 | 本轮保留原策略，没有擅自改成仅前台同步或新增默认开关 |

## 验证

- `clipboard_payload_routing`：原有边界测试，以及 offscreen 剪贴板状态、复合文字/图片、文件引用、IPC 分帧、加密长度边界回归。
- 同一测试中的本地 HTTPS 服务：正确配对证书、受信任但非配对证书、旧下载/上传取消、停止与重配、Content-Length 及 chunked 超限、畸形 REF 不影响在途请求。
- `clipboard_helper_lifecycle`：用测试子进程注入非法退出输出、卡死、READY 缺失、内部管道积压和多行合法输出。
- 两组测试纳入 Windows CI；使用公开测试证书，不读取用户配对身份。
- Windows Qt 6.11.1 / MSVC Release 完整构建。

上述自动回归使用 offscreen 剪贴板和本机随机端口。会话退出竞态通过调用顺序和锁保护复核，未声称完成并发压力实机复现。macOS 原生粘贴板仍需对应环境验证。Linux 和真实 Sunshine 实测结果见下文。


## Hyper-V Ubuntu 实测（2026-09-22）

环境：本机 Hyper-V `spike-ubuntu26`，Ubuntu 26.04.1 LTS，Qt 6.10.2，GCC 15，SDL 2.32.10，GNOME Shell / Mutter 50.1。所有测试在独立目录执行；没有改动 VM 当前桌面的剪贴板。

| 验证层 | 结果 | 范围 |
| --- | --- | --- |
| Linux 编译 | 通过 | 两组回归测试和正式 `moonlight-clipboard-helper`；不是完整客户端构建 |
| `clipboard_payload_routing` | 通过 | 包括本地 HTTPS 证书固定、异步传输取消、下载限额及完整剪贴板状态回归 |
| `clipboard_helper_lifecycle` | 通过 | 异常输出、响应 / READY 超时、管道背压、多行消息 |
| X11 原生跨进程剪贴板 | 通过 | 独立 Xvfb + 正式 helper + 独立 Qt 应用；双向文本/图文、中文和 emoji、A→B→A、同图换文字、无回声、文件保护 |
| GNOME 原生 Wayland | **未通过，兼容性问题未修复** | 独立 headless GNOME 会话；测试应用已获得键盘焦点，复制成功后 helper 无出站帧；注入入站文本后另一进程仍读到原本的本地文本；Ping/Pong 正常 |

Wayland 协议记录显示后台 helper 绑定的是 `wl_data_device_manager`，没有收到测试应用的剪贴板更新；反向写入调用为 `wl_data_device.set_selection(..., 0)`。该独立 GNOME 会话没有向 helper 公布 data-control 扩展。由此确认：原先基于无窗口 helper + QClipboard 的全局同步方案在这个原生 Wayland 环境不成立，不能用 offscreen 或 X11 测试通过来推断 Wayland 可用。此项属于新增发现，尚未实现 Wayland 授权剪贴板通道。

X11 验证并不等同于“Wayland 桌面下强制 xcb 也一定可用”；后者本轮没有验证。下节另行记录后续真实 Sunshine 端到端串流。

复现工具位于工作区 `.codex-build/linux-clipboard-validation/native/`；结果与 Wayland 协议记录位于 `.codex-build/linux-clipboard-validation/evidence/`。VM 目录为 `/home/spike/clipboard-validation-20260922`。测试 compositor、Xvfb、helper 和 peer 均已退出。

Linux 编译另发现一个已验证 MIME 分支中的枚举/整数条件表达式警告，已删除不可达分支，重新编译并运行上述通过项。

## 本机 Sunshine 端到端实测（2026-09-22）

通过正常 PIN 配对连接本机已安装的 Sunshine `v2026.921.161858.杂鱼`。在 VM 内复制已有完整源码构建目录，覆盖本轮修改的 session、clipboard 和 helper 文件后成功编译完整客户端；使用独立 Xvfb 桌面，通过真实 Desktop 串流、加密控制通道和配对身份的 HTTPS blob 接口传输。Windows / Linux 两个独立 Qt peer 只读写系统剪贴板，没有代替同步实现。比对文本 SHA-256 和 RGBA 像素 SHA-256，文本仅归一化 CRLF/LF。Windows 原剪贴板在测试进程内保留并于结束恢复；VM 用户当前桌面未参与。

归一化后的诊断轮为 **15/18 通过**，不能称为全链路全部通过：

| 方向 / 用例 | 结果 |
| --- | --- |
| Windows → Ubuntu：A→B→A、中文/emoji/换行、小 PNG、大 PNG | 6 项通过 |
| Windows → Ubuntu：图片后复制同图配文字、同图换文字 | 2 项失败；线上分别收到 token=0 的纯文字或纯图片，未收到完整 compound burst |
| Windows → Ubuntu：290 KB 文本 | 失败；未收到对应 REF |
| Ubuntu → Windows：以上全部 9 项 | 诊断轮全部通过；更早一轮同图换文字曾失败，仍存在主机端时序不稳定，不能据单轮通过认定已消除 |

通过只记录帧类型、token、长度和哈希的 helper 透明转发器进一步定位：

1. **Sunshine 主机侧复合格式去重缺陷**：本地 Sunshine 源码 `src_assets/common/sunshine-control-panel/src-tauri/src/clipboard.rs` 的 `snapshot_and_post()` 分别检查 text / png 回声，在一个格式重复时用 `post_outbound()` 发另一个格式，token=0。此行为与实际收到的单帧一致。Moonlight 无法从独立文字帧判断主机是否原本还带图片，不能通过无条件保留旧图片修复，否则会破坏正常复制纯文字。
2. **Sunshine 主机侧大文本 MIME 不兼容**：其 `MIME_TEXT` 为 `text/plain; charset=utf-8`，blob 接口禁止带参数的 MIME。向本机接口发送同一测试内容的对照实验：该值返回 `400 {"error":"bad_mime"}`，`text/plain` 返回 200。Moonlight 本轮已使用无参数 MIME，因此 Ubuntu → Windows 的大文本实际传输成功。这两个主机端问题未在当前 Moonlight 仓库中修复，也未替换用户安装的 Sunshine。
3. 首次恢复旧 Desktop 会话时，Sunshine 发生访问违例并由服务重新启动。Windows Application 事件 1000 / 1001 记录 `2026-09-22 23:50:00`、异常 `0xc0000005`、未知模块。后续新建及恢复串流成功；没有证据将崩溃归因于剪贴板，本轮未修改主机编码器或会话代码。

完整矩阵结果和帧元数据位于 `.codex-build/linux-clipboard-validation/evidence/e2e-matrix.log`、`e2e-wire-metadata.log`；复现工具位于 `.codex-build/linux-clipboard-validation/e2e/`。透明转发器不记录 configure 中的配对私钥或用户原剪贴板。实际测试配对名为 `Ubuntu clipboard validation`。

另外一轮补充 **5/5 通过**：Ubuntu → Windows 的 65,501 / 65,502 字节纯文本、在途大图片之后的新文字保持最终状态、Ubuntu 文件剪贴板拒绝入站覆盖、清除文件剪贴板后正常同步恢复。结果在 `evidence/e2e-extra.log`。这两个纯文本长度用例走现有 blob 分流，不作为 compound 内联长度边界的实机证明；后者由前述协议回归覆盖。所有测试进程退出，保留正常配对；未结束主机 Desktop 应用。

## Sunshine 修复后复测（2026-09-23）

上述两个主机端问题已在 [Sunshine Control Panel #148](https://github.com/qiin2333/sunshine-control-panel/pull/148) 修复；按维护者要求不更新 Sunshine 主项目的子模块 pin。新面板改为完整当前快照去重，序列化接收写入与 watcher 读取，并使用 `text/plain` 上传文本。

使用实际构建的新 Windows 面板进程（已核对可执行文件路径）、原 Core 服务和上述修复版 Linux Moonlight 重新串流：主矩阵 **18/18**、补充保护用例 **5/5** 全部通过。每项主矩阵内容匹配后再等待 800 ms 检查，防止把短暂正确但随后被覆盖算为成功。线上已确认 compound 两帧共享非零 token，主机大文本 REF 使用 `text/plain`。面板完整 Rust 测试 223 通过、3 忽略、0 失败，前端和 Windows MSVC 构建通过。

结果保存在 `evidence/sunshine-fixed-matrix.log`、`sunshine-fixed-wire.log`、`sunshine-fixed-extra.log`。测试后已恢复已安装面板与原 Windows 剪贴板；没有替换 Core 服务二进制。Wayland 限制和首次旧会话恢复崩溃的未定原因仍保持前述结论。
