#pragma once

#include "transportpolicy.h"

namespace TransportPolicy {

// Confined to one client owner. HTTP errors invalidate write eligibility;
// they never grant a new connection or fabricate an applied encoder target.
class Mirror
{
public:
    explicit Mirror(QString sessionId);
    void accept(Status next);
    void acknowledge(Submission reply, const QString& expectedRequestId);
    void invalidate();
    void stop();
    bool canSubmit() const;
    const QString& connectionEpoch() const { return m_ConnectionEpoch; }
    const std::optional<Status>& state() const { return m_State; }
    const std::optional<QString>& requestRevision() const { return m_RequestRevision; }
    void clearRequestOutcome() { m_RequestRevision.reset(); }

private:
    QString m_SessionId;
    QString m_ConnectionEpoch;
    std::optional<Status> m_State;
    std::optional<QString> m_RequestRevision;
    bool m_Fresh = false;
    bool m_Stopped = false;
};

} // namespace TransportPolicy
