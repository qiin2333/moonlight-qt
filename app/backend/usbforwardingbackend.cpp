#include "usbforwardingbackend.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>

#ifdef Q_OS_WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

// 从 Windows 实例 ID（USB\VID_054C&PID_0CE6\...）解析出 "054c:0ce6"。
QString vidPidFromInstanceId(const QString &instanceId)
{
    static const QRegularExpression vidRe(QStringLiteral("VID_([0-9A-Fa-f]{4})"));
    static const QRegularExpression pidRe(QStringLiteral("PID_([0-9A-Fa-f]{4})"));
    const QRegularExpressionMatch vidMatch = vidRe.match(instanceId);
    const QRegularExpressionMatch pidMatch = pidRe.match(instanceId);
    if (!vidMatch.hasMatch() || !pidMatch.hasMatch()) {
        return QString();
    }
    return vidMatch.captured(1).toLower() + QLatin1Char(':')
        + pidMatch.captured(1).toLower();
}

// "1-2" 这类真实 busid 才可绑定；"IncompatibleHub" 与空串都不可。
bool isRealBusId(const QString &busId)
{
    static const QRegularExpression realRe(
        QStringLiteral("^[1-9][0-9]*-[1-9][0-9]*$"));
    return realRe.match(busId).hasMatch();
}

} // namespace

UsbForwardingBackend::UsbForwardingBackend(QObject *parent)
    : QObject(parent)
{
}

UsbForwardingBackend* UsbForwardingBackend::get()
{
    static UsbForwardingBackend backend;
    return &backend;
}

QString UsbForwardingBackend::locateUsbipd() const
{
    QString exe = QStandardPaths::findExecutable(QStringLiteral("usbipd"));
    if (exe.isEmpty()) {
        const QString bundledPath =
            QStringLiteral("C:/Program Files/usbipd-win/usbipd.exe");
        if (QFileInfo::exists(bundledPath)) {
            exe = bundledPath;
        }
    }
    return exe;
}

void UsbForwardingBackend::setBusy(bool busy)
{
    if (m_Busy == busy) {
        return;
    }
    m_Busy = busy;
    emit busyChanged();
}

void UsbForwardingBackend::setError(const QString &error)
{
    if (m_Error == error) {
        return;
    }
    m_Error = error;
    emit errorChanged();
}

void UsbForwardingBackend::refresh()
{
    if (m_Busy) {
        return;
    }
    const QString exe = locateUsbipd();
    if (exe.isEmpty()) {
        m_Devices.clear();
        emit devicesChanged();
        setError(tr("usbipd-win is not installed."));
        return;
    }
    setBusy(true);
    setError(QString());

    QProcess *probe = new QProcess(this);
    /* FailedToStart emits errorOccurred but never finished; without this the
     * busy flag would stick and the device list would go stale. */
    connect(probe, &QProcess::errorOccurred, this,
            [this, probe](QProcess::ProcessError processError) {
        if (processError != QProcess::FailedToStart) {
            return;
        }
        probe->deleteLater();
        setBusy(false);
        m_Devices.clear();
        emit devicesChanged();
        setError(tr("usbipd could not be started. Requires usbipd-win 2.2.0+."));
    });
    connect(probe, &QProcess::finished, this, [this, probe](int exitCode) {
        probe->deleteLater();
        setBusy(false);

        if (exitCode != 0) {
            const QString detail =
                QString::fromLocal8Bit(probe->readAllStandardError()).simplified();
            m_Devices.clear();
            emit devicesChanged();
            setError(detail.isEmpty()
                         ? tr("usbipd state failed (exit %1). Requires usbipd-win 2.2.0+.")
                               .arg(exitCode)
                         : detail);
            return;
        }

        const QByteArray output = probe->readAllStandardOutput();
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(output, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            m_Devices.clear();
            emit devicesChanged();
            setError(tr("Could not parse usbipd state output."));
            return;
        }

        QVariantList devices;
        const QJsonArray array = doc.object().value(QLatin1String("Devices")).toArray();
        for (const QJsonValue &value : array) {
            const QJsonObject o = value.toObject();
            const QString busId = o.value(QLatin1String("BusId")).toString();
            const QString clientIp =
                o.value(QLatin1String("ClientIPAddress")).toString();
            const QString description =
                o.value(QLatin1String("Description")).toString();
            const QString instanceId =
                o.value(QLatin1String("InstanceId")).toString();
            const bool isForced = o.value(QLatin1String("IsForced")).toBool();
            const QString persistedGuid =
                o.value(QLatin1String("PersistedGuid")).toString();

            const bool isBound = !persistedGuid.isEmpty();
            const bool isConnected = !busId.isEmpty();
            const bool isAttached = !clientIp.isEmpty();
            const bool supported = isConnected && isRealBusId(busId);

            QVariantMap device;
            device.insert(QStringLiteral("busId"), busId);
            device.insert(QStringLiteral("description"), description);
            device.insert(QStringLiteral("instanceId"), instanceId);
            device.insert(QStringLiteral("vidPid"), vidPidFromInstanceId(instanceId));
            device.insert(QStringLiteral("isBound"), isBound);
            device.insert(QStringLiteral("isConnected"), isConnected);
            device.insert(QStringLiteral("isAttached"), isAttached);
            device.insert(QStringLiteral("isSupported"), supported);
            device.insert(QStringLiteral("isForced"), isForced);
            device.insert(QStringLiteral("persistedGuid"), persistedGuid);
            devices.append(device);
        }

        m_Devices = devices;
        emit devicesChanged();
    });
    QTimer::singleShot(8000, probe, &QProcess::kill);
    probe->start(exe, {QStringLiteral("state")});
}

void UsbForwardingBackend::bind(const QString &busId)
{
    if (busId.isEmpty()) {
        return;
    }
    const QString exe = locateUsbipd();
    if (exe.isEmpty()) {
        emit operationFinished(false, tr("usbipd-win is not installed."));
        return;
    }
#ifdef Q_OS_WIN32
    const QString params = QStringLiteral("bind --busid \"%1\"").arg(busId);
    const HINSTANCE result = ShellExecuteW(
        nullptr, L"runas",
        reinterpret_cast<const wchar_t*>(exe.utf16()),
        reinterpret_cast<const wchar_t*>(params.utf16()),
        nullptr, SW_HIDE);
    const bool ok = reinterpret_cast<INT_PTR>(result) > 32;
#else
    const bool ok = false;
#endif
    if (ok) {
        emit operationFinished(true, tr("Requested sharing. Refreshing device list…"));
        QTimer::singleShot(1500, this, [this] { refresh(); });
    } else {
        emit operationFinished(false, tr("The elevation was cancelled or failed."));
    }
}

void UsbForwardingBackend::unbind(const QString &busId, const QString &persistedGuid)
{
    QStringList args;
    if (!busId.isEmpty() && isRealBusId(busId)) {
        args << QStringLiteral("unbind") << QStringLiteral("--busid") << busId;
    } else if (!persistedGuid.isEmpty()) {
        args << QStringLiteral("unbind") << QStringLiteral("--guid") << persistedGuid;
    } else {
        return;
    }
    const QString exe = locateUsbipd();
    if (exe.isEmpty()) {
        emit operationFinished(false, tr("usbipd-win is not installed."));
        return;
    }
#ifdef Q_OS_WIN32
    QString params;
    for (const QString &arg : args) {
        params += QLatin1Char('"') + arg + QLatin1String("\" ");
    }
    const HINSTANCE result = ShellExecuteW(
        nullptr, L"runas",
        reinterpret_cast<const wchar_t*>(exe.utf16()),
        reinterpret_cast<const wchar_t*>(params.utf16()),
        nullptr, SW_HIDE);
    const bool ok = reinterpret_cast<INT_PTR>(result) > 32;
#else
    const bool ok = false;
#endif
    if (ok) {
        emit operationFinished(true, tr("Requested stop sharing. Refreshing device list…"));
        QTimer::singleShot(1500, this, [this] { refresh(); });
    } else {
        emit operationFinished(false, tr("The elevation was cancelled or failed."));
    }
}
