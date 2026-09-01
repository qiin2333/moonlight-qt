#pragma once

#include "usb_agent_protocol.h"

#include <QObject>
#include <QJsonObject>

class QLocalServer;
class QLocalSocket;

namespace RemoteUsbAgent {

class Backend;

class Server final : public QObject
{
    Q_OBJECT

public:
    explicit Server(QString socketName, QByteArray authToken,
                    Backend *backend = nullptr,
                    QObject *parent = nullptr);
    ~Server() override;

    bool listen(QString *error = nullptr);
    void close() noexcept;

private slots:
    void acceptConnection();
    void readClient();
    void clientDisconnected();

private:
    void send(QJsonObject object);
    void sendError(const QString &code, const QString &message,
                   quint64 generation = 0);
    bool authenticate(const QJsonObject &request);
    void handleRequest(const QJsonObject &request);

    QLocalServer *m_server = nullptr;
    QLocalSocket *m_client = nullptr;
    QString m_socketName;
    QByteArray m_authToken;
    Backend *m_backend = nullptr;
    QByteArray m_readBuffer;
    bool m_authenticated = false;
    bool m_helloSeen = false;
    quint64 m_stopGeneration = 0;
};

} // namespace RemoteUsbAgent
