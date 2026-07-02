#pragma once

#include "backend/nvcomputer.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QByteArray>
#include <QSslError>
#include <QSslConfiguration>
#include <QString>
#include <QUrl>

class QSslSocket;

class FileMappingClient : public QObject
{
    Q_OBJECT

public:
    struct Capability {
        bool ok = false;
        bool enabled = false;
        bool listening = false;
        quint16 port = 0;
        QString sessionEndpoint;
        QString sessionUrl;
        QString sessionToken;
        QString clientUuid;
        QJsonArray features;
        QString error;
    };

    struct SmokeResult {
        bool ok = false;
        QString error;
        QJsonObject hello;
        QJsonObject list;
        QJsonObject read;
    };

    struct RpcResult {
        bool ok = false;
        QString error;
        QJsonObject reply;
    };

    explicit FileMappingClient(NvComputer* computer, QObject* parent = nullptr);
    ~FileMappingClient() override;

    Capability fetchCapability(int timeoutMs = 3000);
    bool connectSession(const Capability& capability, int timeoutMs, QString* error = nullptr);
    QJsonObject lastHello() const { return m_LastHello; }
    RpcResult list(const QString& mappingId, const QString& path, int timeoutMs = 5000);
    RpcResult stat(const QString& mappingId, const QString& path, int timeoutMs = 5000);
    RpcResult read(const QString& mappingId,
                   const QString& path,
                   quint64 offset = 0,
                   quint32 length = 64 * 1024,
                   int timeoutMs = 5000);
    SmokeResult smokeRead(const QString& mappingId,
                          const QString& path,
                          quint64 offset = 0,
                          quint32 length = 64 * 1024,
                          int timeoutMs = 5000);

private:
    bool buildCapabilityUrl(QUrl& outUrl) const;
    bool buildSessionUrl(const Capability& capability, QUrl& outUrl) const;
    bool sendAndWait(const QJsonObject& message, QJsonObject& out, int timeoutMs, QString* error = nullptr);
    RpcResult sendRpc(QJsonObject message, int timeoutMs);
    void closeSession();
    QNetworkAccessManager* nam();
    QSslConfiguration sslConfiguration() const;
    bool isPinnedCertificateError(const QList<QSslError>& errors) const;

    NvComputer* m_Computer;
    QNetworkAccessManager* m_Nam = nullptr;
    QSslSocket* m_Socket = nullptr;
    QByteArray m_WsBuffer;
    QJsonObject m_LastHello;
    quint64 m_NextRequestId = 1;
    bool m_SessionConnected = false;
};
