#pragma once

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslError>
#include <QVariant>

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
// HTTP data is sent as well. The manager must be used exclusively under this
// policy, with automatic redirects disabled. Change identities only between
// requests, disconnecting the old enforcement connection first.
inline QMetaObject::Connection enforce(QNetworkAccessManager* manager, QObject* context,
                                       const QSslCertificate& pinned)
{
    // Reused connections do not emit encrypted(). Discard connections created
    // before adopting this policy or under a previous paired identity.
    const char* identityProperty = "_moonlightPairedCertificate";
    const QByteArray identity = pinned.toDer();
    const QVariant previous = manager->property(identityProperty);
    if (!previous.isValid() || previous.toByteArray() != identity) {
        manager->clearConnectionCache();
        manager->setProperty(identityProperty, identity);
    }
    return QObject::connect(manager, &QNetworkAccessManager::encrypted, context,
                            [pinned](QNetworkReply* reply) {
                                if (!matches(pinned, reply->sslConfiguration().peerCertificate())) {
                                    reply->abort();
                                }
                            });
}

} // namespace PairedCertificate
