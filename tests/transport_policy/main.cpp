#include "backend/transportpolicy.h"
#include "backend/transportpolicymirror.h"
#include "backend/transportpolicycontroller.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QtTest>
#include <stdexcept>
#include <atomic>
#include <mutex>
#if QT_VERSION >= QT_VERSION_CHECK(6, 3, 0)
#define EXPECT_INVALID(expr) QVERIFY_THROWS_EXCEPTION(std::invalid_argument, expr)
#else
#define EXPECT_INVALID(expr) QVERIFY_EXCEPTION_THROWN(expr, std::invalid_argument)
#endif

struct FakeState
{
    TransportPolicy::Status status;
    std::atomic_int queries{ 0 };
    std::atomic_int completedQueries{ 0 };
    std::atomic_int writes{ 0 };
    std::atomic_bool failWrite{ false };
    std::atomic_int queryDelayMs{ 0 };
    std::mutex mutex;
    QStringList epochs;
    QString lastPath;
    QJsonObject lastBody;
};
class FakeTransport : public TransportPolicy::Transport
{
public:
    explicit FakeTransport(std::shared_ptr<FakeState> state) : m_State(std::move(state)) {}
    TransportPolicy::Status query(const QString&, const QString& epoch) override
    {
        TransportPolicy::Status snapshot;
        {
            std::lock_guard<std::mutex> lock(m_State->mutex);
            m_State->epochs.push_back(epoch);
            snapshot = m_State->status;
        }
        m_State->queries++;
        QThread::msleep(static_cast<unsigned long>(m_State->queryDelayMs.load()));
        m_State->completedQueries++;
        return snapshot;
    }
    TransportPolicy::Submission submit(const QString& path, const QJsonObject& body) override
    {
        {
            std::lock_guard<std::mutex> lock(m_State->mutex);
            m_State->lastPath = path;
            m_State->lastBody = body;
        }
        m_State->writes++;
        if (m_State->failWrite)
            throw std::runtime_error("Request outcome unconfirmed");
        return { body.value("requestId").toString(), m_State->status.accepted.revision,
                 m_State->status };
    }

private:
    std::shared_ptr<FakeState> m_State;
};

class TransportPolicyTest : public QObject
{
    Q_OBJECT
    QJsonObject sample(const char* path)
    {
        QFile file(QFINDTESTDATA(path));
        if (!file.open(QIODevice::ReadOnly))
            qFatal("Captured policy sample is missing");
        auto j = QJsonDocument::fromJson(file.readAll());
        if (!j.isObject())
            qFatal("Malformed captured fixture");
        return j.object();
    }
    TransportPolicy::Notification notice(const TransportPolicy::Status& s,
                                         const QString& sequence = QStringLiteral("1"))
    {
        TransportPolicy::Notification n;
        n.sessionId = s.sessionId;
        n.connectionEpoch = s.connectionEpoch;
        n.sequence = sequence;
        n.controlEpoch = s.controlEpoch;
        n.acceptedRevision = s.accepted.revision;
        n.source = s.accepted.source;
        n.failure = QStringLiteral("none");
        n.flags = (s.encoderReady ? 8 : 0) | (s.pending ? 4 : 0);
        if (s.confirmed) {
            n.appliedRevision = s.confirmed->revision;
            n.flags |= 1;
        }
        for (const auto& r : s.receipts) {
            if (r.firstSentFrame &&
                (!n.firstSentRevision ||
                 TransportPolicy::compareIdentity(r.policy.revision, *n.firstSentRevision) > 0)) {
                n.firstSentRevision = r.policy.revision;
                n.firstSentFrame = r.firstSentFrame;
                n.flags |= 2;
            }
            if (r.policy.revision == s.accepted.revision)
                n.failure = r.failure;
        }
        return n;
    }
private slots:
    void notificationsKeepFullUnsignedValuesAndRejectMalformedProgress()
    {
        auto n = notice(TransportPolicy::parseStatus(sample("samples/policy-final.json")));
        n.connectionEpoch = n.sequence = QStringLiteral("18446744073709551615");
        n.firstSentFrame = QStringLiteral("0");
        QVERIFY(n.valid());
        for (int i = 0; i < 8; ++i) {
            auto bad = n;
            switch (i) {
            case 0:
                bad.sequence = "0";
                break;
            case 1:
                bad.connectionEpoch = "18446744073709551616";
                break;
            case 2:
                bad.flags |= 128;
                break;
            case 3:
                bad.appliedRevision.reset();
                break;
            case 4:
                bad.firstSentFrame.reset();
                break;
            case 5:
                bad.source = "unknown";
                break;
            case 6:
                bad.failure = "unknown";
                break;
            case 7:
                bad.flags |= 4;
                break;
            }
            QVERIFY(!bad.valid());
        }
    }
    void notificationWaterlinesRejectReorderingIdentityAndRevival()
    {
        const auto n = notice(TransportPolicy::parseStatus(sample("samples/policy-final.json")));
        QVERIFY(n.valid());
        QVERIFY(!n.advances(n));
        auto next = n;
        next.sequence = "2";
        QVERIFY(next.advances(n));
        auto wrong = next;
        wrong.sessionId = "2";
        QVERIFY(!wrong.advances(n));
        wrong = next;
        wrong.connectionEpoch = "42";
        QVERIFY(!wrong.advances(n));
        wrong = next;
        wrong.source = "manual";
        QVERIFY(!wrong.advances(n));
        wrong = next;
        wrong.flags &= ~8;
        QVERIFY(wrong.advances(n));
        wrong = next;
        wrong.firstSentFrame =
            *n.firstSentFrame == QStringLiteral("0") ? QStringLiteral("1") : QStringLiteral("0");
        QVERIFY(!wrong.advances(n));
        auto stopped = next;
        stopped.flags = (stopped.flags & 3) | 16;
        QVERIFY(stopped.advances(n));
        next.sequence = "3";
        QVERIFY(!next.advances(stopped));
    }
    void boundedQueryHistoryDoesNotEraseNoticeProgressOrWaitForever()
    {
        auto s = TransportPolicy::parseStatus(sample("samples/policy-final.json"));
        auto n = notice(s);
        QVERIFY(n.satisfiedBy(s));
        n.firstSentRevision = "1";
        n.firstSentFrame = "0";
        for (int i = s.receipts.size() - 1; i >= 0; --i)
            if (s.receipts[i].policy.revision == "1")
                s.receipts.remove(i);
        QVERIFY(n.valid());
        QVERIFY(n.satisfiedBy(s));
        n.firstSentRevision = s.accepted.revision;
        s.receipts.clear();
        QVERIFY(!n.satisfiedBy(s));
    }
    void encoderReadinessCanChangeWithoutErasingHistoricalApplication()
    {
        auto s = TransportPolicy::parseStatus(sample("samples/policy-final.json"));
        const auto ready = notice(s);
        s.encoderReady = false;
        auto rebuilding = notice(s, "2");
        QVERIFY(rebuilding.valid());
        QVERIFY(rebuilding.advances(ready));
        QVERIFY(ready.satisfiedBy(s));
        QVERIFY(rebuilding.satisfiedBy(s));
        QCOMPARE(rebuilding.appliedRevision, ready.appliedRevision);
        QCOMPARE(rebuilding.firstSentFrame, ready.firstSentFrame);
        auto unconfirmed = s;
        unconfirmed.confirmed.reset();
        QVERIFY(!rebuilding.satisfiedBy(unconfirmed));
    }
    void authenticatedNoticeWakesAnInFlightQueryAndPreventsStaleActionability()
    {
        auto state = std::make_shared<FakeState>();
        state->status = TransportPolicy::parseStatus(sample("samples/policy-final.json"));
        const auto old = state->status;
        state->queryDelayMs = 200;
        TransportPolicy::Controller controller(
            old.sessionId, [state] { return std::make_unique<FakeTransport>(state); }, 10000, false,
            old.connectionEpoch);
        QTRY_VERIFY(state->queries.load() == 1);
        auto n = notice(old);
        n.sequence = "2";
        n.acceptedRevision = "27";
        QVERIFY(controller.receiveNotification(n));
        QVERIFY(!controller.view().canSubmit());
        QVERIFY(!controller.setManualBudget(5000));
        QTRY_VERIFY(state->completedQueries.load() >= 1);
        QVERIFY(!controller.view().queryError.isEmpty());
        QVERIFY(!controller.view().status);
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->status.accepted.revision = "27";
            state->status.accepted.immutableFields.insert("revision", "27");
            state->status.pending = true;
        }
        auto latest = n;
        latest.sequence = "3";
        QVERIFY(controller.receiveNotification(latest));
        QVERIFY(!controller.receiveNotification(latest));
        QTRY_VERIFY(controller.view().canSubmit());
        QCOMPARE(controller.view().status->accepted.revision, QStringLiteral("27"));
        QVERIFY(state->queries.load() < 5);
        QCOMPARE(state->writes.load(), 0);
        controller.stop();
        latest.sequence = "4";
        QVERIFY(!controller.receiveNotification(latest));
        QVERIFY(!controller.view().status);
    }
    void notificationCannotDiscoverOwnershipOrGrantReadOnlyControl()
    {
        auto state = std::make_shared<FakeState>();
        state->status = TransportPolicy::parseStatus(sample("samples/policy-final.json"));
        const auto n = notice(state->status);
        state->queryDelayMs = 200;
        TransportPolicy::Controller discover(
            n.sessionId, [state] { return std::make_unique<FakeTransport>(state); }, 10000);
        QVERIFY(!discover.receiveNotification(n));
        discover.stop();
        state->queryDelayMs = 0;
        TransportPolicy::Controller readOnly(
            n.sessionId, [state] { return std::make_unique<FakeTransport>(state); }, 10000, true,
            n.connectionEpoch);
        QTRY_VERIFY(readOnly.view().status);
        auto wrong = n;
        wrong.connectionEpoch = "42";
        QVERIFY(!readOnly.receiveNotification(wrong));
        const int queries = state->queries.load();
        QVERIFY(readOnly.receiveNotification(n));
        QTRY_VERIFY(state->queries.load() > queries);
        QVERIFY(!readOnly.view().canSubmit());
        QVERIFY(!readOnly.setModes(true, true, 5000));
        QCOMPARE(state->writes.load(), 0);
    }
    void statisticsV2AcceptsCompleteHighRateCountsAndRequiresTheLifetimeContract()
    {
        auto statistics = sample("samples/network-statistics-v1.json")
                              .value("cases")
                              .toArray()
                              .first()
                              .toObject()
                              .value("statistics")
                              .toObject();
        const auto epoch = statistics.value("connectionEpoch").toString();
        statistics.insert("version", 2);
        statistics.insert("receivedPackets", QStringLiteral("26731"));
        statistics.insert("missingPackets", QStringLiteral("270"));
        statistics.insert("sampledPackets", QStringLiteral("27001"));
        statistics.insert("rawLossPercent", 270 * 100.0 / 27001);
        statistics.insert("freshnessRemainingUs", QStringLiteral("500000"));
        const auto result = TransportPolicy::parseNetworkStatistics(statistics, epoch);
        QCOMPARE(result.receivedPackets, 26731);
        QCOMPARE(result.missingPackets, 270);
        QCOMPARE(*result.freshnessRemainingUs, 500000);
        statistics.remove("freshnessRemainingUs");
        EXPECT_INVALID(TransportPolicy::parseNetworkStatistics(statistics, epoch));
        statistics.insert("freshnessRemainingUs", QStringLiteral("500000"));
        statistics.insert("version", 1);
        EXPECT_INVALID(TransportPolicy::parseNetworkStatistics(statistics, epoch));
    }
    void relativeStatisticsExpiryDoesNotChangeThePrimaryControlReceipt()
    {
        auto source = sample("samples/policy-final.json");
        auto statistics = sample("samples/network-statistics-v1.json")
                              .value("cases")
                              .toArray()
                              .first()
                              .toObject()
                              .value("statistics")
                              .toObject();
        statistics.insert("connectionEpoch", source.value("connectionEpoch"));
        statistics.insert("freshnessRemainingUs", QStringLiteral("500000"));
        source.insert("networkStatistics", statistics);
        TransportPolicy::View view;
        view.status = TransportPolicy::parseStatus(source);
        view.fresh = true;
        const auto begin = std::chrono::steady_clock::now();
        view.networkDeadline = begin + std::chrono::microseconds(500000);
        QVERIFY(view.networkStatistics(begin + std::chrono::microseconds(499999))->rawLossPercent);
        QVERIFY(!view.networkStatistics(begin + std::chrono::microseconds(500000))->rawLossPercent);
        QCOMPARE(view.networkStatistics(begin + std::chrono::seconds(10))->reason,
                 QStringLiteral("feedback_stale"));
        QVERIFY(view.canSubmit());
        QVERIFY(view.status->networkStatistics->rawLossPercent);
        view.networkDeadline.reset();
        QVERIFY(!view.networkStatistics(begin)->rawLossPercent);
        view.queryError = QStringLiteral("HTTP failed");
        QVERIFY(!view.networkStatistics(begin)->rawLossPercent);
        statistics.insert("freshnessRemainingUs", QStringLiteral("1000001"));
        source.insert("networkStatistics", statistics);
        QVERIFY(!TransportPolicy::parseStatus(source).networkStatistics);
    }
    void readOnlyWorkerPinsTheLaunchEpochAndDeductsHttpTime()
    {
        auto state = std::make_shared<FakeState>();
        auto source = sample("samples/policy-final.json");
        auto statistics = sample("samples/network-statistics-v1.json")
                              .value("cases")
                              .toArray()
                              .first()
                              .toObject()
                              .value("statistics")
                              .toObject();
        statistics.insert("connectionEpoch", source.value("connectionEpoch"));
        statistics.insert("freshnessRemainingUs", QStringLiteral("10000"));
        source.insert("networkStatistics", statistics);
        state->status = TransportPolicy::parseStatus(source);
        state->queryDelayMs = 50;
        TransportPolicy::Controller controller(
            state->status.sessionId, [state] { return std::make_unique<FakeTransport>(state); },
            10000, true, state->status.connectionEpoch);
        QTRY_VERIFY(controller.view().status);
        QVERIFY(!controller.view().networkStatistics()->rawLossPercent);
        QVERIFY(!controller.view().canSubmit());
        QVERIFY(!controller.setManualBudget(5000));
        QVERIFY(!controller.setModes(true, true, 5000));
        QCOMPARE(state->writes.load(), 0);
        QCOMPARE(state->epochs.first(), state->status.connectionEpoch);
        controller.stop();
        QVERIFY(!controller.view().networkStatistics());
        TransportPolicy::Controller wrong(
            state->status.sessionId, [state] { return std::make_unique<FakeTransport>(state); },
            10000, true, QStringLiteral("42"));
        QTRY_VERIFY(!wrong.view().queryError.isEmpty());
        QVERIFY(!wrong.view().status);
    }
    void actualNetworkStatisticsPreserveTheHostAvailabilityAndControlBoundary()
    {
        const auto fixtures = sample("samples/network-statistics-runtime-v1.json");
        const auto cases = fixtures.value("cases").toArray();
        QCOMPARE(cases.size(), 11);
        bool valid = false;
        bool unavailable = false;
        for (const auto value : cases) {
            const auto source = value.toObject().value("status").toObject();
            const auto status = TransportPolicy::parseStatus(source);
            QVERIFY(status.networkStatistics);
            const auto& statistics = *status.networkStatistics;
            const auto raw = source.value("networkStatistics").toObject();
            QCOMPARE(statistics.connectionEpoch, status.connectionEpoch);
            QCOMPARE(statistics.reason, raw.value("reason").toString());
            QCOMPARE(statistics.committedPackets, raw.value("committedPackets").toString());
            QVERIFY(!status.liveControlAvailable);
            QCOMPARE(status.accepted.source, QStringLiteral("legacy"));
            if (statistics.reason == QStringLiteral("valid")) {
                valid = true;
                QVERIFY(statistics.rawLossPercent);
                QCOMPARE(*statistics.rawLossPercent, raw.value("rawLossPercent").toDouble());
            } else {
                unavailable = true;
                QVERIFY(!statistics.rawLossPercent);
            }
        }
        QVERIFY(valid && unavailable);
    }
    void sharedNetworkStatisticsKeepCoverageAndIdentityExact()
    {
        const auto fixtures = sample("samples/network-statistics-v1.json");
        const auto epoch = fixtures.value("expectedEpoch").toString();
        const auto cases = fixtures.value("cases").toArray();
        QCOMPARE(cases.size(), 19);
        for (const auto value : cases) {
            const auto item = value.toObject();
            bool accepted = false;
            try {
                const auto result = TransportPolicy::parseNetworkStatistics(
                    item.value("statistics").toObject(), epoch);
                accepted = true;
                QCOMPARE(result.committedPackets, QStringLiteral("18446744073709551615"));
                QCOMPARE(result.committedIpBytes, QStringLiteral("9007199254740993"));
                if (item.value("loss").isNull())
                    QVERIFY(!result.rawLossPercent);
                else {
                    QVERIFY(result.rawLossPercent);
                    QCOMPARE(*result.rawLossPercent, item.value("loss").toDouble());
                }
            } catch (const std::invalid_argument&) {
            }
            QVERIFY2(accepted == item.value("accepted").toBool(),
                     qPrintable(item.value("name").toString()));
        }
    }
    void optionalNetworkStatisticsNeverCorruptValidControlReceipts()
    {
        auto status = sample("samples/policy-initial.json");
        auto statistics = sample("samples/network-statistics-v1.json")
                              .value("cases")
                              .toArray()
                              .first()
                              .toObject()
                              .value("statistics")
                              .toObject();
        QVERIFY(!TransportPolicy::parseStatus(status).networkStatistics);
        statistics.insert("connectionEpoch", status.value("connectionEpoch"));
        status.insert("networkStatistics", statistics);
        QVERIFY(TransportPolicy::parseStatus(status).networkStatistics);
        const auto revision = TransportPolicy::parseStatus(status).accepted.revision;
        statistics.insert("rawLossPercent", 0.0);
        status.insert("networkStatistics", statistics);
        const auto invalid = TransportPolicy::parseStatus(status);
        QCOMPARE(invalid.accepted.revision, revision);
        QVERIFY(!invalid.networkStatistics);
    }
    void fullUnsignedIdentities()
    {
        QVERIFY(TransportPolicy::isIdentity(QStringLiteral("18446744073709551615")));
        for (const auto& bad : { QString(), QStringLiteral("01"), QStringLiteral(" 1"),
                                 QStringLiteral("1e3"), QStringLiteral("-1"),
                                 QStringLiteral("18446744073709551616"), QStringLiteral("１２") }) {
            QVERIFY(!TransportPolicy::isIdentity(bad));
        }
        QVERIFY(!TransportPolicy::isIdentity(QStringLiteral("0")));
        QVERIFY(TransportPolicy::isIdentity(QStringLiteral("0"),
                                            QStringLiteral("18446744073709551615"), false));
    }
    void actualHostStatesPreserveReceipts()
    {
        for (const auto* name : { "samples/policy-initial.json", "samples/policy-final.json" }) {
            auto j = sample(name);
            auto s = TransportPolicy::parseStatus(j);
            QCOMPARE(s.connectionEpoch, j.value("connectionEpoch").toString());
            QCOMPARE(s.accepted.revision, j.value("acceptedRevision").toString());
            QVERIFY(s.confirmed.has_value());
            QVERIFY(s.encoderReady);
            QVERIFY(s.liveControlAvailable);
            QVERIFY(!s.receipts.isEmpty());
        }
    }
    void fourCombinationsHaveExactFields()
    {
        auto s = TransportPolicy::parseStatus(sample("samples/policy-initial.json"));
        s.automaticFecAvailable = true;
        for (bool bitrate : { false, true })
            for (bool fec : { false, true }) {
                auto j = TransportPolicy::controlRequest(s, "pc-001", bitrate, fec, 7000);
                QCOMPARE(j.size(), 9);
                QVERIFY(j.value("automaticBitrate").isBool());
                QCOMPARE(j.value("automaticBitrate").toBool(), bitrate);
                QCOMPARE(j.value("automaticFec").toBool(), fec);
                QCOMPARE(j.value("maximumTotalKbps").toInt(), 7000);
                QCOMPARE(j.value("connectionEpoch").toString(), s.connectionEpoch);
                QCOMPARE(j.value("expectedRevision").toString(), s.accepted.revision);
                QVERIFY(!j.contains("controlSource"));
                QVERIFY(!j.contains("activationEpoch"));
            }
    }
    void unavailableAutomaticFecFailsClosedWithoutDisablingBitrate()
    {
        auto j = sample("samples/policy-initial.json");
        QVERIFY(!TransportPolicy::parseStatus(j).automaticFecAvailable);
        for (const QJsonValue capability :
             { QJsonValue(false), QJsonValue("true"), QJsonValue(1) }) {
            j["experimentalAutomaticFecAvailable"] = capability;
            auto s = TransportPolicy::parseStatus(j);
            QVERIFY(!s.automaticFecAvailable);
            EXPECT_INVALID(TransportPolicy::controlRequest(s, "pc-1", false, true, 7000));
            QVERIFY(TransportPolicy::controlRequest(s, "pc-2", true, false, 7000)
                        .value("automaticBitrate")
                        .toBool());
        }
        j["experimentalAutomaticFecAvailable"] = true;
        QVERIFY(TransportPolicy::parseStatus(j).automaticFecAvailable);
    }

    void manualPreservesProtectionAndReserves()
    {
        auto s = TransportPolicy::parseStatus(sample("samples/policy-final.json"));
        auto j = TransportPolicy::manualRequest(s, "manual-1", 6500);
        QCOMPARE(j.size(), 8);
        auto b = j.value("budget").toObject();
        QCOMPARE(b.value("totalKbps").toInt(), 6500);
        QCOMPARE(b.value("otherTrafficKbps").toInt(), s.accepted.otherKbps);
        QCOMPARE(b.value("videoOverheadKbps").toInt(), s.accepted.overheadKbps);
        QCOMPARE(j.value("fec").toObject().value("key").toInt(), s.accepted.fecKey);
        QVERIFY(!j.contains("automaticControl"));
    }
    void numericIdentitiesAndBooleanCoercionFail()
    {
        auto j = sample("samples/policy-initial.json");
        j["connectionEpoch"] = 42;
        EXPECT_INVALID(TransportPolicy::parseStatus(j));
        j = sample("samples/policy-initial.json");
        j["experimentalLiveControlAvailable"] = "true";
        EXPECT_INVALID(TransportPolicy::parseStatus(j));
        j = sample("samples/policy-initial.json");
        auto p = j.value("accepted").toObject();
        p["encoderKbps"] = 123.5;
        j["accepted"] = p;
        EXPECT_INVALID(TransportPolicy::parseStatus(j));
        for (const auto* field : { "budgetBasis", "encoderCeilingKbps" }) {
            j = sample("samples/policy-initial.json");
            p = j.value("accepted").toObject();
            p.remove(QLatin1String(field));
            j["accepted"] = p;
            EXPECT_INVALID(TransportPolicy::parseStatus(j));
        }
        for (const QJsonValue ceiling :
             { QJsonValue(0), QJsonValue(800001), QJsonValue(1.5), QJsonValue("1000") }) {
            j = sample("samples/policy-initial.json");
            p = j.value("accepted").toObject();
            p["encoderCeilingKbps"] = ceiling;
            j["accepted"] = p;
            EXPECT_INVALID(TransportPolicy::parseStatus(j));
        }
    }
    void contradictoryAcceptedOrAppliedVersionsFail()
    {
        auto j = sample("samples/policy-initial.json");
        j["acceptedRevision"] = "123";
        EXPECT_INVALID(TransportPolicy::parseStatus(j));
        j = sample("samples/policy-initial.json");
        j["encoderAppliedRevision"] = "123";
        EXPECT_INVALID(TransportPolicy::parseStatus(j));
        j = sample("samples/policy-initial.json");
        j["receipts"] = QJsonArray();
        EXPECT_INVALID(TransportPolicy::parseStatus(j));
        for (int variant = 0; variant < 3; ++variant) {
            j = sample("samples/policy-final.json");
            auto rows = j.value("receipts").toArray();
            auto receipt = rows.first().toObject();
            receipt["encoderApplied"] = variant == 1;
            receipt["firstSentFrame"] = variant == 0 ? QJsonValue("0") : QJsonValue();
            receipt["failure"] = variant == 0 ? "none" : "backend_failure";
            rows[0] = receipt;
            j["receipts"] = rows;
            if (variant < 2)
                EXPECT_INVALID(TransportPolicy::parseStatus(j));
            else
                QVERIFY(!TransportPolicy::parseStatus(j).receipts.first().encoderApplied);
        }
    }
    void contradictoryPoliciesWithTheSameRevisionFail()
    {
        auto j = sample("samples/policy-initial.json");
        auto accepted = j.value("accepted").toObject();
        accepted["wireBudgetKbps"] = accepted.value("wireBudgetKbps").toInt() - 1000;
        j["accepted"] = accepted;
        EXPECT_INVALID(TransportPolicy::parseStatus(j));
        j = sample("samples/policy-initial.json");
        auto rows = j.value("receipts").toArray();
        auto receipt = rows.last().toObject();
        receipt["encoderKbps"] = receipt.value("encoderKbps").toInt() - 1;
        rows[rows.size() - 1] = receipt;
        j["receipts"] = rows;
        EXPECT_INVALID(TransportPolicy::parseStatus(j));
    }
    void duplicateOrFutureReceiptsFail()
    {
        auto j = sample("samples/policy-initial.json");
        auto rows = j.value("receipts").toArray();
        rows.append(rows.first());
        j["receipts"] = rows;
        EXPECT_INVALID(TransportPolicy::parseStatus(j));
        j = sample("samples/policy-initial.json");
        rows = j.value("receipts").toArray();
        auto p = rows.first().toObject();
        p["revision"] = "99999";
        rows[0] = p;
        j["receipts"] = rows;
        EXPECT_INVALID(TransportPolicy::parseStatus(j));
    }
    void stoppedUnavailableAndOutOfRangeRequestsFail()
    {
        auto s = TransportPolicy::parseStatus(sample("samples/policy-initial.json"));
        EXPECT_INVALID(TransportPolicy::controlRequest(s, "pc-1", true, true, 0));
        EXPECT_INVALID(TransportPolicy::manualRequest(s, "pc-1", 800001));
        EXPECT_INVALID(TransportPolicy::manualRequest(s, "bad id", 6500));
        s.stopped = true;
        EXPECT_INVALID(TransportPolicy::manualRequest(s, "pc-1", 6500));
        s.stopped = false;
        s.liveControlAvailable = false;
        EXPECT_INVALID(TransportPolicy::controlRequest(s, "pc-1", true, true, 7000));
    }
    void mirrorRejectsOldConnectionAndStoppedCallbacks()
    {
        auto s = TransportPolicy::parseStatus(sample("samples/policy-initial.json"));
        TransportPolicy::Mirror mirror(s.sessionId);
        QVERIFY(!mirror.canSubmit());
        mirror.accept(s);
        QVERIFY(mirror.canSubmit());
        auto wrong = s;
        wrong.connectionEpoch = "42";
        EXPECT_INVALID(mirror.accept(wrong));
        QCOMPARE(mirror.connectionEpoch(), s.connectionEpoch);
        QVERIFY(!mirror.canSubmit());
        mirror.accept(s);
        mirror.stop();
        EXPECT_INVALID(mirror.accept(s));
        QVERIFY(!mirror.canSubmit());
    }
    void mirrorRejectsRevisionAndControlRegression()
    {
        auto s = TransportPolicy::parseStatus(sample("samples/policy-final.json"));
        TransportPolicy::Mirror mirror(s.sessionId);
        mirror.accept(s);
        auto stale = s;
        stale.accepted.revision = "1";
        EXPECT_INVALID(mirror.accept(stale));
        QCOMPARE(mirror.state()->accepted.revision, s.accepted.revision);
        stale = s;
        stale.controlEpoch = "1";
        EXPECT_INVALID(mirror.accept(stale));
        QVERIFY(!mirror.canSubmit());
    }
    void mirrorNeverInventsEncoderReadinessOrRequestAcknowledgement()
    {
        auto s = TransportPolicy::parseStatus(sample("samples/policy-initial.json"));
        TransportPolicy::Mirror mirror(s.sessionId);
        mirror.accept(s);
        EXPECT_INVALID(mirror.acknowledge({ "different-request", s.accepted.revision, s }, "pc-1"));
        QVERIFY(!mirror.requestRevision());
        QVERIFY(!mirror.canSubmit());
        mirror.accept(s);
        QVERIFY(!mirror.requestRevision());
        mirror.acknowledge({ "pc-1", s.accepted.revision, s }, "pc-1");
        QVERIFY(mirror.requestRevision());
        auto unknown = s;
        unknown.confirmed.reset();
        unknown.encoderReady = false;
        mirror.accept(unknown);
        QVERIFY(!mirror.state()->confirmed);
        QVERIFY(!mirror.state()->encoderReady);
    }
    void mirrorChecksImmutablePoliciesAndReceiptProgression()
    {
        auto s = TransportPolicy::parseStatus(sample("samples/policy-final.json"));
        TransportPolicy::Mirror mirror(s.sessionId);
        mirror.accept(s);
        auto changed = s;
        changed.accepted.immutableFields.insert("wireBudgetKbps", 42);
        EXPECT_INVALID(mirror.accept(changed));
        QVERIFY(!mirror.canSubmit());
        changed = s;
        changed.receipts[0].policy.immutableFields.insert("encoderKbps", 42);
        EXPECT_INVALID(mirror.accept(changed));
        changed = s;
        for (auto& r : changed.receipts)
            if (r.encoderApplied) {
                r.encoderApplied = false;
                break;
            }
        EXPECT_INVALID(mirror.accept(changed));
        changed = s;
        for (auto& r : changed.receipts)
            if (r.firstSentFrame) {
                r.firstSentFrame.reset();
                break;
            }
        EXPECT_INVALID(mirror.accept(changed));
    }
    void mirrorQueryErrorsInvalidateWritesWithoutChangingLastKnownState()
    {
        auto s = TransportPolicy::parseStatus(sample("samples/policy-initial.json"));
        TransportPolicy::Mirror mirror(s.sessionId);
        mirror.accept(s);
        mirror.invalidate();
        QVERIFY(!mirror.canSubmit());
        QCOMPARE(mirror.state()->accepted.revision, s.accepted.revision);
        mirror.accept(s);
        QVERIFY(mirror.canSubmit());
        auto unavailable = s;
        unavailable.liveControlAvailable = false;
        mirror.accept(unavailable);
        QVERIFY(!mirror.canSubmit());
    }
    void workerUsesExplicitModesAndPinsDiscoveredEpoch()
    {
        auto state = std::make_shared<FakeState>();
        state->status = TransportPolicy::parseStatus(sample("samples/policy-initial.json"));
        state->status.automaticFecAvailable = true;
        TransportPolicy::Controller controller(
            state->status.sessionId, [state] { return std::make_unique<FakeTransport>(state); },
            15);
        QTRY_VERIFY(controller.view().canSubmit());
        QVERIFY(controller.setModes(false, true, 7000));
        QTRY_VERIFY(controller.view().requestRevision.has_value());
        QCOMPARE(state->writes.load(), 1);
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            QCOMPARE(state->lastPath, QStringLiteral("api/v2/transport-control"));
            QCOMPARE(state->lastBody.value("automaticBitrate").toBool(), false);
            QCOMPARE(state->lastBody.value("automaticFec").toBool(), true);
            QCOMPARE(state->lastBody.value("maximumTotalKbps").toInt(), 7000);
            QVERIFY(state->epochs.first().isEmpty());
            for (int i = 1; i < state->epochs.size(); ++i)
                QCOMPARE(state->epochs[i], state->status.connectionEpoch);
        }
        controller.stop();
        QVERIFY(!controller.view().canSubmit());
        QVERIFY(!controller.setManualBudget(6000));
    }
    void workerDoesNotRetryAnUnknownWriteOutcome()
    {
        auto state = std::make_shared<FakeState>();
        state->status = TransportPolicy::parseStatus(sample("samples/policy-initial.json"));
        state->failWrite = true;
        TransportPolicy::Controller controller(
            state->status.sessionId, [state] { return std::make_unique<FakeTransport>(state); },
            15);
        QTRY_VERIFY(controller.view().canSubmit());
        QVERIFY(controller.setManualBudget(6500));
        QTRY_VERIFY(!controller.view().requestError.isEmpty());
        QTRY_VERIFY(state->queries.load() >= 4);
        QCOMPARE(state->writes.load(), 1);
        QVERIFY(!controller.view().requestRevision);
        QVERIFY(!controller.view().requestError.isEmpty());
    }
    void workerCannotPublishALateQueryOrWriteAfterStop()
    {
        auto state = std::make_shared<FakeState>();
        state->status = TransportPolicy::parseStatus(sample("samples/policy-initial.json"));
        state->queryDelayMs = 100;
        TransportPolicy::Controller controller(
            state->status.sessionId, [state] { return std::make_unique<FakeTransport>(state); },
            15);
        QTRY_VERIFY(state->queries.load() >= 1);
        controller.stop();
        QVERIFY(!controller.view().status);
        QVERIFY(!controller.view().canSubmit());
        QVERIFY(!controller.setModes(true, true, 7000));
        QCOMPARE(state->writes.load(), 0);
    }
};
QTEST_GUILESS_MAIN(TransportPolicyTest)
#include "main.moc"
