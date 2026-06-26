#pragma once

#include "backend/nvcomputer.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSslError>
#include <QSslConfiguration>
#include <QString>
#include <QUrl>

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

    explicit FileMappingClient(NvComputer* computer, QObject* parent = nullptr);

    Capability fetchCapability(int timeoutMs = 3000);
    SmokeResult smokeRead(const QString& mappingId,
                          const QString& path,
                          quint64 offset = 0,
                          quint32 length = 64 * 1024,
                          int timeoutMs = 5000);

private:
    bool buildCapabilityUrl(QUrl& outUrl) const;
    QNetworkAccessManager* nam();
    QSslConfiguration sslConfiguration() const;
    bool isPinnedCertificateError(const QList<QSslError>& errors) const;

    NvComputer* m_Computer;
    QNetworkAccessManager* m_Nam = nullptr;
};
