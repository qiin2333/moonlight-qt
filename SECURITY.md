# BuffPlum Fork Security Notice / 安全说明

## 中文

这是由 BuffPlum 独立维护的非官方 Moonlight 客户端。Moonlight、Foundation Sunshine 以及相关上游项目不为本 Fork 的专属功能提供支持。

### 全盘文件传输风险

- 配套的 BuffPlum Foundation Sunshine 会向已配对客户端列出 Sunshine 进程能够访问的全部磁盘。
- 文件传输连接沿用配对身份、HTTPS 能力接口和短期会话令牌，但这不能替代可信网络边界。
- 仅应在个人设备和可信局域网中使用。不要把 Sunshine 或文件传输端口直接暴露到公网。
- Sunshine 应使用完成串流所需的最低权限运行。管理员或系统服务权限会扩大可访问文件范围。
- 默认拒绝覆盖同名文件，但这不是备份方案。启用功能前应备份重要数据。
- GitHub Actions 生成的安装包目前不包含商业代码签名。请仅从 [BuffPlum/moonlight-qt Releases](https://github.com/BuffPlum/moonlight-qt/releases) 下载并核对校验值。

Fork 专属安全问题请提交到 [BuffPlum/moonlight-qt Issues](https://github.com/BuffPlum/moonlight-qt/issues)，不要发送密码、令牌、证书、个人文件或完整日志中的敏感信息。

## English

This is an unofficial Moonlight client independently maintained by BuffPlum. The Moonlight and Foundation Sunshine upstream projects do not support fork-specific features.

The companion BuffPlum Foundation Sunshine host exposes every drive readable by the Sunshine process to an authenticated paired client. Use the feature only between personal devices on a trusted LAN, never expose its ports directly to the public Internet, run Sunshine with the least privileges needed, and keep independent backups. GitHub Actions packages are currently unsigned; download them only from the BuffPlum release page and verify the published checksums.

Report fork-specific security problems through [BuffPlum/moonlight-qt Issues](https://github.com/BuffPlum/moonlight-qt/issues) without attaching credentials, certificates, personal files, or unredacted sensitive logs.
