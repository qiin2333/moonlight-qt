# Remote USB 反向隧道方案 — 架构设计（替代 RUSB）

> 核心洞察：usbipd-win（导出端）和 usbip-win2（导入端）本来就是一对标准 USB/IP
> over TCP 实现。Moonlight 中间**唯一**要做的是把这条"方向反了"的 TCP 连接
> 反向隧道化（远程场景 NAT 阻断直连），外加认证。USB/IP 协议本身一个字节都不该
> 由 Moonlight 理解。

## 1. 一句话架构

**Moonlight 提供「认证的反向 TCP 端口转发」，让 Sunshine 侧的 USB/IP 客户端
（usbip-win2）像 attach 本机服务一样 attach 到客户端侧的 USB/IP 服务器（usbipd）。
数据面是纯字节流拷贝，无 USB/IP 语义。**

```
[客户端 PC · Moonlight]                     [游戏 PC · Sunshine]
USB/IP 服务器（usbipd-win）                  USB/IP 客户端（usbip-win2）
  bind → 监听 TCP:3240                          attach 127.0.0.1:<p>
        ▲                                            ▼
        │本地 TCP 连 127.0.0.1:3240                   │本地 TCP 连 127.0.0.1:<p>
        ▼                                            ▼
Moonlight 反向隧道客户端                     Sunshine 反向端口转发服务
        │                                            │
        └───────── TLS（客户端发起，穿 NAT）──────────┘
                    双向字节流拷贝
```

## 2. 架构本质：一个通用层 + 一个平台适配点

反向隧道方案的真正价值，是把 Moonlight 的角色**压到只剩两层**：

```
┌───────────────────────────────────────────────┐
│ 通用层（全平台一致，一次实现）                   │
│  - UI/UX：设置页、绑定对话框、overlay           │
│  - 反向隧道客户端：字节流拷贝 + TLS + 认证       │
│  - 会话绑定                                     │
└───────────────────────┬───────────────────────┘
                        │ 连本地的「USB/IP 服务器」
                        ▼
┌───────────────────────────────────────────────┐
│ USB/IP 服务器（平台差异集中在这里）              │
│  - Windows: usbipd-win（现成，外部进程）        │
│  - Linux:   usbip-host 内核 + usbipd（现成）    │
│  - Android: USBIPServerForAndroid（现成，前台服务）│
│  - macOS / iOS：无现成 → 见第 4 章              │
└───────────────────────────────────────────────┘
```

**通用层不碰 USB/IP，所以跨平台；USB/IP 的实现完全外挂，所以平台差异只落在
"外挂哪个 USB/IP 服务器"这一个点。** 这正是"外挂 usbipd"方案的精髓，也是它
区别于 RUSB 的关键：RUSB 把 USB/IP 编解码塞进了通用层，导致每个平台都要背
协议栈；反向隧道把它剥出去，通用层瘦成字节流。

## 3. 组件与职责

| 侧 | 组件 | 职责 | 状态 |
|---|---|---|---|
| 客户端 | USB/IP 服务器（usbipd-win / usbip-host / USBIPServerForAndroid） | 操作设备、跑 USB/IP | 外部，已有 |
| 客户端 | `UsbForwardingBackend` | bind/unbind/list + UAC | **已实现**（前几批） |
| 客户端 | 反向隧道客户端（agent 内新增） | 连本地 USB/IP 服务器 + 连 Sunshine TLS + 字节流拷贝 | 新增 |
| Sunshine | 反向端口转发服务 | 监听 127.0.0.1 + 接受 TLS + 触发 USB/IP 客户端 attach + 字节流拷贝 | 新增（简化自 loopback bridge） |
| Sunshine | USB/IP 客户端（usbip-win2） | 导入设备、建 UDE | 外部，已有 |

## 4. 跨平台接入（核心约束）

反向隧道方案把 Moonlight 的职责压成字节流，代价是：**能否转发 USB，取决于两端
各自有没有现成的 USB/IP 实现。** 这是标准 USB/IP 的平台支持矩阵，不是 Moonlight
能补的。

### 4.1 客户端（有设备的机器，Moonlight）——决定"设备能不能被导出"

| 平台 | USB/IP 服务器 | 反向隧道通用层 | USB 转发 |
|---|---|---|---|
| **Windows** | usbipd-win（成熟，签名驱动） | ✅ | ✅ 极简外挂 |
| **Linux** | usbip-host 内核 + usbipd | ✅ | ✅ 外挂现成 |
| **Android** | [USBIPServerForAndroid](https://github.com/cgutman/USBIPServerForAndroid)（cgutman 本人，前台服务） | ✅ | ✅ 外挂现成 |
| **macOS** | 无 | ✅ | ❌ 无后端可挂 |
| **iOS / tvOS** | 无，USB 访问受限 | ✅ | ❌ 不支持 |
| **HarmonyOS** | 无 | ✅ | ❌ 不支持 |

> Android 关键反转：旧 RUSB 文档说 USBIPServerForAndroid"需要 RUSB adapter 才能用"
> （因为它没有 84 字节 broker HELLO / 32 字节 framing）。在反向隧道架构下该判断
> 彻底反转——反向隧道恰恰不要 framing，要的就是裸 USB/IP 字节流透传，所以
> USBIPServerForAndroid **零 adapter、直接可用**。它是标准 USB/IP 1.1.1 服务器
> （OP_REQ_DEVLIST=0x8005 / OP_REQ_IMPORT=0x8003 / 48B URB PDU / TCP:3240，前台
> Service），与 usbipd-win 同角色、与 usbip-win2 直接配合。

### 4.2 主机（用设备的机器，Sunshine）——决定"设备能不能被导入"

| 平台 | USB/IP 客户端 | USB 转发 |
|---|---|---|
| **Windows** | usbip-win2（成熟） | ✅ |
| **Linux** | vhci-hcd 内核 + usbip attach | ✅ |
| **macOS** | 无 | ❌ |

### 4.3 结论与阶段划分

- **首期（P1）**：Windows / Linux / Android 客户端 → Windows / Linux 主机。两端都有
  现成 USB/IP 实现，反向隧道方案全速落地，通用层一次实现三端复用。
- **Linux 客户端接入**：通用层（反向隧道 + UI/UX）完全复用，只需给
  `UsbForwardingBackend` 加一个 Linux 后端——`usbip bind`（内核 usbip-host）替代
  `usbipd bind`，语义完全对齐（bind/unbind/list 都是标准 USB/IP 概念）。
- **Android 客户端接入**：编排 USBIPServerForAndroid（启动前台服务 + USB 设备
  授权），反向隧道客户端连本地 3240。与 Windows 编排 usbipd-win 完全对称。
- **macOS**：唯一"完全无 USB/IP 服务器"的主要平台。若要支持，唯一路径是自实现
  USB/IP 服务器（libusb + 自编解码），工作量巨大且与"外挂极简"背道而驰。
  **建议首期明确不支持，保留文档级留白。**

### 4.4 这一层如何反过来回答「RUSB 到底还要不要」

- RUSB 的唯一不可替代价值，是它为"**没有 USB/IP 服务器的平台**"提供了自实现路径。
- 一旦目标收敛为"Windows / Linux / Android 客户端 → Windows / Linux 主机"（外设
  转发的主力场景），这三个平台都有现成 USB/IP 实现（usbipd-win / usbip-host /
  USBIPServerForAndroid），RUSB 的跨平台自实现能力就**用不上**。
- 所以：**按反向隧道重构，RUSB 栈整体退役**；macOS 留作未来（若真要做，再考虑
  是否复刻 USB/IP 服务器实现）。

## 5. 单设备完整时序

```
1. 串流开始，Moonlight 用户在 overlay 选「转发 DualSense」（busid "1-2"）
2. 客户端 agent：
   a. TCP 连本地 USB/IP 服务器 127.0.0.1:3240
   b. 发起 TLS 连 Sunshine（client cert + 会话 token）
   c. TLS 建立后发一行 JSON 握手：{"op":"forward","busid":"1-2"}
3. Sunshine 端口转发服务：
   a. 接受 TLS，校验 client cert + token
   b. 读握手 JSON，拿到 busid "1-2"
   c. 在 127.0.0.1 动态监听端口 <p>
   d. 触发 usbip-win2 attach 127.0.0.1:<p> --busid 1-2
   e. 接受 attach 进来的本地连接
   f. 回一行 {"op":"ready"}，之后纯字节流
4. 双方开始双向拷贝：
   agent：usbipd socket ⇄ TLS socket
   Sunshine：attach socket ⇄ TLS socket
5. 数据流贯通：usbip-win2 ⇄ Sunshine ⇄ TLS ⇄ agent ⇄ usbipd ⇄ 设备
6. 串流结束 / 用户释放 / 设备拔出 → 任一端关 socket → 整条链路级联关闭
   → Sunshine 触发 usbip-win2 detach，agent 关本地连接
```

## 6. 核心模型：USB 隧道 = 串流会话里的「第四条流」

Moonlight↔Sunshine 一次串流本来就是**多流会话**：视频（UDP）、音频（UDP）、
控制（ENet 可靠 UDP，跑输入）、HTTPS（配对/启动），共享一个经 RTSP 协商、
配对 cert 认证、NAT 穿透的逻辑会话。

**USB 隧道应建模成这个会话里的第四条流（video / audio / control / usb），而不是
外挂的独立侧信道。** 认证、端口协商、生命周期全部继承会话现有机器：

| 会话资产 | USB 隧道如何继承 |
|---|---|
| 配对 cert 身份 | 隧道 TLS 用同一 client cert，Sunshine 天然只接受已配对客户端 |
| RTSP 协商通道 | 隧道端口 + 会话 token 像视频/音频端口一样在 RTSP 握手协商下发 |
| 已转发端口段 | 隧道端口落在 Sunshine 现有 GameStream 端口范围，零新增防火墙/NAT 配置 |
| 会话生命周期 | 它是会话的一条流，串流结束随会话自动销毁 |

### 铁律：逻辑复用，物理独立

**复用会话的身份 + 协商 + 生命周期 + 端口段（逻辑层），但 USB 字节走专属 socket
（物理层）。** 绝不把 USB 字节流多路复用进 ENet 控制流：

1. **输入延迟是命根**——控制流跑手柄/鼠标/键盘。注入 USB 大流量会队头阻塞，
   抖动落在最不能抖的输入上。
2. **TCP↔ENet 阻抗失配**——usbipd 是 TCP 可靠有序流，硬桥接 ENet 等于重新引入
   framing/重排/流控，正是反向隧道要删掉的复杂度。
3. **故障隔离**——USB 隧道卡死不该拖垮视频/控制；独立 socket 才有干净失败边界。

### 四个决策点（都落在"第四条流"框架内）

- **D1 认证**：TLS 用配对 client cert（`IdentityManager` 已存，身份层）+ 会话 token
  经 RTSP 下发（会话层，绑定生命周期防重放）。比 RUSB 的 broker HTTPS capability
  简单一个量级：无独立 capability 获取、无 nonce 派生密钥。
- **D2 多设备**：每设备一条独立 TLS 隧道，不做隧道内多路复用（framing 是 RUSB 的
  复杂度来源）。外设并发数小（1–3），TLS 握手开销可忽略。一条 TCP = 一个设备的
  URB 通道，与 USB/IP 天然对齐。
- **D3 生命周期**：隧道 = 会话的一条流。建立于 overlay 点选设备；拆除于串流结束/
  用户释放/设备拔出/网络断开（任一端关 socket 级联拆除）。bind（持久化，"哪些设备
  允许转发"）与隧道（会话级，"此刻在转发"）两层分离。
- **D4 端口**：隧道端口在 RTSP 会话协商里由 Sunshine 分配下发（与视频/音频端口
  同机制），落在 GameStream 端口段内。Sunshine 据此监听 + 触发 usbip-win2 attach
  到 `127.0.0.1:<p>`（`--remote` 支持 `ip:port`），与该会话的 TLS 连接配对。

## 7. 握手协议（唯一的新协议，极简）

> **⚠ 目标态 vs 当前实现（勿混淆）**：本节及 D1/D4 描述的是**目标契约**——
> RTSP 协商下发隧道端口与一次性 token、复用配对 client cert、端口落在 GameStream
> 端口段。**当前代码尚未依赖 Sunshine 侧任何改动**：在主机侧端点落地之前，客户端
> 以 `MOONLIGHT_USB_TUNNEL_PORT`（Sunshine 侧隧道端口）与
> `MOONLIGHT_USB_TUNNEL_TOKEN`（会话 token）两个环境变量作为过渡契约，用于
> 客户端半程的开发对拍；Sunshine 落地 RTSP 协商后这两个变量即移除。实现
> Sunshine 端点时以本节契约为准，不要按环境变量行为实现。

TLS 建立后，客户端先发一行 JSON，Sunshine 回一行，之后纯字节流：

```
C → S: {"op":"forward","token":"<session-token>","busid":"1-2"}
S → C: {"op":"ready"}                          // 成功，进入字节流阶段
S → C: {"op":"error","reason":"unauthorized"}  // 失败，关闭
```

- 单行 JSON，换行分隔，无 framing、无分片、无流控（交给 TCP + TLS）。
- `busid` 是客户端 USB/IP 服务器的 busid，Sunshine 只做**字符串透传**给
  usbip-win2 attach，不解析语义。

## 8. 各侧改动清单

### 客户端（Moonlight）——新增，无需动 RUSB 栈

1. 反向隧道客户端（`usb-agent` 或 in-process）：
   - 连本地 USB/IP 服务器 3240（`QTcpSocket`）
   - 连 Sunshine（`QSslSocket`，复用 IdentityManager 的 client cert）
   - 发握手 JSON → 等 `ready` → 双向 `readyRead` 字节流拷贝
2. `Session`：overlay 点选 → 建隧道；串流结束 → 拆隧道。数据源用
   `UsbForwardingBackend` 的绑定设备（已实现）。
3. `UsbForwardingBackend`：Windows 走 usbipd-win（已实现）；Linux 加 usbip-host
   后端（`usbip bind/unbind/list`，同语义）。

### Sunshine——简化，不是加码

1. 新增「反向端口转发服务」：接受 TLS + 握手 JSON → 动态监听端口 → 触发
   usbip-win2 attach → 字节流拷贝。
2. **复用**现有 usbip-win2 attach/detach 集成代码，只把"数据来源"从 RUSB 流换成
   TLS 字节流。
3. 现有 loopback bridge（解析 USB/IP + bus id 替换）**退役**——它做的 USB/IP 理解
   在本方案里完全多余。

## 9. 与 RUSB 的对比

| 维度 | RUSB 方案 | 最简反向隧道 |
|---|---|---|
| 通用层职责 | 字节流 + **USB/IP 编解码** + adapter | 字节流 + 认证（瘦） |
| 谁理解 USB/IP | 两端都理解（多余） | **没人理解** |
| 多设备 | framing 多路复用 | 每设备一条隧道 |
| 认证 | broker HTTPS capability + nonce | TLS 双向 + 会话 token |
| 跨平台覆盖 | 各平台自实现（含 macOS） | **Windows/Linux/Android**（有现成服务器），仅 macOS 缺 |
| 新增代码量 | 大（Rust core 改造 + adapter 1000+ 行） | 小（两个字节流拷贝 + 握手） |
| 首期复杂度 | 高 | **极低** |

## 10. 开放问题与风险（诚实标注）

1. **Sunshine 侧改动无法在本仓库验证**——需 Sunshine 仓库配合落地「反向端口转发
   服务」；本仓库只能实现客户端侧并定义握手契约。
2. **usbip-win2 attach 触发**：需确认 Sunshine 现有 usbip-win2 集成暴露的
   attach/detach 接口能否指向动态端口。
3. **多客户端并发**：Sunshine 同时服务多串流时，端口转发 + attach 配对要按
   token/会话隔离（D4 已预留）。
4. **macOS 客户端**：唯一完全无 USB/IP 服务器的主要平台。若要支持，唯一路径是
   自实现 USB/IP 服务器——等于重拾 RUSB 的复杂度，需单独立项评估。iOS / HarmonyOS
   本就 USB 受限，不在范围。
5. **认证强度取舍**：TLS 双向 + 会话 token 安全性等价于"已配对客户端 + 当前
   会话"；若未来要更强的每设备授权，可在握手 JSON 加一次性 per-device token。
6. **usbipd 3240 暴露面**：agent 连本地 127.0.0.1:3240，usbipd 默认只对 localhost
   开放，不额外扩大暴露面。
7. **等时传输 / hub**：沿用 usbipd 自身能力边界（不支持等时），不引入新限制。

## 11. 决策建议

- 若目标收敛为"外设（手柄/键鼠）在 **Windows/Linux 客户端 → Windows/Linux 主机**
  之间转发"，本方案是正确形态，且让前几批的 `UsbForwardingBackend`（bind/UAC）
  与 UI/UX 原型全部保留价值。
- 代价：**退役 Moonlight 侧 RUSB/Rust core/adapter/codec 栈 + Sunshine 侧
  loopback bridge 退役**，并在 Sunshine 仓库落地反向端口转发服务。
- 前置动作：先拿 Sunshine 侧对「反向端口转发服务」的可行性确认（尤其 usbip-win2
  attach 到动态端口的接口），再决定客户端侧是否按本方案重写。
