# Moonlight 全盘文件传输 / Full-disk File Transfer

This fork provides a two-pane file manager for Foundation Sunshine. It automatically lists all accessible drives on both computers and does not require shared-folder configuration.

## 使用方法

1. 使用配套 Foundation Sunshine 完成配对并开始串流。
2. 按 `Ctrl+Alt+Shift+O`，选择 **文件传输**。
3. 左侧是运行 Moonlight 的本机，右侧是 Sunshine 主机。
4. 双击磁盘或目录进入，单击选择一个文件或文件夹。
5. 点击 **上传 →** 将左侧项目发送到右侧当前目录；点击 **← 下载** 将右侧项目下载到左侧当前目录。
6. 也可以把左侧文件或文件夹拖到右侧目录上传，或把右侧项目拖到左侧目录下载。
7. 支持从 Windows 资源管理器把文件或文件夹直接拖进右侧远程目录上传。
8. 鼠标滚轮滚动列表，**↑ 上一级** 返回上级，`F5` 或 **刷新** 刷新两侧。

文件传输窗口使用类似 Windows 资源管理器的文件类型图标、名称/大小列和中文状态提示。文件与文件夹均可双向递归传输，传输在后台线程运行，并在底部显示进度。

## 规则与限制

- 主机磁盘由 Sunshine 动态枚举，通常显示为 `C:`、`D:`、`E:` 等。
- 不覆盖任何同名文件或目录；请先改名或选择其他目标目录。
- 上传使用 256 KiB JSON/Base64 分块，主机端使用临时文件完成后原子提交。
- 下载先写客户端临时文件，完整接收后才提交。
- 不传输符号链接，不提供删除、移动、重命名或执行。
- Sunshine/Moonlight 进程自身没有权限访问的目录也无法传输。
- 文件窗口依赖正在进行的已配对串流会话，因为令牌是短期且绑定客户端身份的。
