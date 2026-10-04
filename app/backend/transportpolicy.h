#pragma once

#include <QJsonObject>
#include <QString>
#include <QVector>
#include <optional>

namespace TransportPolicy {

struct AutomaticControl
{
    bool bitrate;
    bool fec;
    int maximumKbps;
};
struct Policy
{
    QString revision;
    QString controlEpoch;
    QString source;
    int totalKbps;
    int encoderKbps;
    int fecBase;
    int fecKey;
    int fecRecovery;
    int otherKbps;
    int repairKbps;
    int probeKbps;
    int overheadKbps;
    std::optional<AutomaticControl> automatic;
    QJsonObject immutableFields;
};
struct Receipt
{
    Policy policy;
    bool encoderApplied;
    std::optional<QString> firstSentFrame;
    QString failure;
};
struct NetworkStatistics
{
    QString connectionEpoch;
    QString receiverClockEpoch;
    std::optional<QString> sampleTimeUs;
    int receivedPackets = 0;
    int missingPackets = 0;
    int unknownPackets = 0;
    QString committedPackets;
    QString committedIpBytes;
    QString missingDeclarations;
    QString lateCorrections;
    QString unresolvedEvictions;
    std::optional<double> rawLossPercent;
    std::optional<double> coveragePercent;
    bool fresh = false;
    bool historyTruncated = false;
    QString reason;
    std::optional<int> freshnessRemainingUs;
    std::optional<int> windowDurationMs;
};
struct Status
{
    QString sessionId;
    QString connectionEpoch;
    QString controlEpoch;
    Policy accepted;
    std::optional<Policy> confirmed;
    bool encoderReady;
    bool pending;
    bool stopped;
    bool liveControlAvailable;
    QVector<Receipt> receipts;
    std::optional<NetworkStatistics> networkStatistics;
    bool automaticFecAvailable = false;
};
struct Submission
{
    QString requestId;
    QString requestRevision;
    Status status;
};

enum class ReceiptPhase
{
    Unknown,
    Pending,
    SdkApplied,
    FirstSent,
    Failed
};
// Receipt progress belongs to its exact revision, never to the latest request.
ReceiptPhase receiptPhase(const Status& status, const QString& revision);

// Full unsigned 64-bit values never pass through JSON numbers or signed integers.
bool isIdentity(const QString& value,
                const QString& maximum = QStringLiteral("18446744073709551615"),
                bool nonzero = true);
int compareIdentity(const QString& a, const QString& b);
Status parseStatus(const QJsonObject& object);
NetworkStatistics parseNetworkStatistics(const QJsonObject& object, const QString& expectedEpoch);
Submission parseSubmission(const QJsonObject& object);
QJsonObject controlRequest(const Status& status, const QString& requestId, bool bitrate, bool fec,
                           int maximumKbps);
QJsonObject manualRequest(const Status& status, const QString& requestId, int totalKbps);

} // namespace TransportPolicy
