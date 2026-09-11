#include "usbforwardingbackend.h"
#include "usbforwardingenvironment.h"

#include "usbforwardinglocalserver.h"

#ifdef Q_OS_DARWIN
// 仅 macOS 的绑定偏好持久化用到；不无条件引入，让 tests/ 无需 Qt Qml。
#include "settings/streamingpreferences.h"
#endif

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QTimer>

#ifdef Q_OS_WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

#ifndef Q_OS_DARWIN
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
#endif

// macOS helper 的 busid 是 libusb 拓扑路径："1-2"，经 hub 时 "1-2.3"。
// 必须与 usbipdcpp 的 find_by_busid 生成算法一致（bus + "-" + 端口链 "." 连接）。
bool isMacBusId(const QString &busId)
{
    static const QRegularExpression macRe(
        QStringLiteral("^[1-9][0-9]*-[0-9]+(\\.[0-9]+)*$"));
    return macRe.match(busId).hasMatch();
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

QVariantList UsbForwardingBackend::parseHelperDevices(const QByteArray &helperJson,
                                                     QString *error)
{
    if (error) {
        error->clear();
    }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(helperJson, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isArray()) {
        if (error) {
            *error = tr("Could not parse the USB device list.");
        }
        return {};
    }

    QVariantList devices;
    for (const QJsonValue &value : doc.array()) {
        const QJsonObject o = value.toObject();
        const QString busId = o.value(QLatin1String("busId")).toString();
        const QString vidPid = o.value(QLatin1String("vidPid")).toString().toLower();
        const QString product = o.value(QLatin1String("product")).toString();
        const QString manufacturer =
                o.value(QLatin1String("manufacturer")).toString();
        const bool claimable = o.value(QLatin1String("claimable")).toBool();

        QString description = !product.isEmpty() ? product : manufacturer;
        if (description.isEmpty()) {
            description = vidPid;
        }

        const bool occupied = !claimable;

        QVariantMap device;
        device.insert(QStringLiteral("busId"), busId);
        device.insert(QStringLiteral("description"), description);
        device.insert(QStringLiteral("instanceId"),
                      o.value(QLatin1String("serial")).toString());
        device.insert(QStringLiteral("vidPid"), vidPid);
        // isBound 恒 false，由 refreshFromHelper() 按用户偏好叠加。
        device.insert(QStringLiteral("isBound"), false);
        // 出现在枚举输出里即已连接。
        device.insert(QStringLiteral("isConnected"), true);
        device.insert(QStringLiteral("isAttached"), false);
        device.insert(QStringLiteral("isSupported"), isMacBusId(busId) && claimable);
        device.insert(QStringLiteral("isForced"), false);
        device.insert(QStringLiteral("persistedGuid"), QString());
        device.insert(QStringLiteral("isOccupied"), occupied);
        devices.append(device);
    }

    return devices;
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
#ifdef Q_OS_DARWIN
    refreshFromHelper();
#else
    const QString exe = UsbForwardingEnvironment::locateUsbipd();
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
#endif
}

#ifdef Q_OS_DARWIN

void UsbForwardingBackend::refreshFromHelper()
{
    const QString helper = UsbForwardingLocalServer::locateHelper();
    if (helper.isEmpty()) {
        m_Devices.clear();
        emit devicesChanged();
        setError(tr("The bundled USB sharing component is missing. Reinstall Moonlight."));
        return;
    }
    setBusy(true);
    setError(QString());

    QProcess *probe = new QProcess(this);
    /* 与 Windows 分支同理：FailedToStart 只发 errorOccurred 不发 finished。 */
    connect(probe, &QProcess::errorOccurred, this,
            [this, probe](QProcess::ProcessError processError) {
        if (processError != QProcess::FailedToStart) {
            return;
        }
        probe->deleteLater();
        setBusy(false);
        m_Devices.clear();
        emit devicesChanged();
        setError(tr("The USB helper could not be started."));
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
                         ? tr("The USB device list failed (exit %1).").arg(exitCode)
                         : detail);
            return;
        }

        QString parseError;
        QVariantList devices =
                parseHelperDevices(probe->readAllStandardOutput(), &parseError);
        if (!parseError.isEmpty()) {
            m_Devices.clear();
            emit devicesChanged();
            setError(parseError);
            return;
        }

        const QStringList bound =
                StreamingPreferences::get()->usbForwardingBoundDevices();
        for (QVariant &value : devices) {
            QVariantMap device = value.toMap();
            device.insert(QStringLiteral("isBound"),
                          bound.contains(device.value(QStringLiteral("busId")).toString()));
            value = device;
        }

        m_Devices = devices;
        emit devicesChanged();
    });
    QTimer::singleShot(8000, probe, &QProcess::kill);
    probe->start(helper, {QStringLiteral("list"), QStringLiteral("--json")});
}

#endif

void UsbForwardingBackend::bind(const QString &busId)
{
    if (busId.isEmpty()) {
        return;
    }
#ifdef Q_OS_DARWIN
    // macOS 上绑定只是记偏好：serve 进程由 Session 在转发时按需拉起，
    // 因此无需提权，立即生效（正在转发的会话不受影响，下次选择时生效）。
    StreamingPreferences *prefs = StreamingPreferences::get();
    QStringList bound = prefs->usbForwardingBoundDevices();
    if (!bound.contains(busId)) {
        bound.append(busId);
        prefs->setUsbForwardingBoundDevices(bound);
        prefs->save();
    }
    emit operationFinished(true, tr("Requested sharing. Refreshing device list…"));
    QTimer::singleShot(300, this, [this] { refresh(); });
#else
    const QString exe = UsbForwardingEnvironment::locateUsbipd();
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
#endif
}

void UsbForwardingBackend::unbind(const QString &busId, const QString &persistedGuid)
{
    if (busId.isEmpty() && persistedGuid.isEmpty()) {
        return;
    }
#ifdef Q_OS_DARWIN
    // macOS：从偏好列表移除即可；persistedGuid 是 Windows 概念，忽略。
    StreamingPreferences *prefs = StreamingPreferences::get();
    QStringList bound = prefs->usbForwardingBoundDevices();
    if (bound.removeAll(busId) > 0) {
        prefs->setUsbForwardingBoundDevices(bound);
        prefs->save();
    }
    emit operationFinished(true, tr("Requested stop sharing. Refreshing device list…"));
    QTimer::singleShot(300, this, [this] { refresh(); });
#else
    QStringList args;
    if (!busId.isEmpty() && isRealBusId(busId)) {
        args << QStringLiteral("unbind") << QStringLiteral("--busid") << busId;
    } else if (!persistedGuid.isEmpty()) {
        args << QStringLiteral("unbind") << QStringLiteral("--guid") << persistedGuid;
    } else {
        return;
    }
    const QString exe = UsbForwardingEnvironment::locateUsbipd();
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
#endif
}
