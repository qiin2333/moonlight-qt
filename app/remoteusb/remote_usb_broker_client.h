#pragma once

/*
 * Qt control-plane client for the Remote USB broker.
 *
 * This class only performs the short, certificate-authenticated capability
 * request.  USB/IP bytes never pass through QNetworkAccessManager; callers
 * use the returned endpoint to create a separate RemoteUsbByteChannel.
 */

#include <QObject>

#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <optional>

class QNetworkAccessManager;

namespace RemoteUsb {

inline constexpr quint32 kBrokerCapabilityVersion = 1u;
inline constexpr quint32 kBrokerCapabilityMinUrb = 49u;
inline constexpr quint32 kBrokerCapabilityMaxUrb = 1024u * 1024u;
inline constexpr quint32 kBrokerCapabilityMaxInflight = 4096u;
inline constexpr quint64 kBrokerCapabilityMaxWindowBytes = 16u * 1024u * 1024u;
inline constexpr quint32 kBrokerCapabilityMaxWindowPdus = 4096u;
inline constexpr quint32 kBrokerCapabilityDefaultWindowBytes =
    kBrokerCapabilityMaxWindowBytes;
inline constexpr quint32 kBrokerCapabilityDefaultWindowPdus =
    kBrokerCapabilityMaxWindowPdus;
inline constexpr quint32 kBrokerCapabilityDefaultReassemblySize = 1024u * 1024u;
inline constexpr quint32 kBrokerCapabilityDefaultMaxFragments = 4096u;
inline constexpr quint32 kBrokerCapabilityMaxExpiresMs = 10u * 60u * 1000u;
inline constexpr qsizetype kBrokerCapabilityMaxResponseBytes = 64 * 1024;

/* A capability is deliberately a value object.  The nonce is copied by the
 * caller into the broker HELLO and must not be logged or persisted. */
struct BrokerCapability {
    QString host;
    quint16 port = 0;
    QByteArray nonce;
    quint32 maxUrb = kBrokerCapabilityMaxUrb;
    quint32 maxInflight = kBrokerCapabilityMaxInflight;
    quint64 txWindowBytes = kBrokerCapabilityDefaultWindowBytes;
    quint32 txWindowPdus = kBrokerCapabilityDefaultWindowPdus;
    quint64 rxWindowBytes = kBrokerCapabilityDefaultWindowBytes;
    quint32 rxWindowPdus = kBrokerCapabilityDefaultWindowPdus;
    quint32 maxReassemblySize = kBrokerCapabilityDefaultReassemblySize;
    quint32 maxFragments = kBrokerCapabilityDefaultMaxFragments;
    quint32 expiresMs = 0;

    bool valid() const noexcept;
};

struct BrokerCapabilityRequest {
    quint64 streamGeneration = 0;
    quint64 sessionToken = 0;
    quint64 attachmentToken = 0;
    quint64 leaseToken = 0;
};

/* Explicit paired-host context used by both the Moonlight process and the
 * standalone USB agent. Keeping this as a value object prevents the agent
 * from depending on NvComputer, IdentityManager, or Moonlight settings. */
struct BrokerHostConfig {
    QString host;
    quint16 httpsPort = 0;
    QSslCertificate serverCertificate;
    QString clientIdentity;
    QString clientName;
    QSslConfiguration sslConfiguration;

    bool valid() const noexcept;
};

class RemoteUsbBrokerClient final : public QObject
{
public:
    explicit RemoteUsbBrokerClient(BrokerHostConfig hostConfig,
                                   QNetworkAccessManager *networkManager = nullptr,
                                   QObject *parent = nullptr);
    ~RemoteUsbBrokerClient() override = default;

    Q_DISABLE_COPY(RemoteUsbBrokerClient)

    /*
     * Fetches a fresh, lease-bound capability.  The method is synchronous so
     * it can be used by the existing worker-based session setup; do not call
     * it from the GUI thread.  No token or response body is included in an
     * error string or log message.
     */
    std::optional<BrokerCapability> fetch(
        const BrokerCapabilityRequest &request,
        int timeoutMs = 5000,
        QString *error = nullptr);

    /* Strict parser kept public for deterministic tests and non-network
     * embedders.  expectedHost is mandatory: a capability may not redirect a
     * paired client to an arbitrary host. */
    static bool parseCapability(const QByteArray &json,
                                const QString &expectedHost,
                                BrokerCapability *out,
                                QString *error = nullptr);

    /* The HTTPS query identity and the fixed 16-byte HELLO identity use the
     * same canonicalization on every Qt platform. */
    static QString canonicalIdentity(const QString &value);
    static QByteArray wireIdentity(const QString &value);

private:
    static bool sameHost(const QString &left, const QString &right) noexcept;
    static bool decodeNonce(const QString &encoded, QByteArray *out) noexcept;
    static bool readUnsigned(const QJsonValue &value,
                             quint64 maximum,
                             quint64 *out) noexcept;
    static bool readField(const QJsonObject &object,
                          const QJsonObject &limits,
                          const QStringList &names,
                          quint64 maximum,
                          quint64 defaultValue,
                          quint64 *out) noexcept;

    QNetworkAccessManager *m_networkManager = nullptr;
    BrokerHostConfig m_hostConfig;
};

} // namespace RemoteUsb
