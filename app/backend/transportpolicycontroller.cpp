#include "transportpolicycontroller.h"
#include "transportpolicymirror.h"

#include <QThread>
#include <QUuid>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace TransportPolicy {
bool Notification::valid() const
{
    if (!isIdentity(sessionId, QStringLiteral("4294967295")) || !isIdentity(connectionEpoch) ||
        !isIdentity(sequence) || !isIdentity(controlEpoch) || !isIdentity(acceptedRevision) ||
        (flags & ~quint16(0x7f)) ||
        (source != "legacy" && source != "manual" && source != "googcc" && source != "local") ||
        (failure != "none" && failure != "unsupported" && failure != "backend_failure" &&
         failure != "superseded" && failure != "stopped"))
        return false;
    if (bool(flags & 1) != appliedRevision.has_value() ||
        bool(flags & 2) != firstSentRevision.has_value() ||
        firstSentRevision.has_value() != firstSentFrame.has_value())
        return false;
    if (appliedRevision &&
        (!isIdentity(*appliedRevision) || compareIdentity(*appliedRevision, acceptedRevision) > 0))
        return false;
    if (firstSentRevision &&
        (!appliedRevision || !isIdentity(*firstSentRevision) ||
         !isIdentity(*firstSentFrame, QStringLiteral("18446744073709551615"), false) ||
         compareIdentity(*firstSentRevision, *appliedRevision) > 0))
        return false;
    if ((flags & 4) &&
        (failure != "none" || appliedRevision == std::optional<QString>(acceptedRevision)))
        return false;
    return !(flags & 16) || !(flags & (4 | 8 | 32 | 64));
}
bool Notification::advances(const Notification& p) const
{
    if (!valid() || sessionId != p.sessionId || connectionEpoch != p.connectionEpoch ||
        (p.flags & 16) || compareIdentity(sequence, p.sequence) <= 0 ||
        compareIdentity(acceptedRevision, p.acceptedRevision) < 0 ||
        compareIdentity(controlEpoch, p.controlEpoch) < 0 ||
        (controlEpoch == p.controlEpoch && source != p.source))
        return false;
    if (p.appliedRevision &&
        (!appliedRevision || compareIdentity(*appliedRevision, *p.appliedRevision) < 0))
        return false;
    if (p.firstSentRevision &&
        (!firstSentRevision || compareIdentity(*firstSentRevision, *p.firstSentRevision) < 0 ||
         (*firstSentRevision == *p.firstSentRevision && firstSentFrame != p.firstSentFrame)))
        return false;
    // Readiness can fall during a rebuild; historical receipts cannot.
    return true;
}
bool Notification::satisfiedBy(const Status& s) const
{
    if (sessionId != s.sessionId || connectionEpoch != s.connectionEpoch ||
        compareIdentity(s.accepted.revision, acceptedRevision) < 0 ||
        compareIdentity(s.controlEpoch, controlEpoch) < 0 ||
        (s.controlEpoch == controlEpoch && s.accepted.source != source) ||
        (appliedRevision &&
         (!s.confirmed || compareIdentity(s.confirmed->revision, *appliedRevision) < 0)) ||
        ((flags & 16) && !s.stopped))
        return false;
    if (firstSentRevision) {
        bool found = false;
        for (const auto& r : s.receipts) {
            if (r.policy.revision == *firstSentRevision) {
                if (!r.encoderApplied || r.firstSentFrame != firstSentFrame)
                    return false;
                found = true;
            }
        }
        // Historical receipts can leave the bounded HTTP history; current
        // accepted progress must still be evidenced by its own receipt.
        if (!found && s.accepted.revision == *firstSentRevision)
            return false;
    }
    if (s.accepted.revision == acceptedRevision && failure != "none" && !s.stopped) {
        for (const auto& r : s.receipts) {
            if (r.policy.revision == acceptedRevision)
                return r.failure == failure;
        }
        return false;
    }
    return true;
}
struct Controller::Shared
{
    mutable std::mutex mutex;
    std::condition_variable wake;
    bool stopped = false;
    std::optional<Command> command;
    QString sessionId;
    QString expectedEpoch;
    std::optional<Notification> notification;
    bool queryRequested = false;
    View view;
};

Controller::Controller(QString sessionId, Factory factory, int pollIntervalMs, bool readOnly,
                       QString expectedEpoch)
    : m_Shared(std::make_shared<Shared>())
{
    if (!isIdentity(sessionId, QStringLiteral("4294967295")) || pollIntervalMs < 1 || !factory ||
        (!expectedEpoch.isEmpty() && !isIdentity(expectedEpoch))) {
        throw std::invalid_argument("Invalid transport controller configuration");
    }
    const auto shared = m_Shared;
    shared->sessionId = sessionId;
    shared->expectedEpoch = expectedEpoch;
    shared->view.readOnly = readOnly;
    m_Thread.reset(QThread::create([shared, sessionId = std::move(sessionId),
                                    factory = std::move(factory), pollIntervalMs, readOnly,
                                    expectedEpoch] {
        Mirror mirror(sessionId);
        std::unique_ptr<Transport> transport;
        View current;
        current.readOnly = readOnly;
        const auto stopped = [&] {
            std::lock_guard<std::mutex> lock(shared->mutex);
            return shared->stopped;
        };
        const auto publish = [&] {
            std::lock_guard<std::mutex> lock(shared->mutex);
            if (shared->stopped)
                return;
            current.status = mirror.state();
            current.requestRevision = mirror.requestRevision();
            if (shared->notification &&
                (!current.status || !shared->notification->satisfiedBy(*current.status))) {
                mirror.invalidate();
                current.queryError = QStringLiteral("State reconciliation pending");
            }
            current.fresh = mirror.canSubmit();
            // A UI command can arrive during a poll. Preserve its busy state
            // until that exact command has been consumed, rather than accepting another.
            shared->view = current;
            shared->view.submitting = current.submitting || shared->command.has_value();
        };
        while (!stopped()) {
            std::optional<Command> command;
            {
                std::lock_guard<std::mutex> lock(shared->mutex);
                command = shared->command;
                shared->command.reset();
                shared->queryRequested = false;
            }
            current.submitting = command.has_value();
            if (command) {
                current.requestError.clear();
                mirror.clearRequestOutcome();
            }
            const auto queryBegin = std::chrono::steady_clock::now();
            try {
                if (!transport)
                    transport = factory();
                if (!transport)
                    throw std::runtime_error("No paired transport available");
                auto reply = transport->query(sessionId, mirror.connectionEpoch().isEmpty()
                                                             ? expectedEpoch
                                                             : mirror.connectionEpoch());
                if (!expectedEpoch.isEmpty() && reply.connectionEpoch != expectedEpoch)
                    throw std::invalid_argument("Connection changed");
                {
                    std::lock_guard<std::mutex> lock(shared->mutex);
                    if (shared->notification && !shared->notification->satisfiedBy(reply))
                        throw std::invalid_argument("Query is behind authenticated notification");
                }
                current.networkDeadline.reset();
                if (reply.networkStatistics && reply.networkStatistics->freshnessRemainingUs)
                    current.networkDeadline =
                        queryBegin +
                        std::chrono::microseconds(*reply.networkStatistics->freshnessRemainingUs);
                mirror.accept(std::move(reply));
                current.queryError.clear();
                publish();
                if (command && !stopped()) {
                    if (readOnly || !mirror.canSubmit())
                        throw std::invalid_argument("Current policy is not actionable");
                    {
                        std::lock_guard<std::mutex> lock(shared->mutex);
                        if (shared->notification &&
                            !shared->notification->satisfiedBy(*mirror.state()))
                            throw std::invalid_argument("Reconcile newer state before submitting");
                    }
                    const auto id =
                        QStringLiteral("pc-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
                    auto body = command->manual
                                    ? manualRequest(*mirror.state(), id, command->totalKbps)
                                    : controlRequest(*mirror.state(), id, command->bitrate,
                                                     command->fec, command->totalKbps);
                    auto reply = transport->submit(command->manual
                                                       ? QStringLiteral("api/v2/transport-policy")
                                                       : QStringLiteral("api/v2/transport-control"),
                                                   body);
                    if (!stopped())
                        mirror.acknowledge(std::move(reply), id);
                }
            } catch (const std::exception& e) {
                mirror.invalidate();
                if (command)
                    current.requestError = QString::fromUtf8(e.what());
                else
                    current.queryError = QString::fromUtf8(e.what());
            } catch (...) {
                mirror.invalidate();
                if (command)
                    current.requestError = QStringLiteral("Request outcome unconfirmed");
                else
                    current.queryError = QStringLiteral("Query failed");
            }
            current.submitting = false;
            publish();
            std::unique_lock<std::mutex> lock(shared->mutex);
            const auto periodic =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(pollIntervalMs);
            while (!shared->stopped && !shared->command) {
                const auto due =
                    shared->queryRequested
                        ? std::min(periodic, queryBegin + std::chrono::milliseconds(250))
                        : periodic;
                if (std::chrono::steady_clock::now() >= due)
                    break;
                shared->wake.wait_until(lock, due);
            }
        }
        mirror.stop();
        // No Session/UI callback is ever retained or published on shutdown.
    }));
    m_Thread->setObjectName(QStringLiteral("Transport Policy"));
    m_Thread->start();
}
Controller::~Controller()
{
    stop();
}
View Controller::view() const
{
    std::lock_guard<std::mutex> lock(m_Shared->mutex);
    return m_Shared->view;
}
bool Controller::submit(Command command)
{
    if (command.totalKbps < 1 || command.totalKbps > 800000)
        return false;
    {
        std::lock_guard<std::mutex> lock(m_Shared->mutex);
        if (m_Shared->stopped || m_Shared->command || !m_Shared->view.canSubmit())
            return false;
        m_Shared->command = command;
        m_Shared->view.submitting = true;
    }
    m_Shared->wake.notify_one();
    return true;
}
bool Controller::setModes(bool bitrate, bool fec, int maximumKbps)
{
    return submit({ false, bitrate, fec, maximumKbps });
}
bool Controller::setManualBudget(int totalKbps)
{
    return submit({ true, false, false, totalKbps });
}
bool Controller::receiveNotification(const Notification& notification)
{
    {
        std::lock_guard<std::mutex> lock(m_Shared->mutex);
        const auto epoch = !m_Shared->expectedEpoch.isEmpty() ? m_Shared->expectedEpoch
                           : m_Shared->view.status ? m_Shared->view.status->connectionEpoch
                                                   : QString();
        if (m_Shared->stopped || !notification.valid() ||
            notification.sessionId != m_Shared->sessionId || epoch.isEmpty() ||
            notification.connectionEpoch != epoch ||
            (m_Shared->notification && !notification.advances(*m_Shared->notification)))
            return false;
        m_Shared->notification = notification;
        m_Shared->queryRequested = true;
        if (!m_Shared->view.status || !notification.satisfiedBy(*m_Shared->view.status)) {
            m_Shared->view.fresh = false;
            m_Shared->view.queryError = QStringLiteral("State reconciliation pending");
        }
    }
    m_Shared->wake.notify_one();
    return true;
}
void Controller::stop()
{
    {
        std::lock_guard<std::mutex> lock(m_Shared->mutex);
        m_Shared->stopped = true;
        m_Shared->command.reset();
        m_Shared->notification.reset();
        m_Shared->queryRequested = false;
        m_Shared->view.fresh = false;
        m_Shared->view.submitting = false;
        m_Shared->view.networkDeadline.reset();
        m_Shared->view.status.reset();
    }
    m_Shared->wake.notify_one();
    if (m_Thread)
        m_Thread->wait();
}
} // namespace TransportPolicy
