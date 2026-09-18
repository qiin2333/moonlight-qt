# 目录导览(Architecture)

> 回答一个问题:每个目录是谁的、干什么的。上游同构的部分**不要随意改名/移动**
> ——那会破坏与 moonlight-stream/moonlight-qt 的 merge 能力(见 [upstream-sync.md](upstream-sync.md))。

## 上游继承(与上游同构,merge 面)

| 目录/文件 | 用途 |
|---|---|
| `app/` | 客户端主体:GUI(QML)、streaming 会话、backend、settings、输入、渲染器。fork 在此有大量增量(见下) |
| `moonlight-common-c/` | 协议库 submodule,**指向我们自己的 fork**(qiin2333/moonlight-common-c),其 `mic` 分支是实际消费线;上游同步走 PR |
| `qmdnsengine/` `h264bitstream/` `third-party/AMF` | vendored / submodule 依赖 |
| `AntiHooking/` | Windows 防 DLL 注入库(上游) |
| `config.tests/` | qmake 特性探测(SL=SteamLink SDK、EGL) |
| `scripts/` | 构建与打包脚本(部分上游、部分 fork);fork 的本地工具脚本也放这里 |
| `wix/` `setup-deps.ps1/.py` | Windows 安装包与预编译依赖拉取 |
| `moonlight-qt.pro` `globaldefs.pri` | 顶层构建:SUBDIRS 注册 + 公共配置 |

## fork 新增(上游不存在,零冲突)

| 目录/文件 | 用途 |
|---|---|
| `clipboard-helper/` | 剪贴板同步 helper(独立进程) |
| `usb-helper/` | macOS USB/IP 后端 helper(moonlight-usbd,C++23 + CMake,内嵌自己的 third_party 子模块) |
| `file-mapping/` | 主机文件共享:mount / protocol / vfs 三层(`FileMapping` 命名空间);`smoke/` 是手工冒烟二进制(不在 CI) |
| `tests/` | 测试目录,CI 覆盖矩阵与约定见 [../tests/README.md](../tests/README.md) |
| `docs/` | 设计文档与流程 SOP(upstream-sync、architecture 等) |
| `projects/` | 业务项目材料(**仅本地,不入库**,见 CONTRIBUTING.md) |
| `scripts/run-test.bat` | 测试统一入口(CI 与本地共用) |

## 约定

- **新增顶层模块默认 qmake** 并注册进 `moonlight-qt.pro` 的 SUBDIRS;仅当「上游不存在 + 依赖 CMake-only + 纯新增目录」三条件同时满足才允许 CMake(现例:usb-helper)。
- 语言标准:C++17 为 app 地板(SteamLink 工具链上限),usb-helper 为 C++23 孤岛,不再引入第三种。
- 依赖钉值:Linux 源码依赖在 build.yml;Win/Mac 预编译组在 setup-deps(v-tag);升级要与上游对齐(见 upstream-sync.md 的依赖双轨一节)。
- 本地产物(build/、libs/、IDE 目录、.DS_Store)一律 gitignore,不入库。

## 历史

- 2026-09 目录清理:移除空壳目录(src/、streaming/、include/、soundio/ 上游遗留)与误跟踪的 .DS_Store;filemapping-smoke 并入 file-mapping/smoke;DS5 探针脚本并入 scripts/。
