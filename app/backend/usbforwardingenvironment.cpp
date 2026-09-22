#include "usbforwardingenvironment.h"

#include "usbforwardinglocalserver.h"
#include "systemproperties.h"

#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>
#include <QTcpSocket>

#ifdef Q_OS_WIN32
#include <windows.h>
#endif

#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
static QString locateLinuxTool(const QString &name)
{
    return QStandardPaths::findExecutable(name,
                                          { QStringLiteral("/usr/sbin"), QStringLiteral("/sbin"),
                                            QStringLiteral("/usr/bin"), QStringLiteral("/bin") });
}
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
#elif defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    const State readiness = probeServices();
    if (readiness != Ready) {
        m_Version.clear();
        finish(readiness);
        return;
    }
    m_Checking = true;
    emit checkingChanged();
    m_State = Checking;
    emit stateChanged();
    startHelperVersionProbe(UsbForwardingLocalServer::locateHelper());
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
#elif defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    if (!SystemProperties::isUsbForwardingSupported() ||
        UsbForwardingLocalServer::locateHelper().isEmpty() ||
        locateLinuxTool(QStringLiteral("usbip")).isEmpty() ||
        locateLinuxTool(QStringLiteral("usbipd")).isEmpty())
        return NotInstalled;
    if (QStandardPaths::findExecutable(QStringLiteral("pkexec"),
                                       { QStringLiteral("/usr/bin"), QStringLiteral("/bin") })
            .isEmpty())
        return CheckFailed;
    QTcpSocket daemon;
    daemon.connectToHost(QHostAddress::LocalHost, 3240);
    return daemon.waitForConnected(300) ? Ready : ServiceStopped;
#elif defined(Q_OS_DARWIN)
    // Called synchronously from the session worker: never spawn a process
    // here, just check that the bundled helper is present.
    return UsbForwardingLocalServer::locateHelper().isEmpty() ? NotInstalled : Ready;
#else
    return NotInstalled;
#endif
}

QString UsbForwardingEnvironment::readinessError(State state)
{
    switch (state) {
    case Ready: return {};
    case DriverStopped:
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
        return tr("The usbip-host kernel module is not available.");
#else
        return tr("The USB forwarding driver is not running. Start VBoxUSBMon as administrator, or restart Windows.");
#endif
    case ServiceStopped:
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
        return tr("The USB/IP service is not running. Start your distribution's usbipd service "
                  "and retry.");
#else
        return tr("The usbipd service is not running. Start the service and retry.");
#endif
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    case NotInstalled:
        return tr("USB forwarding requires your distribution's USB/IP tools (usbip and usbipd), "
                  "the usbip-host kernel module, and Moonlight's USB control helper.");
    default:
        return tr("USB forwarding requires polkit (pkexec) and a desktop authentication agent.");
#elif defined(Q_OS_DARWIN)
    case NotInstalled:
        return tr("The bundled USB sharing component is missing. Reinstall Moonlight.");
    default:
        return tr("Could not verify the bundled USB sharing component. Reinstall Moonlight.");
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
