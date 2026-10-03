#pragma once
#include "backend/transportpolicycontroller.h"
#include <QElapsedTimer>
#include <QJsonArray>
#include <QString>
#include <functional>

class OverlayMenuPanel;

// Bounded metadata trace, compiled only with the developer Session driver.
// Worker transport and owner receive timestamps use the same monotonic clock.
void recordTransportNotificationEvent(const QString& directory, QJsonObject event);

// Application observations only. The external paired harness independently
// validates actual host SDK/first-send receipts; this is not a policy View.
struct LegacySessionObservation
{
    QString sessionId;
    QString connectionEpoch;
    int configuredBitrateKbps = 0;
    int acknowledgedBitrateKbps = 0;
    bool requestInFlight = false;
    bool receivedVideo = false;
};

// Developer build only. Drives the production panel within its real Session.
// No external UI injection, fake HTTP, or fabricated application receipts.
class TransportPolicySessionDriver
{
public:
    TransportPolicySessionDriver(OverlayMenuPanel& panel, QString output,
                                 std::function<void()> showMenu);
    void tick(const TransportPolicy::View& view);
    void tickLegacy(const LegacySessionObservation& observation);
    void tickStatistics(const TransportPolicy::View& view,
                        const LegacySessionObservation& observation);
    void tickNotifications(const TransportPolicy::View& view,
                           const LegacySessionObservation& observation);
    bool legacyMode() const { return m_LegacyMode; }
    bool statisticsMode() const { return m_StatisticsMode; }
    bool notificationsMode() const { return m_NotificationsMode; }
    QString notificationOutput() const { return m_NotificationsMode ? m_Output : QString(); }
    bool takeExitRequest();
    bool takeOwnerReconnectRequest();

private:
    void finish(bool passed, QString error = {});
    void save();
    bool recordStep(const TransportPolicy::View& view, bool reconnected = false);
    QJsonObject snapshot(const TransportPolicy::View& view) const;
    bool recordLegacyStep(const LegacySessionObservation& observation, bool reconnected = false);
    void requestOwnedHostRestart(const QString& epoch, int budget);
    OverlayMenuPanel& m_Panel;
    QString m_Output;
    std::function<void()> m_ShowMenu;
    QElapsedTimer m_Clock;
    QJsonArray m_Steps;
    QJsonArray m_Results;
    QJsonObject m_FaultInFlightObservation;
    QJsonObject m_RequestErrorObservation;
    QJsonObject m_RequestRecoveryObservation;
    int m_Index = 0;
    bool m_Processing = false;
    bool m_Waiting = false;
    bool m_WaitingReconnect = false;
    bool m_Finished = false;
    bool m_ExitRequested = false;
    bool m_ExpectedBitrate = false;
    bool m_ExpectedFec = false;
    bool m_LegacyMode = false;
    bool m_StatisticsMode = false;
    bool m_NotificationsMode = false;
    std::optional<std::chrono::steady_clock::time_point> m_StatisticsDeadline;
    bool m_OwnerReconnectRequested = false;
    int m_ExpectedBudget = 0;
    QString m_PreviousRequest;
    QString m_PreviousEpoch;
    qint64 m_ActionAt = 0;
};
