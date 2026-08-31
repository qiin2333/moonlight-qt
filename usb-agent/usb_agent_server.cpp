#include "usb_agent_server.h"
#include "usb_agent_backend.h"

#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>

#include <utility>

namespace RemoteUsbAgent {

Server::Server(QString socketName, QByteArray authToken,
               Backend *backend, QObject *parent)
    : QObject(parent),
      m_server(new QLocalServer(this)),
      m_socketName(std::move(socketName)),
      m_authToken(std::move(authToken)),
      m_backend(backend)
{
    connect(m_server, &QLocalServer::newConnection,
            this, &Server::acceptConnection);
    if (m_backend != nullptr) {
        m_backend->setCallbacks(BackendCallbacks {
            [this](QByteArray deviceId, quint64 generation) {
                send(QJsonObject {{ QStringLiteral("event"),
                                    QStringLiteral("opened") },
                                  { QStringLiteral("deviceId"),
                                    QString::fromUtf8(deviceId) },
                                  { QStringLiteral("generation"),
                                    QString::number(generation) }});
            },
            [this](quint64 generation) {
                const quint64 eventGeneration = m_stopGeneration != 0
                    ? std::exchange(m_stopGeneration, 0) : generation;
                send(QJsonObject {{ QStringLiteral("event"),
                                    QStringLiteral("stopped") },
                                  { QStringLiteral("generation"),
                                    QString::number(eventGeneration) }});
            },
            [this](quint64 generation, QString message) {
                sendError(QStringLiteral("lease_failed"), message, generation);
            }
        });
    }
}

Server::~Server()
{
    close();
    if (m_backend != nullptr) {
        m_backend->setCallbacks({});
    }
}

bool Server::listen(QString *error)
{
    if (m_socketName.isEmpty() || m_authToken.isEmpty() ||
        m_authToken.size() > kMaxTokenBytes) {
        if (error != nullptr) {
            *error = QStringLiteral("invalid agent socket or token");
        }
        return false;
    }
    if (!m_server->listen(m_socketName)) {
        /* A stale endpoint is safe to remove only after listen failed. */
        QLocalServer::removeServer(m_socketName);
        if (!m_server->listen(m_socketName)) {
            if (error != nullptr) {
                *error = m_server->errorString();
            }
            return false;
        }
    }
#if defined(Q_OS_UNIX)
    /* The endpoint carries the bearer token, so do not leave it readable by
     * other local users. The parent directory remains the OS IPC boundary. */
    QFile::setPermissions(m_socketName,
                           QFileDevice::ReadOwner | QFileDevice::WriteOwner);
#endif
    return true;
}

void Server::close() noexcept
{
    if (m_backend != nullptr) {
        m_backend->stop();
    }
    if (m_client != nullptr) {
        m_client->disconnectFromServer();
        m_client->deleteLater();
        m_client = nullptr;
    }
    if (m_server != nullptr) {
        m_server->close();
    }
}

void Server::acceptConnection()
{
    while (m_server->hasPendingConnections()) {
        QLocalSocket *socket = m_server->nextPendingConnection();
        if (m_client != nullptr) {
            socket->disconnectFromServer();
            socket->deleteLater();
            continue;
        }
        m_client = socket;
        m_readBuffer.clear();
        m_authenticated = false;
        m_helloSeen = false;
        connect(socket, &QLocalSocket::readyRead,
                this, &Server::readClient);
        connect(socket, &QLocalSocket::disconnected,
                this, &Server::clientDisconnected);
    }
}

void Server::readClient()
{
    if (m_client == nullptr) {
        return;
    }
    m_readBuffer.append(m_client->readAll());
    if (m_readBuffer.size() > kMaxMessageBytes) {
        sendError(QStringLiteral("message_too_large"),
                  QStringLiteral("agent message exceeds 64 KiB"));
        m_client->disconnectFromServer();
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
            sendError(QStringLiteral("invalid_json"),
                      QStringLiteral("request must be a JSON object"));
            continue;
        }
        const QJsonObject request = document.object();
        if (!m_helloSeen) {
            m_helloSeen = true;
            if (!authenticate(request)) {
                sendError(QStringLiteral("unauthorized"),
                          QStringLiteral("agent token rejected"));
                m_client->disconnectFromServer();
                return;
            }
            m_authenticated = true;
            send(QJsonObject {{ QStringLiteral("event"), QStringLiteral("ready") },
                              { QStringLiteral("version"),
                                static_cast<int>(kProtocolVersion) }});
            continue;
        }
        if (m_authenticated) {
            handleRequest(request);
        }
    }
}

void Server::clientDisconnected()
{
    QLocalSocket *socket = qobject_cast<QLocalSocket *>(sender());
    if (socket != nullptr && socket != m_client) {
        socket->deleteLater();
        return;
    }
    if (m_backend != nullptr) {
        m_backend->stop();
    }
    if (m_client != nullptr) {
        m_client->deleteLater();
        m_client = nullptr;
    }
    m_readBuffer.clear();
    m_authenticated = false;
    m_helloSeen = false;
}

void Server::send(QJsonObject object)
{
    if (m_client == nullptr || m_client->state() != QLocalSocket::ConnectedState) {
        return;
    }
    QByteArray payload = QJsonDocument(std::move(object)).toJson(QJsonDocument::Compact);
    payload.append('\n');
    if (payload.size() > kMaxMessageBytes) {
        return;
    }
    m_client->write(payload);
}

void Server::sendError(const QString &code, const QString &message,
                       quint64 generation)
{
    QJsonObject object {{ QStringLiteral("event"), QStringLiteral("error") },
                        { QStringLiteral("code"), code },
                        { QStringLiteral("message"), message }};
    if (generation != 0) {
        object.insert(QStringLiteral("generation"),
                      QString::number(generation));
    }
    send(std::move(object));
}

bool Server::authenticate(const QJsonObject &request)
{
    if (request.value(QStringLiteral("op")).toString() !=
        QStringLiteral("hello")) {
        return false;
    }
    const QByteArray supplied = request.value(QStringLiteral("token"))
                                    .toString().toUtf8();
    if (supplied.isEmpty() || supplied.size() > kMaxTokenBytes) {
        return false;
    }
    /* Hash both values before comparing to avoid a length-dependent early
     * return on the local bearer token. */
    return QCryptographicHash::hash(supplied, QCryptographicHash::Sha256) ==
           QCryptographicHash::hash(m_authToken, QCryptographicHash::Sha256);
}

void Server::handleRequest(const QJsonObject &request)
{
    const QString op = request.value(QStringLiteral("op")).toString();
    bool generationOk = false;
    const quint64 generation = request.value(QStringLiteral("generation"))
                                   .toString().toULongLong(&generationOk);
    if (!generationOk || generation == 0) {
        sendError(QStringLiteral("invalid_generation"),
                  QStringLiteral("request generation is invalid"));
        return;
    }
    if (op == QStringLiteral("enumerate")) {
        QString error;
        const QJsonArray devices = m_backend != nullptr
            ? m_backend->enumerate(&error) : QJsonArray {};
        if (!error.isEmpty()) {
            sendError(QStringLiteral("backend_unavailable"), error,
                      generation);
            return;
        }
        send(QJsonObject {{ QStringLiteral("event"), QStringLiteral("devices") },
                          { QStringLiteral("devices"), devices },
                          { QStringLiteral("generation"),
                            QString::number(generation) },
                          { QStringLiteral("backend"),
                            QStringLiteral("libusb") }});
    } else if (op == QStringLiteral("start") ||
               op == QStringLiteral("stop")) {
        if (op == QStringLiteral("stop")) {
            if (m_backend != nullptr) {
                m_stopGeneration = generation;
                m_backend->stop();
            } else {
                send(QJsonObject {{ QStringLiteral("event"),
                                    QStringLiteral("stopped") },
                                  { QStringLiteral("generation"),
                                    QString::number(generation) }});
            }
            return;
        }
        QString error;
        if (m_backend == nullptr || !m_backend->start(request, &error)) {
            sendError(QStringLiteral("backend_unconfigured"),
                      error.isEmpty()
                          ? QStringLiteral("USB backend is not configured in this agent")
                          : error,
                      generation);
            return;
        }
        send(QJsonObject {{ QStringLiteral("event"), QStringLiteral("opening") },
                          { QStringLiteral("deviceId"),
                            request.value(QStringLiteral("deviceId")) },
                          { QStringLiteral("generation"),
                            QString::number(generation) }});
    } else {
        sendError(QStringLiteral("unknown_operation"),
                  QStringLiteral("unsupported agent operation"), generation);
    }
}

} // namespace RemoteUsbAgent
