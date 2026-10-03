#include "transportpolicymirror.h"

#include <stdexcept>
#include <utility>

namespace TransportPolicy {
namespace {
void require(bool condition, const char* message)
{
    if (!condition)
        throw std::invalid_argument(message);
}
}
Mirror::Mirror(QString sessionId) : m_SessionId(std::move(sessionId))
{
    require(isIdentity(m_SessionId, QStringLiteral("4294967295")),
            "Invalid client session identity");
}
void Mirror::accept(Status next)
{
    m_Fresh = false;
    require(!m_Stopped, "Client connection stopped");
    require(next.sessionId == m_SessionId && isIdentity(next.connectionEpoch),
            "Client session changed");
    require(m_ConnectionEpoch.isEmpty() || m_ConnectionEpoch == next.connectionEpoch,
            "Connection changed; reconnect required");
    if (m_State) {
        const auto& old = *m_State;
        require(compareIdentity(next.accepted.revision, old.accepted.revision) >= 0 &&
                    compareIdentity(next.controlEpoch, old.controlEpoch) >= 0,
                "Stale transport state");
        if (old.confirmed && next.confirmed) {
            require(compareIdentity(next.confirmed->revision, old.confirmed->revision) >= 0,
                    "Stale encoder state");
        }
        for (const auto& receipt : next.receipts) {
            for (const auto& prior : old.receipts) {
                if (prior.policy.revision != receipt.policy.revision)
                    continue;
                require(prior.policy.immutableFields == receipt.policy.immutableFields,
                        "Policy revision was mutated");
                require(!prior.encoderApplied || receipt.encoderApplied,
                        "Encoder receipt regressed");
                require(!prior.firstSentFrame || prior.firstSentFrame == receipt.firstSentFrame,
                        "Send receipt regressed or changed");
            }
        }
    }
    m_ConnectionEpoch = next.connectionEpoch;
    m_State = std::move(next);
    m_Fresh = true;
}
void Mirror::acknowledge(Submission reply, const QString& expectedRequestId)
{
    m_Fresh = false;
    m_RequestRevision.reset();
    require(reply.requestId == expectedRequestId, "Request identity mismatch");
    require(isIdentity(reply.requestRevision) &&
                compareIdentity(reply.requestRevision, reply.status.accepted.revision) <= 0,
            "Invalid request revision");
    accept(std::move(reply.status));
    m_RequestRevision = std::move(reply.requestRevision);
}
void Mirror::invalidate()
{
    m_Fresh = false;
}
void Mirror::stop()
{
    m_Stopped = true;
    m_Fresh = false;
    m_RequestRevision.reset();
}
bool Mirror::canSubmit() const
{
    return !m_Stopped && m_Fresh && m_State && !m_State->stopped && m_State->liveControlAvailable;
}
} // namespace TransportPolicy
