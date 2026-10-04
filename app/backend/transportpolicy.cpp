#include "transportpolicy.h"

#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace TransportPolicy {

ReceiptPhase receiptPhase(const Status& status, const QString& revision)
{
    for (const auto& receipt : status.receipts) {
        if (receipt.policy.revision != revision)
            continue;
        if (receipt.failure != QStringLiteral("none"))
            return ReceiptPhase::Failed;
        if (receipt.encoderApplied && receipt.firstSentFrame)
            return ReceiptPhase::FirstSent;
        if (receipt.encoderApplied)
            return ReceiptPhase::SdkApplied;
        return ReceiptPhase::Pending;
    }
    return ReceiptPhase::Unknown;
}
namespace {
void require(bool condition, const char* message)
{
    if (!condition)
        throw std::invalid_argument(message);
}
QString identity(const QJsonObject& j, const char* key, bool nonzero = true)
{
    auto value = j.value(QLatin1String(key));
    require(value.isString() &&
                isIdentity(value.toString(), QStringLiteral("18446744073709551615"), nonzero),
            "Invalid decimal identity");
    return value.toString();
}
int integer(const QJsonObject& j, const char* key, int minimum = 0, int maximum = 800000)
{
    auto value = j.value(QLatin1String(key));
    double n = value.toDouble();
    require(value.isDouble() && std::isfinite(n) && std::trunc(n) == n && n >= minimum &&
                n <= maximum,
            "Invalid integer field");
    return static_cast<int>(n);
}
bool boolean(const QJsonObject& j, const char* key)
{
    auto value = j.value(QLatin1String(key));
    require(value.isBool(), "Invalid boolean field");
    return value.toBool();
}
QJsonObject object(const QJsonObject& j, const char* key)
{
    auto value = j.value(QLatin1String(key));
    require(value.isObject(), "Invalid object field");
    return value.toObject();
}
bool requestIdentity(const QString& id)
{
    static const QRegularExpression pattern(QStringLiteral("\\A[A-Za-z0-9_-]{1,128}\\z"));
    return pattern.match(id).hasMatch();
}
Policy policy(const QJsonObject& j)
{
    auto f = object(j, "fec");
    auto r = object(j, "reservesKbps");
    std::optional<AutomaticControl> automatic;
    require(j.contains(QStringLiteral("automaticControl")), "Missing automatic control state");
    if (!j.value(QStringLiteral("automaticControl")).isNull()) {
        auto a = object(j, "automaticControl");
        identity(a, "activationEpoch", false);
        automatic = AutomaticControl{ boolean(a, "automaticBitrate"), boolean(a, "automaticFec"),
                                      integer(a, "maximumTotalKbps", 1) };
    }
    auto source = j.value(QStringLiteral("controlSource"));
    require(source.isString() && (source.toString() == "legacy" || source.toString() == "manual" ||
                                  source.toString() == "googcc" || source.toString() == "local"),
            "Invalid control source");
    auto immutable = j;
    immutable.remove(QStringLiteral("encoderApplied"));
    immutable.remove(QStringLiteral("firstSentFrame"));
    immutable.remove(QStringLiteral("failure"));
    return { identity(j, "revision"),
             identity(j, "controlEpoch"),
             source.toString(),
             integer(j, "wireBudgetKbps", 1),
             integer(j, "encoderKbps", 1),
             integer(f, "base", 0, 100),
             integer(f, "key", 0, 100),
             integer(f, "recovery", 0, 100),
             integer(r, "otherTraffic"),
             integer(r, "repair"),
             integer(r, "probe"),
             integer(r, "videoOverhead"),
             automatic,
             immutable };
}
QJsonObject requestBase(const Status& s, const QString& id)
{
    require(!s.stopped && s.liveControlAvailable && requestIdentity(id),
            "Live control unavailable or invalid request identity");
    return { { "version", 2 },
             { "sessionId", s.sessionId },
             { "connectionEpoch", s.connectionEpoch },
             { "controlEpoch", s.controlEpoch },
             { "expectedRevision", s.accepted.revision },
             { "requestId", id } };
}
} // namespace

int compareIdentity(const QString& a, const QString& b)
{
    if (a.size() != b.size())
        return a.size() < b.size() ? -1 : 1;
    return QString::compare(a, b, Qt::CaseSensitive);
}
bool isIdentity(const QString& value, const QString& maximum, bool nonzero)
{
    if (value.isEmpty() || value.size() > maximum.size() ||
        (value.size() > 1 && value.front() == QChar('0')) ||
        (nonzero && value == QStringLiteral("0")))
        return false;
    for (const auto ch : value)
        if (ch < QChar('0') || ch > QChar('9'))
            return false;
    return compareIdentity(value, maximum) <= 0;
}
NetworkStatistics parseNetworkStatistics(const QJsonObject& j, const QString& expectedEpoch)
{
    const int version = integer(j, "version", 1, 2);
    NetworkStatistics result;
    result.connectionEpoch = identity(j, "connectionEpoch");
    require(result.connectionEpoch == expectedEpoch, "Network connection changed");
    result.receiverClockEpoch = identity(j, "receiverClockEpoch", false);
    const auto time = [&](const char* key) -> std::optional<QString> {
        require(j.contains(QLatin1String(key)), "Missing network time");
        return j.value(QLatin1String(key)).isNull()
                   ? std::nullopt
                   : std::optional<QString>(identity(j, key, false));
    };
    result.sampleTimeUs = time("sampleTimeUs");
    const auto begin = time("windowBeginUs"), end = time("windowEndUs");
    require(bool(begin) == bool(end), "Partial network window");
    if (begin)
        require(result.sampleTimeUs && compareIdentity(*begin, *end) <= 0 &&
                    compareIdentity(*end, *result.sampleTimeUs) <= 0,
                "Invalid network window");
    if (begin) {
        const auto duration = end->toULongLong() - begin->toULongLong();
        require(duration <= 5000000, "Unbounded network window");
        result.windowDurationMs = static_cast<int>(duration / 1000);
    }
    const auto count = [&](const char* key) {
        const auto text = identity(j, key, false);
        require(isIdentity(text, version == 1 ? QStringLiteral("4096") : QStringLiteral("32768"),
                           false),
                "Unbounded network sample");
        return text.toInt();
    };
    result.receivedPackets = count("receivedPackets");
    result.missingPackets = count("missingPackets");
    result.unknownPackets = count("unknownPackets");
    const int sampled = count("sampledPackets");
    require(sampled == result.receivedPackets + result.missingPackets + result.unknownPackets,
            "Inconsistent network sample count");
    result.committedPackets = identity(j, "committedPackets", false);
    result.committedIpBytes = identity(j, "committedIpBytes", false);
    result.missingDeclarations = identity(j, "missingDeclarations", false);
    result.lateCorrections = identity(j, "lateCorrections", false);
    result.unresolvedEvictions = identity(j, "unresolvedEvictions", false);
    result.fresh = boolean(j, "fresh");
    require(version == 1 || j.contains(QStringLiteral("freshnessRemainingUs")),
            "Missing statistics lifetime");
    if (j.contains(QStringLiteral("freshnessRemainingUs"))) {
        const auto remaining = identity(j, "freshnessRemainingUs", false);
        require(isIdentity(remaining, QStringLiteral("1000000"), false),
                "Invalid statistics lifetime");
        result.freshnessRemainingUs = remaining.toInt();
        require(result.fresh || *result.freshnessRemainingUs == 0,
                "Stale statistics cannot retain a lifetime");
    }
    result.historyTruncated = boolean(j, "historyTruncated");
    require(j.value(QStringLiteral("reason")).isString(), "Missing network availability");
    result.reason = j.value(QStringLiteral("reason")).toString();
    require(QStringList{ "valid", "not_negotiated", "unavailable", "no_samples", "feedback_stale",
                         "history_truncated", "coverage_incomplete" }
                .contains(result.reason),
            "Unknown network availability");
    const auto percent = [&](const char* key) -> std::optional<double> {
        const auto value = j.value(QLatin1String(key));
        if (value.isNull())
            return std::nullopt;
        require(value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() >= 0 &&
                    value.toDouble() <= 100,
                "Invalid network percentage");
        return value.toDouble();
    };
    result.rawLossPercent = percent("rawLossPercent");
    result.coveragePercent = percent("coveragePercent");
    if (result.reason == QStringLiteral("valid")) {
        require(begin && result.fresh && !result.historyTruncated && result.unknownPackets == 0 &&
                    sampled > 0 && result.receiverClockEpoch != QStringLiteral("0") &&
                    result.rawLossPercent &&
                    std::abs(*result.rawLossPercent - result.missingPackets * 100.0 / sampled) <
                        1e-8,
                "Loss rate lacks complete fresh coverage");
    } else
        require(!result.rawLossPercent, "Unavailable loss rate must be null");
    if (begin && sampled > 0 && !result.historyTruncated) {
        require(result.coveragePercent &&
                    std::abs(*result.coveragePercent -
                             (result.receivedPackets + result.missingPackets) * 100.0 / sampled) <
                        1e-8,
                "Inconsistent network coverage");
    } else
        require(!result.coveragePercent, "Unknown coverage must be null");
    return result;
}
Status parseStatus(const QJsonObject& j)
{
    integer(j, "version", 2, 2);
    auto session = identity(j, "sessionId");
    require(isIdentity(session, QStringLiteral("4294967295")), "Invalid session identity");
    auto epoch = identity(j, "controlEpoch");
    auto accepted = policy(object(j, "accepted"));
    require(accepted.revision == identity(j, "acceptedRevision") &&
                accepted.controlEpoch == epoch &&
                j.value(QStringLiteral("controlSource")).isString() &&
                j.value(QStringLiteral("controlSource")).toString() == accepted.source,
            "Inconsistent accepted policy");
    std::optional<Policy> confirmed;
    if (!j.value(QStringLiteral("confirmed")).isNull())
        confirmed = policy(object(j, "confirmed"));
    require(j.contains(QStringLiteral("confirmed")) &&
                j.contains(QStringLiteral("encoderAppliedRevision")),
            "Missing encoder state");
    if (confirmed) {
        require(confirmed->revision == identity(j, "encoderAppliedRevision") &&
                    compareIdentity(confirmed->revision, accepted.revision) <= 0,
                "Inconsistent encoder policy");
    } else
        require(j.value(QStringLiteral("encoderAppliedRevision")).isNull(),
                "Unexpected applied revision");
    require(j.value(QStringLiteral("receipts")).isArray(), "Invalid receipts");
    auto rows = j.value(QStringLiteral("receipts")).toArray();
    require(rows.size() <= 32, "Receipt limit exceeded");
    QVector<Receipt> receipts;
    QSet<QString> seen;
    bool confirmedReceipt = !confirmed;
    for (const auto row : rows) {
        require(row.isObject(), "Invalid receipt");
        auto r = row.toObject();
        auto p = policy(r);
        require(compareIdentity(p.revision, accepted.revision) <= 0 && !seen.contains(p.revision),
                "Inconsistent receipt revision");
        seen.insert(p.revision);
        auto failure = r.value(QStringLiteral("failure"));
        require(failure.isString() &&
                    (failure.toString() == "none" || failure.toString() == "unsupported" ||
                     failure.toString() == "backend_failure" ||
                     failure.toString() == "superseded" || failure.toString() == "stopped"),
                "Invalid receipt failure");
        bool applied = boolean(r, "encoderApplied");
        std::optional<QString> sent;
        require(r.contains(QStringLiteral("firstSentFrame")), "Missing send state");
        if (!r.value(QStringLiteral("firstSentFrame")).isNull())
            sent = identity(r, "firstSentFrame", false);
        if (confirmed && p.immutableFields == confirmed->immutableFields && applied)
            confirmedReceipt = true;
        receipts.push_back({ std::move(p), applied, sent, failure.toString() });
    }
    require(confirmedReceipt, "Confirmed policy has no application receipt");
    Status result{ session,
                   identity(j, "connectionEpoch"),
                   epoch,
                   std::move(accepted),
                   confirmed,
                   boolean(j, "encoderReady"),
                   boolean(j, "pending"),
                   boolean(j, "stopped"),
                   boolean(j, "experimentalLiveControlAvailable"),
                   std::move(receipts) };
    const auto fecCapability = j.value(QStringLiteral("experimentalAutomaticFecAvailable"));
    result.automaticFecAvailable = fecCapability.isBool() && fecCapability.toBool();
    // Invalid/unsupported optional statistics cannot invalidate a valid control
    // receipt, and can never become a numeric zero loss in the application.
    if (j.value(QStringLiteral("networkStatistics")).isObject()) {
        try {
            result.networkStatistics = parseNetworkStatistics(
                j.value(QStringLiteral("networkStatistics")).toObject(), result.connectionEpoch);
        } catch (const std::invalid_argument&) {
        }
    }
    return result;
}
Submission parseSubmission(const QJsonObject& j)
{
    auto id = j.value(QStringLiteral("requestId"));
    require(id.isString() && requestIdentity(id.toString()), "Invalid request identity");
    auto revision = identity(j, "requestRevision");
    auto status = parseStatus(j);
    require(compareIdentity(revision, status.accepted.revision) <= 0,
            "Unexpected request revision");
    return { id.toString(), revision, std::move(status) };
}
QJsonObject controlRequest(const Status& s, const QString& id, bool bitrate, bool fec,
                           int maximumKbps)
{
    require(maximumKbps >= 1 && maximumKbps <= 800000, "Invalid total budget limit");
    require(!fec || s.automaticFecAvailable, "Automatic FEC unavailable");
    auto request = requestBase(s, id);
    request.insert(QStringLiteral("automaticBitrate"), bitrate);
    request.insert(QStringLiteral("automaticFec"), fec);
    request.insert(QStringLiteral("maximumTotalKbps"), maximumKbps);
    return request;
}
QJsonObject manualRequest(const Status& s, const QString& id, int totalKbps)
{
    require(totalKbps >= 1 && totalKbps <= 800000, "Invalid total budget");
    auto request = requestBase(s, id);
    const auto& p = s.accepted;
    request.insert(QStringLiteral("budget"),
                   QJsonObject{ { "totalKbps", totalKbps },
                                { "otherTrafficKbps", p.otherKbps },
                                { "repairReserveKbps", p.repairKbps },
                                { "probeReserveKbps", p.probeKbps },
                                { "videoOverheadKbps", p.overheadKbps } });
    request.insert(
        QStringLiteral("fec"),
        QJsonObject{ { "base", p.fecBase }, { "key", p.fecKey }, { "recovery", p.fecRecovery } });
    return request;
}
} // namespace TransportPolicy
