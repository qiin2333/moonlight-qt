#include "usbforwardinglocalserver.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QProcess>

// The helper prints "READY <port>" on stdout once its USB/IP listener is up
// and an "ERROR {json...}" line instead when it cannot serve. After that
// first line stdout stays silent for the rest of its lifetime (all library
// logging goes to stderr), so a plain line-based wait is race-free.
static constexpr int START_TIMEOUT_MS = 10000;
static constexpr int PROCESS_START_TIMEOUT_MS = 2000;
static constexpr int STOP_TIMEOUT_MS = 2000;
static constexpr int KILL_TIMEOUT_MS = 1000;
static constexpr int STDERR_TAIL_LIMIT = 8192;

bool UsbForwardingLocalServer::spawnSupported()
{
#ifdef Q_OS_DARWIN
    return true;
#else
    return false;
#endif
}

QString UsbForwardingLocalServer::locateHelper()
{
    const QString helperName = QStringLiteral("moonlight-usbd");

    QString envPath = QString::fromLocal8Bit(qgetenv("MOONLIGHT_USB_HELPER"));
    if (!envPath.isEmpty() && QFileInfo::exists(envPath)) {
        return envPath;
    }

    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        QDir(appDir).filePath(helperName),
        QDir(appDir).filePath(QStringLiteral("../usb-helper/build/moonlight-usbd")),
        QDir(appDir).filePath(QStringLiteral("../../usb-helper/build/moonlight-usbd")),
        QDir(QDir::currentPath()).filePath(helperName)
    };

    for (const QString& candidate : candidates) {
        const QString cleanPath = QDir::cleanPath(candidate);
        if (QFileInfo::exists(cleanPath)) {
            return cleanPath;
        }
    }

    return QString();
}

UsbForwardingLocalServer::~UsbForwardingLocalServer()
{
    stop();
}

bool UsbForwardingLocalServer::start(const QStringList& busIds, quint16* actualPort, QString* error)
{
    if (m_Process != nullptr) {
        *error = QStringLiteral("already running");
        return false;
    }

    const QString helperPath = locateHelper();
    if (helperPath.isEmpty()) {
        *error = QStringLiteral("helper_not_found");
        return false;
    }

    QStringList arguments = { QStringLiteral("serve"), QStringLiteral("--listen"), QStringLiteral("127.0.0.1:0") };
    for (const QString& busId : busIds) {
        arguments << QStringLiteral("--bind") << busId;
    }

    m_Process = new QProcess();
    m_Process->setProgram(helperPath);
    m_Process->setArguments(arguments);
    m_Process->setProcessChannelMode(QProcess::SeparateChannels);
    m_Process->start();

    if (!m_Process->waitForStarted(PROCESS_START_TIMEOUT_MS)) {
        *error = QStringLiteral("spawn_failed: ") + m_Process->errorString();
        stop();
        return false;
    }

    QByteArray pending;
    QElapsedTimer deadline;
    deadline.start();

    while (true) {
        const int remaining = START_TIMEOUT_MS - static_cast<int>(deadline.elapsed());
        if (remaining <= 0) {
            *error = QStringLiteral("ready_timeout");
            stop();
            return false;
        }

        // waitForReadyRead also returns when the process exits, which lets
        // an ERROR-then-exit helper finish without burning the full timeout.
        m_Process->waitForReadyRead(remaining);
        pending += m_Process->readAllStandardOutput();

        int newlineIndex;
        while ((newlineIndex = pending.indexOf('\n')) >= 0) {
            const QByteArray line = pending.left(newlineIndex).trimmed();
            pending.remove(0, newlineIndex + 1);
            if (line.isEmpty()) {
                continue;
            }
            if (line.startsWith("READY ")) {
                bool portOk = false;
                const int port = QString::fromLatin1(line.mid(6)).toInt(&portOk);
                if (portOk && port >= 1 && port <= 65535) {
                    *actualPort = static_cast<quint16>(port);
                    // The helper logs to stderr for its entire lifetime
                    // (usbipdcpp/spdlog). Keep draining it into a bounded
                    // tail so the pipe never fills and blocks the helper.
                    // This connection needs a running event loop on this
                    // object's thread (startConfiguredRemoteUsb runs on the
                    // GUI thread via the queued worker callback).
                    auto drainStderr = [this] {
                        m_StderrTail += m_Process->readAllStandardError();
                        if (m_StderrTail.size() > STDERR_TAIL_LIMIT) {
                            m_StderrTail.remove(0, m_StderrTail.size() - STDERR_TAIL_LIMIT);
                        }
                    };
                    QObject::connect(m_Process, &QProcess::readyReadStandardError,
                                     m_Process, drainStderr);
                    drainStderr();
                    return true;
                }
                *error = QStringLiteral("invalid_ready_line: ") + QString::fromLatin1(line);
                stop();
                return false;
            }
            if (line.startsWith("ERROR")) {
                const QByteArray stderrTail = m_Process->readAllStandardError();
                *error = QString::fromLatin1(line);
                if (!stderrTail.trimmed().isEmpty()) {
                    *error += QStringLiteral(" | ") + QString::fromLocal8Bit(stderrTail.split('\n').first());
                }
                stop();
                return false;
            }
            // Unexpected stdout content: the helper must stay silent, so
            // treat anything else as a protocol violation rather than
            // skipping it.
            *error = QStringLiteral("unexpected_stdout: ") + QString::fromLatin1(line);
            stop();
            return false;
        }

        if (m_Process->state() == QProcess::NotRunning && !m_Process->waitForReadyRead(10)) {
            *error = QStringLiteral("helper_exited_early");
            const QByteArray stderrTail = m_Process->readAllStandardError();
            if (!stderrTail.trimmed().isEmpty()) {
                *error += QStringLiteral(" | ") + QString::fromLocal8Bit(stderrTail.split('\n').first());
            }
            stop();
            return false;
        }
    }
}

void UsbForwardingLocalServer::stop()
{
    if (m_Process == nullptr) {
        return;
    }

    if (m_Process->state() != QProcess::NotRunning) {
        // The helper exits on stdin EOF; only kill if it ignores that.
        m_Process->closeWriteChannel();
        if (!m_Process->waitForFinished(STOP_TIMEOUT_MS)) {
            m_Process->kill();
            m_Process->waitForFinished(KILL_TIMEOUT_MS);
        }
    }

    delete m_Process;
    m_Process = nullptr;
}

bool UsbForwardingLocalServer::isRunning() const
{
    return m_Process != nullptr && m_Process->state() != QProcess::NotRunning;
}
