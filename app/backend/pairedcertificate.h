#pragma once

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslError>

namespace PairedCertificate {

inline bool matches(const QSslCertificate& pinned, const QSslCertificate& peer)
{
    return !pinned.isNull() && !peer.isNull() && pinned == peer;
}

inline bool canIgnoreErrors(const QSslCertificate& pinned, const QList<QSslError>& errors)
{
    if (pinned.isNull() || errors.isEmpty()) {
        return false;
    }
    for (const QSslError& error : errors) {
        if (!matches(pinned, error.certificate())) {
            return false;
        }
    }
    return true;
}

// A normally trusted peer may produce no sslErrors. Check the pin before
// HTTP data is sent as well. The manager must be scoped to this paired host,
// or the returned connection must be disconnected after the request.
inline QMetaObject::Connection enforce(QNetworkAccessManager* manager,
                                       QObject* context,
                                       const QSslCertificate& pinned)
{
    return QObject::connect(manager, &QNetworkAccessManager::encrypted, context,
                            [pinned](QNetworkReply* reply) {
        if (!matches(pinned, reply->sslConfiguration().peerCertificate())) {
            reply->abort();
        }
    });
}

} // namespace PairedCertificate
