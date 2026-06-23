#include "clipboardhelperclient.h"

#include "backend/identitymanager.h"
#include "backend/nvcomputer.h"
#include "clipboardipc.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMutexLocker>
#include <QProcess>

#include <SDL.h>

#include <Limelight.h>

ClipboardHelperClient::ClipboardHelperClient(NvComputer* computer, QObject* parent)
    : QObject(parent),
      m_Computer(computer),
      m_Process(nullptr),
      m_Enabled(false),
      m_Disabled(false),
      m_StopRequested(false),
      m_HelperReady(false),
      m_ConfigSequence(0),
      m_NextSequence(1),
      m_NextRestartTicks(0),
      m_RestartAttempts(0),
      m_DroppedInboundFrames(0)
{
}

ClipboardHelperClient::~ClipboardHelperClient()
{
    stop();
}

void ClipboardHelperClient::start()
{
    if (m_Enabled) {
        return;
    }

    m_Enabled = true;
    m_Disabled = false;
    m_StopRequested = false;
    m_RestartAttempts = 0;
    m_NextRestartTicks = 0;
    m_HelperPath = findHelperExecutable();
    if (m_HelperPath.isEmpty()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Clipboard helper executable not found; clipboard sync disabled");
        m_Disabled = true;
        return;
    }

    startProcess();
}

bool ClipboardHelperClient::startProcess()
{
    if (!m_Enabled || m_Disabled || m_Process != nullptr) {
        return false;
    }

    m_Process = new QProcess(this);
    m_Process->setProgram(m_HelperPath);
    m_Process->setProcessChannelMode(QProcess::SeparateChannels);
    m_Process->start();
    if (!m_Process->waitForStarted(2000)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Clipboard helper failed to start: %s",
                    m_Process->errorString().toUtf8().constData());
        delete m_Process;
        m_Process = nullptr;
        scheduleRestart("start failed");
        return false;
    }

    m_HelperReady = false;
    m_StdoutBuffer.clear();
    m_StderrBuffer.clear();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Clipboard helper started: %s",
                m_HelperPath.toUtf8().constData());

    sendCurrentConfig();
    return true;
}

void ClipboardHelperClient::stop()
{
    m_Enabled = false;
    m_StopRequested = true;
    m_HelperReady = false;
    m_NextRestartTicks = 0;

    if (m_Process == nullptr) {
        return;
    }

    if (m_Process->state() != QProcess::NotRunning) {
        writeLine(ClipboardIpc::encodeStop(nextSequence()));
        m_Process->closeWriteChannel();
        if (!m_Process->waitForFinished(1000)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Clipboard helper did not exit cleanly; killing it");
            m_Process->kill();
            m_Process->waitForFinished(1000);
        }
    }

    delete m_Process;
    m_Process = nullptr;
}

void ClipboardHelperClient::updateHostContext()
{
    if (!m_Enabled || m_Disabled || m_Process == nullptr ||
            m_Process->state() == QProcess::NotRunning) {
        return;
    }

    sendCurrentConfig();
}

void ClipboardHelperClient::handleIncomingFrame(const char* data, int length)
{
    if (data == nullptr || length <= 0) {
        return;
    }

    QMutexLocker locker(&m_InboundMutex);
    if (m_InboundFrames.size() >= MAX_QUEUED_HOST_FRAMES) {
        m_InboundFrames.dequeue();
        m_DroppedInboundFrames++;
    }
    m_InboundFrames.enqueue(QByteArray(data, length));
}

void ClipboardHelperClient::processPendingMessages()
{
    if (m_Process == nullptr) {
        maybeRestart();
        return;
    }

    if (m_Process->state() == QProcess::NotRunning) {
        readHelperOutput();
        readHelperErrors();
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Clipboard helper exited: exitCode=%d error=%s",
                    m_Process->exitCode(),
                    m_Process->errorString().toUtf8().constData());
        delete m_Process;
        m_Process = nullptr;
        m_HelperReady = false;
        if (!m_StopRequested) {
            scheduleRestart("unexpected exit");
        }
        return;
    }

    m_Process->waitForReadyRead(0);
    readHelperOutput();
    readHelperErrors();
    if (m_HelperReady) {
        flushQueuedHostFrames();
    }
}

bool ClipboardHelperClient::isRunning() const
{
    return m_Process != nullptr && m_Process->state() != QProcess::NotRunning;
}

void ClipboardHelperClient::scheduleRestart(const char* reason)
{
    m_HelperReady = false;
    if (!m_Enabled || m_Disabled || m_StopRequested) {
        return;
    }

    if (m_RestartAttempts >= MAX_RESTART_ATTEMPTS) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Clipboard helper disabled after %d restart attempts (%s)",
                    m_RestartAttempts,
                    reason);
        m_Disabled = true;
        return;
    }

    m_RestartAttempts++;
    m_NextRestartTicks = SDL_GetTicks() + RESTART_DELAY_MS;
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "Clipboard helper will restart after %s (attempt %d/%d)",
                reason,
                m_RestartAttempts,
                MAX_RESTART_ATTEMPTS);
}

void ClipboardHelperClient::maybeRestart()
{
    if (!m_Enabled || m_Disabled || m_StopRequested || m_HelperPath.isEmpty()) {
        return;
    }

    if (m_NextRestartTicks == 0 || SDL_TICKS_PASSED(SDL_GetTicks(), m_NextRestartTicks)) {
        m_NextRestartTicks = 0;
        startProcess();
    }
}

QString ClipboardHelperClient::findHelperExecutable() const
{
#ifdef Q_OS_WIN
    const QString helperName = QStringLiteral("moonlight-clipboard-helper.exe");
#else
    const QString helperName = QStringLiteral("moonlight-clipboard-helper");
#endif

    QString envPath = QString::fromLocal8Bit(qgetenv("MOONLIGHT_CLIPBOARD_HELPER"));
    if (!envPath.isEmpty() && QFileInfo::exists(envPath)) {
        return envPath;
    }

    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        QDir(appDir).filePath(helperName),
        QDir(appDir).filePath(QStringLiteral("../clipboard-helper/release/") + helperName),
        QDir(appDir).filePath(QStringLiteral("../clipboard-helper/debug/") + helperName),
        QDir(appDir).filePath(QStringLiteral("../clipboard-helper/") + helperName),
        QDir(appDir).filePath(QStringLiteral("../../clipboard-helper/release/") + helperName),
        QDir(appDir).filePath(QStringLiteral("../../clipboard-helper/debug/") + helperName),
        QDir(appDir).filePath(QStringLiteral("../../clipboard-helper/") + helperName),
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

bool ClipboardHelperClient::sendCurrentConfig()
{
    ClipboardIpc::HostConfig config;
    if (m_Computer != nullptr) {
        config.address = m_Computer->activeAddress.address();
        config.httpsPort = m_Computer->activeHttpsPort;
        config.serverCertPem = m_Computer->serverCert.toPem();
    }

    IdentityManager* identity = IdentityManager::get();
    if (identity != nullptr) {
        config.clientCertPem = identity->getCertificate();
        config.clientKeyPem = identity->getPrivateKey();
    }

    if (config.address.isEmpty() || config.httpsPort == 0 ||
            config.serverCertPem.isEmpty() ||
            config.clientCertPem.isEmpty() ||
            config.clientKeyPem.isEmpty()) {
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                     "Clipboard helper config is not available yet");
        return false;
    }

    m_HelperReady = false;
    m_ConfigSequence = nextSequence();
    writeLine(ClipboardIpc::encodeConfigure(m_ConfigSequence, config));
    return true;
}

void ClipboardHelperClient::flushQueuedHostFrames()
{
    QQueue<QByteArray> frames;
    int droppedFrames = 0;
    {
        QMutexLocker locker(&m_InboundMutex);
        qSwap(frames, m_InboundFrames);
        droppedFrames = m_DroppedInboundFrames;
        m_DroppedInboundFrames = 0;
    }

    if (droppedFrames != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Dropped %d queued clipboard frames while helper was busy",
                    droppedFrames);
    }

    while (!frames.isEmpty()) {
        writeLine(ClipboardIpc::encodeHostFrame(nextSequence(), frames.dequeue()));
    }
}

void ClipboardHelperClient::readHelperOutput()
{
    if (m_Process == nullptr) {
        return;
    }

    m_StdoutBuffer += m_Process->readAllStandardOutput();
    if (m_StdoutBuffer.size() > ClipboardIpc::MAX_LINE_BYTES) {
        handleProtocolError(QStringLiteral("helper stdout line exceeded protocol limit"));
        m_StdoutBuffer.clear();
        return;
    }

    int newlineIndex = -1;
    while ((newlineIndex = m_StdoutBuffer.indexOf('\n')) >= 0) {
        QByteArray line = m_StdoutBuffer.left(newlineIndex);
        m_StdoutBuffer.remove(0, newlineIndex + 1);
        while (line.endsWith('\r')) {
            line.chop(1);
        }
        processProtocolLine(line);
    }
}

void ClipboardHelperClient::readHelperErrors()
{
    if (m_Process == nullptr) {
        return;
    }

    m_StderrBuffer += m_Process->readAllStandardError();
    if (m_StderrBuffer.size() > ClipboardIpc::MAX_LINE_BYTES) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Clipboard helper stderr line exceeded protocol limit; discarding buffered stderr");
        m_StderrBuffer.clear();
        return;
    }

    int newlineIndex = -1;
    while ((newlineIndex = m_StderrBuffer.indexOf('\n')) >= 0) {
        QByteArray line = m_StderrBuffer.left(newlineIndex);
        m_StderrBuffer.remove(0, newlineIndex + 1);
        while (line.endsWith('\r')) {
            line.chop(1);
        }
        if (!line.isEmpty()) {
            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                         "Clipboard helper: %s",
                         line.constData());
        }
    }
}

void ClipboardHelperClient::processProtocolLine(const QByteArray& line)
{
    ClipboardIpc::Message message;
    QString error;
    if (!ClipboardIpc::decodeLine(line, message, error)) {
        handleProtocolError(error);
        return;
    }

    if (message.type == ClipboardIpc::MessageType::Ready) {
        if (message.sequence != m_ConfigSequence) {
            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                         "Clipboard helper READY for stale config sequence %u",
                         message.sequence);
            return;
        }
        m_HelperReady = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Clipboard helper is ready");
        return;
    }

    if (message.type == ClipboardIpc::MessageType::LocalFrame) {
        int rc = LiSendClipboardData(message.frame.constData(), message.frame.size());
        if (rc != 0) {
            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                         "LiSendClipboardData(helper, %d bytes) -> %d",
                         static_cast<int>(message.frame.size()),
                         rc);
        }
        return;
    }

    if (message.type == ClipboardIpc::MessageType::Error) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Clipboard helper error [%s]: %s",
                    message.code.toUtf8().constData(),
                    message.text.toUtf8().constData());
        return;
    }

    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                 "Clipboard helper sent unexpected message type");
}

void ClipboardHelperClient::handleProtocolError(const QString& error)
{
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "Clipboard helper protocol error: %s",
                error.toUtf8().constData());
}

void ClipboardHelperClient::writeLine(const QByteArray& line)
{
    if (m_Process == nullptr || m_Process->state() == QProcess::NotRunning) {
        return;
    }

    QByteArray out = line;
    out.append('\n');
    qint64 written = m_Process->write(out);
    if (written != out.size()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Clipboard helper pipe write failed: wrote %lld of %d bytes",
                    static_cast<long long>(written),
                    out.size());
    }
    m_Process->waitForBytesWritten(0);
}

quint32 ClipboardHelperClient::nextSequence()
{
    if (m_NextSequence == 0) {
        m_NextSequence = 1;
    }
    return m_NextSequence++;
}
