#pragma once

#include <QObject>
#include <QMap>
#include <QString>
#include <QVariantList>

// USB 设备共享编排层。平台各一支，QML 契约一致：
//  - Windows：外挂 usbipd-win。枚举设备（usbipd state 的 JSON 输出）、
//    bind/unbind（需管理员，通过 ShellExecuteW "runas" 触发 UAC）。绑定状态
//    持久化在 usbipd 自己的注册表里（PersistedGuid）。
//  - macOS：捆绑的 moonlight-usbd helper（usbipdcpp）。枚举走 `list --json`
//    （parseHelperDevices 解析），bind/unbind 只是改 Moonlight 自己的偏好
//    （StreamingPreferences::usbForwardingBoundDevices），无提权。转发用的
//    serve 进程由 Session 经 UsbForwardingLocalServer 按需拉起。
//  - Linux：标准 usbip-host 栈。枚举直读 sysfs（parseSysfsDevices），
//    bind/unbind 经 pkexec 走固定 helper + app 自己的 polkit action
//    （首次共享时一次性安装），绑定状态就是内核 driver 绑定，如实反映。
// 与 UsbForwardingEnvironment 的分工：后者只做环境体检（版本 + 服务状态），
// 这里做真正的设备编排。
//
// 设备列表每一项是 QVariantMap，键：
//   busId         Windows "1-2" / "IncompatibleHub" / ""（未连接）；macOS 拓扑
//                 路径 "1-2" / "1-2.3"（经 hub 时带点）
//   description   设备描述名
//   instanceId    Windows 实例 ID（USB\VID_XXXX&PID_YYYY\...）；macOS 为序列号
//   vidPid        "xxxx:yyyy"
//   isBound       bool（Windows：PersistedGuid 非空；macOS：在偏好列表中）
//   isConnected   bool，当前在枚举输出里
//   isAttached    bool，正在被某客户端使用（macOS 恒 false）
//   isSupported   bool，可共享（已连接、busid 合法；macOS 还要求未被系统占用）
//   isForced      bool（仅 Windows）
//   persistedGuid 已持久化的共享 GUID（仅 Windows；macOS 恒空）
//   isOccupied    bool（仅 macOS）：接口被 macOS 系统驱动占用，无法共享
class UsbForwardingBackend : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList devices READ devices NOTIFY devicesChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)

public:
    static UsbForwardingBackend* get();

    // 解析 moonlight-usbd `list --json` 的输出（macOS 枚举数据源）。
    // 返回与 refresh() 相同 schema 的 QVariantMap 列表（isBound 恒 false，
    // 由调用方按偏好叠加）。失败时置 *error 并返回空表。公开成静态纯函数
    // 以便脱离进程做单元测试（tests/usb_forwarding_backend_list）。
    static QVariantList parseHelperDevices(const QByteArray& helperJson, QString* error);

    // 枚举 <sysfsBusPath>（Linux 上通常 /sys/bus/usb/devices）下的 USB 设备。
    // Linux 枚举数据源：sysfs 属性文件 + driver symlink 判绑定。
    // 跳过根 hub（usbN）与 hub 设备（bDeviceClass 09）。函数本身是纯 Qt 文件
    // 访问、平台无关（Windows 测试用夹具目录驱动），不做平台门控。
    static QVariantList parseSysfsDevices(const QString &sysfsBusPath, QString *error);

    // 设备身份 = vidPid + 序列号，跨重插稳定；用于识别「共享后端口上换成
    // 另一台设备」——usbip 按 busid 寻址，内核 match_busid 只认端口不认
    // 设备，重插后换上的不同设备会被 usbip-host 静默认领。绑定快照由
    // 特权 helper 在 bind 时写入 /var/lib/moonlight-qt/bindings。
    static QString deviceIdentity(const QString &vidPid, const QString &serial);
    static QMap<QString, QString> parseBindIdentities(const QByteArray &bindingsFile);
    static void markReplacedDevices(QVariantList &devices, const QMap<QString, QString> &bindings);

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void bind(const QString &busId);
    Q_INVOKABLE void unbind(const QString &busId, const QString &persistedGuid);

    QVariantList devices() const { return m_Devices; }
    bool busy() const { return m_Busy; }
    QString error() const { return m_Error; }

signals:
    void devicesChanged();
    void busyChanged();
    void errorChanged();
    void operationFinished(bool success, const QString &message);

private:
    explicit UsbForwardingBackend(QObject *parent = nullptr);

    void setBusy(bool busy);
    void setError(const QString &error);
#ifdef Q_OS_DARWIN
    void refreshFromHelper();
#endif
#ifdef Q_OS_LINUX
    // 首次共享时把内置 helper 脚本与 polkit policy 以一次提权安装到位，
    // 之后 bind/unbind 走 app 自己的 auth_admin_keep action。
    // bind 额外携带用户点选时的设备身份（deviceIdentity 结果），helper
    // 在 root 下重读 sysfs 比对，不符拒绝绑定（TOCTOU 窗口从授权等待的
    // 秒级压到 root 内的微秒级）。
    void runPrivileged(const QString &action, const QString &busId,
                       const QString &expectedIdentity = QString());
    bool installPrivilegedHelper(const QString &action, const QString &busId,
                                 const QString &expectedIdentity = QString());
    void runHelperAction(const QString &action, const QString &busId,
                         const QString &expectedIdentity = QString());
    // 从当前枚举结果查 busId 的设备身份；找不到（列表过期）返回空串，
    // 调用方以此拒绝发起 bind，强制用户刷新列表。
    QString expectedIdentityFor(const QString &busId) const;
#endif

    QVariantList m_Devices;
    bool m_Busy = false;
    QString m_Error;
};
