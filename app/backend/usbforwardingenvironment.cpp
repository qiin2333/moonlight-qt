#include "usbforwardingenvironment.h"

#include "usbforwardinglocalserver.h"

#include <QFileInfo>
#include <QHostAddress>
#include <QProcess>
#include <QStandardPaths>
#include <QTcpSocket>
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
#elif defined(Q_OS_DARWIN)
    // macOS has no system USB/IP service; the server (moonlight-usbd, built
    // on usbipdcpp) ships inside the app bundle and is spawned per session.
    const QString helperPath = UsbForwardingLocalServer::locateHelper();
    if (helperPath.isEmpty()) {
        m_Version.clear();
        finish(NotInstalled);
        return;
    }
    m_Checking = true;
    emit checkingChanged();
    m_State = Checking;
    emit stateChanged();
    startHelperVersionProbe(helperPath);
#elif defined(Q_OS_LINUX)
    // Linux wraps the standard usbip-host stack: the usbip userspace tool
    // plus the root usbipd daemon on 3240. Module loading and daemon
    // lifecycle happen through the privileged helper when a device is
    // shared; the readiness probe is read-only.
    const QString usbipExe = QStandardPaths::findExecutable(QStringLiteral("usbip"));
    if (usbipExe.isEmpty()) {
        m_Version.clear();
        finish(NotInstalled);
        return;
    }
    m_Checking = true;
    emit checkingChanged();
    m_State = Checking;
    emit stateChanged();
    startLinuxVersionProbe(usbipExe);
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

void UsbForwardingEnvironment::startHelperVersionProbe(const QString &helperPath)
{
    QProcess *probe = new QProcess(this);
    connect(probe, &QProcess::errorOccurred, this,
            [this, probe](QProcess::ProcessError processError) {
        if (processError != QProcess::FailedToStart) {
            return;
        }
        probe->deleteLater();
        finish(CheckFailed);
    });
    connect(probe, &QProcess::finished, this, [this, probe](int exitCode) {
        probe->deleteLater();
        if (exitCode != 0) {
            finish(CheckFailed);
            return;
        }
        const QString output =
                QString::fromLocal8Bit(probe->readAllStandardOutput());
        const QString firstLine = output.section(QLatin1Char('\n'), 0, 0).simplified();
        // "moonlight-usbd 1.0.0 (usbipdcpp v1.0.9)" -> "usbipdcpp v1.0.9"
        QString version = firstLine;
        const int libraryIndex = version.indexOf(QLatin1String("usbipdcpp"));
        if (libraryIndex >= 0) {
            version = version.mid(libraryIndex);
        }
        else if (version.startsWith(QLatin1String("moonlight-usbd"), Qt::CaseInsensitive)) {
            version.remove(0, 14);
        }
        m_Version = version.trimmed();
        // No service or driver concepts on macOS; presence of the helper is
        // the whole readiness check.
        finish(Ready);
    });
    QTimer::singleShot(8000, probe, &QProcess::kill);
    probe->start(helperPath, {QStringLiteral("--version")});
}

void UsbForwardingEnvironment::startLinuxVersionProbe(const QString &usbipExe)
{
    QProcess *probe = new QProcess(this);
    connect(probe, &QProcess::errorOccurred, this,
            [this, probe](QProcess::ProcessError processError) {
                if (processError != QProcess::FailedToStart) {
                    return;
                }
                probe->deleteLater();
                finish(CheckFailed);
            });
    connect(probe, &QProcess::finished, this, [this, probe](int exitCode) {
        probe->deleteLater();
        if (exitCode != 0) {
            finish(CheckFailed);
            return;
        }
        const QString output = QString::fromLocal8Bit(probe->readAllStandardOutput());
        const QString firstLine = output.section(QLatin1Char('\n'), 0, 0).simplified();
        // "usbip (usbip-utils 2.0)" -> "usbip-utils 2.0"
        QString version = firstLine;
        const int openIndex = version.indexOf(QLatin1Char('('));
        const int closeIndex = version.lastIndexOf(QLatin1Char(')'));
        if (openIndex >= 0 && closeIndex > openIndex) {
            version = version.mid(openIndex + 1, closeIndex - openIndex - 1);
        }
        m_Version = version.trimmed();
        startServiceProbe();
    });
    QTimer::singleShot(8000, probe, &QProcess::kill);
    probe->start(usbipExe, { QStringLiteral("version") });
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
#elif defined(Q_OS_DARWIN)
    // Called synchronously from the session worker: never spawn a process
    // here, just check that the bundled helper is present.
    return UsbForwardingLocalServer::locateHelper().isEmpty() ? NotInstalled : Ready;
#elif defined(Q_OS_LINUX)
    // Called synchronously from the session worker and from refresh():
    // only stat() and one bounded loopback connect, never a process spawn.
    if (QStandardPaths::findExecutable(QStringLiteral("usbip")).isEmpty()) {
        return NotInstalled;
    }
    if (!QFileInfo::exists(QStringLiteral("/sys/module/usbip_host"))) {
        return DriverStopped;
    }
    QTcpSocket probe;
    probe.connectToHost(QHostAddress(QHostAddress::LocalHost), 3240);
    const bool reachable = probe.waitForConnected(300);
    probe.abort();
    return reachable ? Ready : ServiceStopped;
#else
    return NotInstalled;
#endif
}

QString UsbForwardingEnvironment::readinessError(State state)
{
    switch (state) {
    case Ready: return {};
    case DriverStopped:
#ifdef Q_OS_LINUX
        return tr("The USB/IP kernel module (usbip_host) is not loaded. Moonlight loads it "
                  "automatically when you share a device.");
#else
        return tr("The USB forwarding driver is not running. Start VBoxUSBMon as administrator, or restart Windows.");
#endif
    case ServiceStopped:
#ifdef Q_OS_LINUX
        return tr("The usbipd daemon is not running. Moonlight starts it automatically when you "
                  "share a device.");
#else
        return tr("The usbipd service is not running. Start the service and retry.");
#endif
#ifdef Q_OS_DARWIN
    case NotInstalled:
        return tr("The bundled USB sharing component is missing. Reinstall Moonlight.");
    default:
        return tr("Could not verify the bundled USB sharing component. Reinstall Moonlight.");
#elif defined(Q_OS_LINUX)
    case NotInstalled:
        return tr("The usbip tool is not installed. Install the USB/IP package of your "
                  "distribution (usbip-utils or linux-tools).");
    default:
        return tr("Could not verify the USB sharing environment. Check the usbip installation.");
#else
    default:
        return tr("Could not verify the local USB service and driver. Check the usbipd-win installation.");
#endif
    }
}

void UsbForwardingEnvironment::finish(State state)
{
    m_State = state;
    m_Checking = false;
    emit stateChanged();
    emit checkingChanged();
}
