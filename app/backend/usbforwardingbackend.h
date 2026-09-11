#pragma once

#include <QObject>
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

    QVariantList m_Devices;
    bool m_Busy = false;
    QString m_Error;
};
