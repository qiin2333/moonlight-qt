#include "usbforwardinglocalserver.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QDebug>
#include <memory>

// The helper prints "READY <port>" on stdout once its USB/IP listener is up
// and an "ERROR {json...}" line instead when it cannot serve. After that
// first line stdout stays silent for the rest of its lifetime (all library
// logging goes to stderr), so a plain line-based wait is race-free.
static constexpr int START_TIMEOUT_MS = 10000;
static constexpr int PROCESS_START_TIMEOUT_MS = 2000;
static constexpr int STOP_TIMEOUT_MS = 2000;
static constexpr int KILL_TIMEOUT_MS = 1000;
static constexpr int STDERR_TAIL_LIMIT = 8192;
static int nativeProcesses = 0;

bool UsbForwardingLocalServer::hasPendingNativeCleanup() { return nativeProcesses > 0; }

bool UsbForwardingLocalServer::spawnSupported()
{
#if defined(Q_OS_DARWIN) || (defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID))
    return true;
#else
    return false;
#endif
}

QString UsbForwardingLocalServer::locateHelper()
{
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        QDir(appDir).filePath(QStringLiteral("moonlight-usb-host")),
        QDir(appDir).filePath(QStringLiteral("../usb-helper/linux/moonlight-usb-host"))
    };
#else
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

#endif
    for (const QString& candidate : candidates) {
        const QString cleanPath = QDir::cleanPath(candidate);
        if (QFileInfo(cleanPath).isExecutable() && QFileInfo(cleanPath).isFile()) {
            return cleanPath;
        }
    }

    return QString();
}

UsbForwardingLocalServer::~UsbForwardingLocalServer()
{
    stop();
}

void UsbForwardingLocalServer::startNative(const QString& busId, const QString& identity,
                                           StartCallback ready,
                                           std::function<void(const QString&)> exited)
{
    if (m_Process) { ready(0, QStringLiteral("already_running")); return; }
    const QString helper = locateHelper();
    const QString pkexec = QStandardPaths::findExecutable(QStringLiteral("pkexec"),
        {QStringLiteral("/usr/bin"), QStringLiteral("/bin")});
    if (helper.isEmpty() || pkexec.isEmpty()) {
        ready(0, helper.isEmpty() ? QStringLiteral("helper_not_found") : QStringLiteral("authorization_unavailable"));
        return;
    }

    // Root cannot normally access another user's AppImage FUSE mount. Copy
    // the small helper outside it, retaining the directory through rollback.
    // AppImage builds link its C++ runtime statically: pkexec clears LD_*.
    const QString cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (cache.isEmpty() || !QDir().mkpath(cache)) { ready(0, QStringLiteral("helper_staging_failed")); return; }
    auto staging = std::make_shared<QTemporaryDir>(cache + QStringLiteral("/usb-helper-XXXXXX"));
    const QString executable = staging->filePath(QStringLiteral("moonlight-usb-host"));
    if (!staging->isValid() || !QFile::copy(helper, executable) ||
        !QFile::setPermissions(executable, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)) {
        ready(0, QStringLiteral("helper_staging_failed"));
        return;
    }

    startNativeProcess(pkexec, {QStringLiteral("--disable-internal-agent"), executable,
                               QStringLiteral("serve"), QStringLiteral("--busid"), busId,
                               QStringLiteral("--identity"), identity}, staging, std::move(ready), std::move(exited));
}

void UsbForwardingLocalServer::startNativeProcess(const QString& program, const QStringList& arguments,
                                                  std::shared_ptr<QTemporaryDir> staging, StartCallback ready,
                                                  std::function<void(const QString&)> exited)
{
    struct Startup { QByteArray pending, errors; bool complete = false, ready = false; };
    auto state = std::make_shared<Startup>();
    m_Native = true;
    auto* process = m_Process = new QProcess;
    ++nativeProcesses;
    // finish/FailedToStart occur exactly once per process. Count those rather
    // than deferred deletion, which the SDL event loop may postpone.
    process->setProcessChannelMode(QProcess::SeparateChannels);
    // These connections outlive this object when stop() hands cleanup off.
    QObject::connect(process, &QProcess::readyReadStandardError, process, [process, state] {
        state->errors = (state->errors + process->readAllStandardError()).right(STDERR_TAIL_LIMIT);
    });
    QObject::connect(process, &QProcess::finished, process, [process, staging, state, exited](int code) {
        --nativeProcesses;
        state->errors = (state->errors + process->readAllStandardError()).right(STDERR_TAIL_LIMIT);
        if (code) qWarning() << "Native USB helper exited:" << code << state->errors;
        process->deleteLater();
        if (state->ready) exited(code ? QString::fromUtf8(state->errors) : QString());
    });
    QObject::connect(process, &QProcess::errorOccurred, process, [process, staging](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) { --nativeProcesses; process->deleteLater(); }
    });
    auto fail = [this, state, ready](const QString& error) {
        if (state->complete) return;
        state->complete = true;
        stop();
        ready(0, error); // May destroy this object; do not access it afterwards.
    };
    QObject::connect(process, &QProcess::readyReadStandardOutput, this, [state, process, ready, fail] {
        const QByteArray output = process->readAllStandardOutput();
        if (state->complete) return;
        state->pending += output;
        if (state->pending.size() > 4096) { fail(QStringLiteral("invalid_ready_line")); return; }
        const int newline = state->pending.indexOf('\n');
        if (newline < 0) return;
        const QByteArray line = state->pending.left(newline).trimmed();
        bool ok = false;
        const int port = line.startsWith("READY ") ? line.mid(6).toInt(&ok) : 0;
        if (!ok || port < 1 || port > 65535) {
            fail(QString::fromUtf8(line) + QLatin1Char(' ') + QString::fromUtf8(state->errors));
            return;
        }
        state->complete = true;
        state->ready = true;
        ready(static_cast<quint16>(port), {});
    });
    QObject::connect(process, &QProcess::errorOccurred, this, [fail](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) fail(QStringLiteral("authorization_unavailable"));
    });
    QObject::connect(process, &QProcess::finished, this, [this, process, state, fail](int code) {
        state->errors = (state->errors + process->readAllStandardError()).right(STDERR_TAIL_LIMIT);
        if (!state->complete) {
            fail(code == 126 ? QStringLiteral("authorization_cancelled") :
                 code == 127 ? QStringLiteral("authorization_failed") : QString::fromUtf8(state->errors));
            return;
        }
        m_Process = nullptr;
    });
    // Timer belongs to the process: destroying the server disconnects the
    // callback, and a completed startup cannot time out an active forwarding.
    auto* timer = new QTimer(process);
    timer->setSingleShot(true);
    QObject::connect(timer, &QTimer::timeout, this, [fail] { fail(QStringLiteral("authorization_timeout")); });
    timer->start(120000);
    process->start(program, arguments);
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

    if (m_Native) {
        // The elevated supervisor owns rollback, including parent death. Keep
        // the process (and staged helper) alive until it finishes cleanup.
        QProcess* process = m_Process;
        m_Process = nullptr;
        process->disconnect(this);
        for (auto* timer : process->findChildren<QTimer*>()) timer->disconnect(this);
        process->closeWriteChannel();
        if (process->state() == QProcess::NotRunning) process->deleteLater();
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
