#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>

// usbipd-win 编排层：枚举设备（usbipd state 的 JSON 输出）、bind/unbind
// （需管理员，通过 ShellExecuteW "runas" 触发 UAC）。与 UsbForwardingEnvironment
// 的分工：后者只做环境体检（版本 + 服务状态），这里做真正的设备编排。
//
// 设备列表每一项是 QVariantMap，键：
//   busId         "1-2" / "IncompatibleHub" / ""（未连接）
//   description   设备描述名
//   instanceId    Windows 实例 ID（USB\VID_XXXX&PID_YYYY\...）
//   vidPid        "xxxx:yyyy"（从 instanceId 解析，失败为空）
//   isBound       bool，PersistedGuid 非空
//   isConnected   bool，BusId 非空
//   isAttached    bool，ClientIPAddress 非空（正在被某客户端使用）
//   isSupported   bool，可绑定（已连接且不在不兼容 hub 上）
//   isForced      bool
//   persistedGuid 已持久化的共享 GUID（未绑定时为空）
class UsbForwardingBackend : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList devices READ devices NOTIFY devicesChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)

public:
    static UsbForwardingBackend* get();

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

    QString locateUsbipd() const;
    void setBusy(bool busy);
    void setError(const QString &error);

    QVariantList m_Devices;
    bool m_Busy = false;
    QString m_Error;
};
