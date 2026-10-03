#pragma once

#include "transportpolicy.h"
#include <functional>
#include <memory>
#include <chrono>

class QThread;

namespace TransportPolicy {

class Transport
{
public:
    virtual ~Transport() = default;
    // Calls must finish within a bounded timeout; production uses NvHTTP's 2 seconds.
    virtual Status query(const QString& sessionId, const QString& connectionEpoch) = 0;
    virtual Submission submit(const QString& path, const QJsonObject& body) = 0;
};
// Authenticated native metadata only wakes reconciliation. Policy values,
// request identities and permission remain owned by paired HTTPS.
struct Notification
{
    QString sessionId;
    QString connectionEpoch;
    QString sequence;
    QString controlEpoch;
    QString acceptedRevision;
    std::optional<QString> appliedRevision;
    std::optional<QString> firstSentRevision;
    std::optional<QString> firstSentFrame;
    QString source;
    QString failure;
    quint16 flags = 0;
    bool valid() const;
    bool advances(const Notification& previous) const;
    bool satisfiedBy(const Status& status) const;
};
struct View
{
    std::optional<Status> status;
    std::optional<QString> requestRevision;
    QString queryError;
    QString requestError;
    bool fresh = false;
    bool submitting = false;
    bool readOnly = false;
    std::optional<std::chrono::steady_clock::time_point> networkDeadline;
    std::optional<NetworkStatistics> networkStatistics(
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) const
    {
        if (!status || status->stopped || !status->networkStatistics)
            return std::nullopt;
        auto result = *status->networkStatistics;
        if (result.reason == QStringLiteral("valid") &&
            (!queryError.isEmpty() || !networkDeadline || now >= *networkDeadline)) {
            result.rawLossPercent.reset();
            result.fresh = false;
            result.reason =
                networkDeadline ? QStringLiteral("feedback_stale") : QStringLiteral("unavailable");
        }
        return result;
    }
    bool canSubmit() const
    {
        return !readOnly && fresh && !submitting && status && !status->stopped &&
               status->liveControlAvailable;
    }
};

// A dedicated worker owns HTTP and the version mirror. The stream/UI owner
// reads value snapshots; the worker never accesses Session or NvComputer.
class Controller
{
public:
    using Factory = std::function<std::unique_ptr<Transport>()>;
    Controller(QString sessionId, Factory factory, int pollIntervalMs = 1000, bool readOnly = false,
               QString expectedEpoch = {});
    ~Controller();
    Controller(const Controller&) = delete;
    Controller& operator=(const Controller&) = delete;
    View view() const;
    bool setModes(bool bitrate, bool fec, int maximumKbps);
    bool setManualBudget(int totalKbps);
    bool receiveNotification(const Notification& notification);
    void stop();

private:
    struct Shared;
    struct Command
    {
        bool manual;
        bool bitrate;
        bool fec;
        int totalKbps;
    };
    bool submit(Command command);
    std::shared_ptr<Shared> m_Shared;
    std::unique_ptr<QThread> m_Thread;
};
} // namespace TransportPolicy
