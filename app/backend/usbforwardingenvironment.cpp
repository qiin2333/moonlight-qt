#include "usbforwardingenvironment.h"

#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

#ifdef Q_OS_WIN32
#include <windows.h>
#endif

UsbForwardingEnvironment::UsbForwardingEnvironment(QObject *parent)
    : QObject(parent)
{
}

UsbForwardingEnvironment* UsbForwardingEnvironment::get()
{
    static UsbForwardingEnvironment environment;
    return &environment;
}

QString UsbForwardingEnvironment::locateUsbipd()
{
    QString usbipdExe = QStandardPaths::findExecutable(QStringLiteral("usbipd"));
    if (usbipdExe.isEmpty()) {
        const QString bundledPath =
            QStringLiteral("C:/Program Files/usbipd-win/usbipd.exe");
        if (QFileInfo::exists(bundledPath)) {
            usbipdExe = bundledPath;
        }
    }
    return usbipdExe;
}

void UsbForwardingEnvironment::refresh()
{
    if (m_Checking) {
        return;
    }
#ifdef Q_OS_WIN32
    const QString usbipdExe = locateUsbipd();
    if (usbipdExe.isEmpty()) {
        m_Version.clear();
        finish(NotInstalled);
        return;
    }
    m_Checking = true;
    emit checkingChanged();
    m_State = Checking;
    emit stateChanged();
    startVersionProbe(usbipdExe);
#else
    m_Version.clear();
    finish(NotInstalled);
#endif
}

void UsbForwardingEnvironment::startVersionProbe(const QString &usbipdExe)
{
    QProcess *probe = new QProcess(this);
    /* FailedToStart emits errorOccurred but never finished; handle it so a
     * missing executable cannot wedge the probe. */
    connect(probe, &QProcess::errorOccurred, this,
            [this, probe](QProcess::ProcessError processError) {
        if (processError != QProcess::FailedToStart) {
            return;
        }
        probe->deleteLater();
        finish(ServiceStopped);
    });
    connect(probe, &QProcess::finished, this, [this, probe](int exitCode) {
        probe->deleteLater();
        if (exitCode != 0) {
            finish(ServiceStopped);
            return;
        }
        const QString output =
            QString::fromLocal8Bit(probe->readAllStandardOutput());
        const QString firstLine = output.section(QLatin1Char('\n'), 0, 0).simplified();
        // "usbipd-win 4.2.0" -> "4.2.0"
        QString version = firstLine;
        if (version.startsWith(QLatin1String("usbipd-win"), Qt::CaseInsensitive)) {
            version.remove(0, 10);
        }
        version = version.trimmed();
        m_Version = version;
        startServiceProbe();
    });
    QTimer::singleShot(8000, probe, &QProcess::kill);
    probe->start(usbipdExe, {QStringLiteral("--version")});
}

void UsbForwardingEnvironment::startServiceProbe()
{
    finish(probeServices());
}

UsbForwardingEnvironment::State UsbForwardingEnvironment::probeServices()
{
#ifdef Q_OS_WIN32
    const SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) return CheckFailed;
    State result = Ready;
    const struct { const wchar_t* name; State stopped; } services[] = {
        {L"usbipd", ServiceStopped}, {L"VBoxUSBMon", DriverStopped}
    };
    for (const auto& entry : services) {
        const SC_HANDLE service = OpenServiceW(manager, entry.name, SERVICE_QUERY_STATUS);
        if (!service) {
            result = CheckFailed;
            break;
        }
        SERVICE_STATUS status {};
        const bool queried = QueryServiceStatus(service, &status) != FALSE;
        CloseServiceHandle(service);
        if (!queried || status.dwCurrentState != SERVICE_RUNNING) {
            result = queried ? entry.stopped : CheckFailed;
            break;
        }
    }
    CloseServiceHandle(manager);
    return result;
#else
    return NotInstalled;
#endif
}

QString UsbForwardingEnvironment::readinessError(State state)
{
    switch (state) {
    case Ready: return {};
    case DriverStopped:
        return tr("The USB forwarding driver is not running. Start VBoxUSBMon as administrator, or restart Windows.");
    case ServiceStopped:
        return tr("The usbipd service is not running. Start the service and retry.");
    default:
        return tr("Could not verify the local USB service and driver. Check the usbipd-win installation.");
    }
}

void UsbForwardingEnvironment::finish(State state)
{
    m_State = state;
    m_Checking = false;
    emit stateChanged();
    emit checkingChanged();
}
