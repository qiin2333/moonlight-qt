#include "usbforwardingenvironment.h"

#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

namespace {

const char* kServiceName = "usbipd";

} // namespace

UsbForwardingEnvironment::UsbForwardingEnvironment(QObject *parent)
    : QObject(parent)
{
}

UsbForwardingEnvironment* UsbForwardingEnvironment::get()
{
    static UsbForwardingEnvironment environment;
    return &environment;
}

void UsbForwardingEnvironment::refresh()
{
    if (m_Checking) {
        return;
    }
#ifdef Q_OS_WIN32
    QString usbipdExe = QStandardPaths::findExecutable(QStringLiteral("usbipd"));
    if (usbipdExe.isEmpty()) {
        const QString bundledPath =
            QStringLiteral("C:/Program Files/usbipd-win/usbipd.exe");
        if (QFileInfo::exists(bundledPath)) {
            usbipdExe = bundledPath;
        }
    }
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
    m_UsbipdExe = usbipdExe;
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
    probe->start(m_UsbipdExe, {QStringLiteral("--version")});
}

void UsbForwardingEnvironment::startServiceProbe()
{
    QProcess *probe = new QProcess(this);
    /* FailedToStart emits errorOccurred but never finished; handle it so a
     * missing sc.exe cannot wedge the probe. */
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
        const bool running =
            exitCode == 0 &&
            QString::fromLocal8Bit(probe->readAllStandardOutput())
                .contains(QLatin1String("RUNNING"));
        finish(running ? Ready : ServiceStopped);
    });
    QTimer::singleShot(8000, probe, &QProcess::kill);
#ifdef Q_OS_WIN32
    probe->start(QStringLiteral("sc.exe"),
                 {QStringLiteral("query"), QString::fromLatin1(kServiceName)});
#else
    probe->deleteLater();
    finish(ServiceStopped);
#endif
}

void UsbForwardingEnvironment::finish(State state)
{
    m_State = state;
    m_Checking = false;
    emit stateChanged();
    emit checkingChanged();
}
