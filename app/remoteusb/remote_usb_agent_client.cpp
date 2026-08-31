#include "remote_usb_agent_client.h"

#include "../../usb-agent/usb_agent_protocol.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QEventLoop>
#include <QProcessEnvironment>
#include <QRandomGenerator>
#include <QLocalSocket>
#include <QSslKey>
#include <QSslSocket>

#include <utility>

namespace RemoteUsb {

namespace {
constexpr int kConnectRetryIntervalMs = 50;
constexpr int kMaxConnectAttempts = 100;

quint64 randomToken()
{
    quint64 value = 0;
    do {
        value = QRandomGenerator::system()->generate64();
    } while (value == 0);
    return value;
}
}

bool RemoteUsbAgentHostConfig::valid() const noexcept
{
    return !host.trimmed().isEmpty() && httpsPort != 0 &&
           !serverCertificate.isNull() && !clientIdentity.trimmed().isEmpty() &&
           !clientName.trimmed().isEmpty() &&
           !sslConfiguration.localCertificate().isNull() &&
           !sslConfiguration.privateKey().isNull() &&
           sslConfiguration.peerVerifyMode() != QSslSocket::VerifyNone;
}

RemoteUsbAgentClient::RemoteUsbAgentClient(QObject *parent)
    : QObject(parent),
      m_process(new QProcess(this)),
      m_connectTimer(this)
{
    m_connectTimer.setSingleShot(false);
    m_connectTimer.setInterval(kConnectRetryIntervalMs);
    connect(&m_connectTimer, &QTimer::timeout,
            this, &RemoteUsbAgentClient::retryConnect);
    connect(m_process, &QProcess::finished,
            this, &RemoteUsbAgentClient::processFinished);
}

RemoteUsbAgentClient::~RemoteUsbAgentClient()
{
    shutdown();
}

bool RemoteUsbAgentClient::launch(const QString &program,
                                  const QString &socketName,
                                  const QByteArray &token,
                                  QString *error)
{
    if (program.trimmed().isEmpty() || socketName.trimmed().isEmpty() ||
        token.isEmpty() || token.size() > RemoteUsbAgent::kMaxTokenBytes) {
        if (error != nullptr) {
            *error = QStringLiteral("invalid Remote USB agent launch configuration");
        }
        return false;
    }
    shutdown();
    m_shuttingDown = false;
    m_socketName = socketName;
    m_token = token;
    m_connectAttempts = 0;
    m_ready = false;
    m_process->setProgram(program);
    m_process->setArguments({ QStringLiteral("--socket"), socketName });
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("MOONLIGHT_USB_AGENT_TOKEN"),
                       QString::fromUtf8(token));
    m_process->setProcessEnvironment(environment);
    m_process->setProcessChannelMode(QProcess::SeparateChannels);
    m_process->start();
    if (!m_process->waitForStarted(1500)) {
        if (error != nullptr) {
            *error = QStringLiteral("Remote USB agent failed to start");
        }
        return false;
    }
    connectToAgent(socketName, token);
    return true;
}

void RemoteUsbAgentClient::connectToAgent(const QString &socketName,
                                          const QByteArray &token)
{
    closeSocket();
    m_socketName = socketName;
    m_token = token;
    m_connectAttempts = 0;
    m_ready = false;
    if (m_socketName.isEmpty() || m_token.isEmpty() ||
        m_token.size() > RemoteUsbAgent::kMaxTokenBytes) {
        fail(QStringLiteral("invalid Remote USB agent connection"));
        return;
    }
    m_socket = new QLocalSocket(this);
    connect(m_socket, &QLocalSocket::connected,
            this, &RemoteUsbAgentClient::sendHello);
    connect(m_socket, &QLocalSocket::readyRead,
            this, &RemoteUsbAgentClient::readSocket);
    connect(m_socket, &QLocalSocket::disconnected,
            this, [this] {
                if (!m_shuttingDown && m_ready) {
                    fail(QStringLiteral("Remote USB agent disconnected"));
                }
            });
    m_connectTimer.start();
    retryConnect();
}

bool RemoteUsbAgentClient::configureHost(RemoteUsbAgentHostConfig config,
                                         QString *error)
{
    if (!config.valid()) {
        if (error != nullptr) {
            *error = QStringLiteral("invalid Remote USB agent host configuration");
        }
        return false;
    }
    m_hostConfig = std::move(config);
    return true;
}

void RemoteUsbAgentClient::retryConnect()
{
    if (m_socket == nullptr || m_ready || m_shuttingDown) {
        m_connectTimer.stop();
        return;
    }
    if (++m_connectAttempts > kMaxConnectAttempts) {
        m_connectTimer.stop();
        fail(QStringLiteral("Remote USB agent connection timed out"));
        return;
    }
    if (m_socket->state() == QLocalSocket::UnconnectedState) {
        m_socket->connectToServer(m_socketName);
    }
}

void RemoteUsbAgentClient::sendHello()
{
    sendRequest(QStringLiteral("hello"),
                QJsonObject {{ QStringLiteral("token"),
                               QString::fromUtf8(m_token) }});
}

void RemoteUsbAgentClient::enumerate()
{
    sendRequest(QStringLiteral("enumerate"));
}

void RemoteUsbAgentClient::start(const QByteArray &deviceId)
{
    if (!m_hostConfig.valid()) {
        fail(QStringLiteral("Remote USB agent host is not configured"));
        return;
    }
    if (++m_streamGeneration == 0) {
        ++m_streamGeneration;
    }
    const QSslCertificate clientCertificate =
        m_hostConfig.sslConfiguration.localCertificate();
    const QSslKey clientKey = m_hostConfig.sslConfiguration.privateKey();
    sendRequest(QStringLiteral("start"),
                QJsonObject {{ QStringLiteral("deviceId"),
                               QString::fromUtf8(deviceId) },
                             { QStringLiteral("host"), m_hostConfig.host },
                             { QStringLiteral("httpsPort"),
                               static_cast<int>(m_hostConfig.httpsPort) },
                             { QStringLiteral("serverCertificate"),
                               QString::fromLatin1(
                                   m_hostConfig.serverCertificate.toDer().toBase64()) },
                             { QStringLiteral("clientCertificate"),
                               QString::fromLatin1(
                                   clientCertificate.toDer().toBase64()) },
                             { QStringLiteral("clientPrivateKey"),
                               QString::fromLatin1(clientKey.toPem().toBase64()) },
                             { QStringLiteral("clientIdentity"),
                               m_hostConfig.clientIdentity },
                             { QStringLiteral("clientName"),
                               m_hostConfig.clientName },
                             { QStringLiteral("streamGeneration"),
                               QString::number(m_streamGeneration) },
                             { QStringLiteral("sessionToken"),
                               QString::number(randomToken()) },
                             { QStringLiteral("attachmentToken"),
                               QString::number(randomToken()) },
                             { QStringLiteral("leaseToken"),
                               QString::number(randomToken()) }});
}

void RemoteUsbAgentClient::stop()
{
    sendRequest(QStringLiteral("stop"));
}

void RemoteUsbAgentClient::shutdown() noexcept
{
    try {
        if (!m_shuttingDown && m_ready && m_socket != nullptr &&
            m_socket->state() == QLocalSocket::ConnectedState) {
            QEventLoop loop;
            const QMetaObject::Connection stoppedConnection = connect(
                this, &RemoteUsbAgentClient::stopped,
                &loop, &QEventLoop::quit);
            QTimer::singleShot(1500, &loop, &QEventLoop::quit);
            stop();
            m_socket->waitForBytesWritten(250);
            loop.exec(QEventLoop::ExcludeUserInputEvents);
            disconnect(stoppedConnection);
        }
        m_shuttingDown = true;
        m_connectTimer.stop();
        closeSocket();
        if (m_process != nullptr && m_process->state() != QProcess::NotRunning) {
            m_process->terminate();
            if (!m_process->waitForFinished(1000)) {
                m_process->kill();
                m_process->waitForFinished(1000);
            }
        }
        RemoteUsbAgentHostConfig clearedHostConfig;
        m_hostConfig = std::move(clearedHostConfig);
        m_token.clear();
        m_readBuffer.clear();
        m_ready = false;
    } catch (...) {
        m_shuttingDown = true;
        closeSocket();
        if (m_process != nullptr && m_process->state() != QProcess::NotRunning) {
            m_process->kill();
            m_process->waitForFinished(1000);
        }
    }
}

void RemoteUsbAgentClient::sendRequest(const QString &operation,
                                       const QJsonObject &extra)
{
    if (m_socket == nullptr || (!m_ready && operation != QStringLiteral("hello")) ||
        m_socket->state() != QLocalSocket::ConnectedState) {
        return;
    }
    QJsonObject request = extra;
    request.insert(QStringLiteral("op"), operation);
    request.insert(QStringLiteral("generation"),
                   QString::number(++m_generation));
    QByteArray payload = QJsonDocument(request).toJson(QJsonDocument::Compact);
    payload.append('\n');
    if (payload.size() > RemoteUsbAgent::kMaxMessageBytes ||
        m_socket->write(payload) != payload.size()) {
        fail(QStringLiteral("Remote USB agent request was not queued"));
    }
}

void RemoteUsbAgentClient::readSocket()
{
    if (m_socket == nullptr) {
        return;
    }
    m_readBuffer.append(m_socket->readAll());
    if (m_readBuffer.size() > RemoteUsbAgent::kMaxMessageBytes) {
        fail(QStringLiteral("Remote USB agent message is too large"));
        return;
    }
    for (;;) {
        const qsizetype newline = m_readBuffer.indexOf('\n');
        if (newline < 0) {
            return;
        }
        const QByteArray line = m_readBuffer.left(newline).trimmed();
        m_readBuffer.remove(0, newline + 1);
        if (line.isEmpty()) {
            continue;
        }
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error != QJsonParseError::NoError ||
            !document.isObject()) {
            fail(QStringLiteral("Remote USB agent returned invalid JSON"));
            return;
        }
        handleMessage(document.object());
    }
}

void RemoteUsbAgentClient::handleMessage(const QJsonObject &message)
{
    const QString event = message.value(QStringLiteral("event")).toString();
    if (event == QStringLiteral("ready")) {
        const int version = message.value(QStringLiteral("version")).toInt();
        if (version != static_cast<int>(RemoteUsbAgent::kProtocolVersion)) {
            fail(QStringLiteral("Remote USB agent protocol version is unsupported"));
            return;
        }
        m_ready = true;
        m_token.clear();
        m_connectTimer.stop();
        emit ready(static_cast<quint32>(version));
        return;
    }

    const QJsonValue generationValue =
        message.value(QStringLiteral("generation"));
    if (!generationValue.isString()) {
        fail(QStringLiteral("Remote USB agent event generation is missing"));
        return;
    }
    bool generationOk = false;
    const quint64 generation = generationValue.toString().toULongLong(
        &generationOk);
    if (!generationOk || generation == 0) {
        fail(QStringLiteral("Remote USB agent event generation is invalid"));
        return;
    }
    if (generation != m_generation) {
        return;
    }

    if (event == QStringLiteral("devices")) {
        const QJsonValue devices = message.value(QStringLiteral("devices"));
        if (!devices.isArray()) {
            fail(QStringLiteral("Remote USB agent device list is invalid"));
            return;
        }
        emit devicesChanged(devices.toArray());
    } else if (event == QStringLiteral("opened")) {
        emit opened(message.value(QStringLiteral("deviceId")).toString().toUtf8());
    } else if (event == QStringLiteral("stopped")) {
        emit stopped();
    } else if (event == QStringLiteral("opening")) {
        return;
    } else if (event == QStringLiteral("error")) {
        const QString detail = message.value(QStringLiteral("message")).toString();
        fail(detail.isEmpty() ? QStringLiteral("Remote USB agent request failed")
                             : detail);
    }
}

void RemoteUsbAgentClient::processFinished(int, QProcess::ExitStatus)
{
    if (!m_shuttingDown) {
        fail(QStringLiteral("Remote USB agent exited"));
    }
}

void RemoteUsbAgentClient::fail(const QString &message)
{
    emit failed(message);
}

void RemoteUsbAgentClient::closeSocket() noexcept
{
    if (m_socket != nullptr) {
        m_socket->abort();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
}

} // namespace RemoteUsb
