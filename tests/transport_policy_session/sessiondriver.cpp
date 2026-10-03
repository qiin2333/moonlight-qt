#include "sessiondriver.h"
#include "streaming/video/overlaymenupanel.h"
#include "backend/legacytransportscope.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QScopedValueRollback>
#include <QScreen>
#include <QPixmap>
#include <QImage>
#include <QStringList>
#include <QMutex>
#include <QMutexLocker>
#include <stdexcept>

void recordTransportNotificationEvent(const QString& directory, QJsonObject event)
{
    if (directory.isEmpty())
        return;
    static QMutex mutex;
    QMutexLocker locker(&mutex);
    QFile file(QDir(directory).filePath(QStringLiteral("notification-events.jsonl")));
    if (file.size() >= 524288 || !file.open(QIODevice::WriteOnly | QIODevice::Append))
        return;
    const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    event.insert(QStringLiteral("steadyUs"), QString::number(now));
    const auto bytes = QJsonDocument(event).toJson(QJsonDocument::Compact) + '\n';
    if (bytes.size() <= 524288 - file.size())
        file.write(bytes);
}

TransportPolicySessionDriver::TransportPolicySessionDriver(OverlayMenuPanel& panel, QString output,
                                                           std::function<void()> showMenu)
    : m_Panel(panel), m_Output(std::move(output)), m_ShowMenu(std::move(showMenu))
{
    const QDir portable(QCoreApplication::applicationDirPath());
    // Refuse to write into an ordinary installation or accept an arbitrary path.
    if (!QFile::exists(portable.filePath(QStringLiteral("portable.dat"))) ||
        QDir(m_Output).canonicalPath() != portable.canonicalPath()) {
        throw std::invalid_argument("Session integration requires its own portable directory");
    }
    QFile file(portable.filePath(QStringLiteral("transport-session-script.json")));
    if (!file.open(QIODevice::ReadOnly))
        throw std::invalid_argument("Missing session script");
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        throw std::invalid_argument("Invalid session script");
    const auto mode =
        document.object().value(QStringLiteral("mode")).toString(QStringLiteral("policy"));
    if (mode != QStringLiteral("policy") && mode != QStringLiteral("legacy") &&
        mode != QStringLiteral("statistics") && mode != QStringLiteral("notifications"))
        throw std::invalid_argument("Invalid session mode");
    m_LegacyMode = mode == QStringLiteral("legacy");
    m_StatisticsMode = mode == QStringLiteral("statistics");
    m_NotificationsMode = mode == QStringLiteral("notifications");
    m_Steps = document.object().value(QStringLiteral("steps")).toArray();
    if (m_Steps.isEmpty() || m_Steps.size() > 20)
        throw std::invalid_argument("Invalid session steps");
    int stepIndex = 0;
    for (const auto value : m_Steps) {
        const auto step = value.toObject();
        const auto action = step.value(QStringLiteral("action")).toString();
        const int budget = step.value(QStringLiteral("budgetKbps")).toInt();
        if (m_NotificationsMode) {
            if (action != QStringLiteral("observe") || budget < 500 || budget > 800000)
                throw std::invalid_argument(
                    "Notifications session requires passive budget observations");
            continue;
        }
        if (m_StatisticsMode) {
            const QStringList expected{ QStringLiteral("valid"), QStringLiteral("expired"),
                                        QStringLiteral("delayed_reply"),
                                        QStringLiteral("recovered") };
            if (m_Steps.size() != expected.size() || action != expected[stepIndex++] ||
                budget < 500 || budget > 800000)
                throw std::invalid_argument(
                    "Statistics session requires ordered observations and a fixed budget");
            continue;
        }
        if ((action != QStringLiteral("bitrate") && action != QStringLiteral("fec") &&
             action != QStringLiteral("manual") && action != QStringLiteral("budget") &&
             action != QStringLiteral("reconnect") && action != QStringLiteral("fault")) ||
            budget < 500 || budget > 800000 ||
            !step.value(QStringLiteral("expectBitrate")).isBool() ||
            !step.value(QStringLiteral("expectFec")).isBool())
            throw std::invalid_argument("Invalid session action");
        if (m_LegacyMode &&
            ((action != QStringLiteral("budget") && action != QStringLiteral("reconnect") &&
              action != QStringLiteral("fault")) ||
             step.value(QStringLiteral("expectBitrate")).toBool() ||
             step.value(QStringLiteral("expectFec")).toBool()))
            throw std::invalid_argument(
                "Legacy session must use manual bitrate or reconnect actions");
        if (action == QStringLiteral("fault") &&
            (!m_LegacyMode || step.value(QStringLiteral("requestedBudgetKbps")).toInt() < 500 ||
             step.value(QStringLiteral("requestedBudgetKbps")).toInt() > 800000))
            throw std::invalid_argument(
                "Fault action requires legacy mode and a valid requested target");
    }
    m_Clock.start();
    save();
}

void TransportPolicySessionDriver::tickNotifications(const TransportPolicy::View& view,
                                                     const LegacySessionObservation& observation)
{
    if (m_Processing || m_Finished)
        return;
    QScopedValueRollback<bool> guard(m_Processing, true);
    if (!m_NotificationsMode) {
        finish(false, QStringLiteral("Notifications driver mode required"));
        return;
    }
    if (m_Clock.elapsed() > 45000) {
        finish(false, QStringLiteral("Notifications session timeout"));
        return;
    }
    if (!observation.receivedVideo || !view.status || !view.queryError.isEmpty())
        return;
    if (!view.readOnly || view.canSubmit() || observation.requestInFlight ||
        view.status->connectionEpoch != observation.connectionEpoch ||
        observation.configuredBitrateKbps != 10000) {
        finish(false,
               QStringLiteral("Notification observation changed read-only authority or identity"));
        return;
    }
    const auto budget = m_Steps[m_Index].toObject().value(QStringLiteral("budgetKbps")).toInt();
    if (view.status->accepted.totalKbps != budget)
        return;
    bool sent = false;
    for (const auto& receipt : view.status->receipts) {
        if (receipt.policy.revision == view.status->accepted.revision && receipt.encoderApplied &&
            receipt.firstSentFrame && receipt.failure == QStringLiteral("none"))
            sent = true;
    }
    if (!sent || (m_Index == 0 && m_Clock.elapsed() < 1800))
        return;
    m_ShowMenu();
    m_Panel.navigateToLevel(2);
    recordStep(view);
}

QJsonObject TransportPolicySessionDriver::snapshot(const TransportPolicy::View& view) const
{
    QJsonArray rows;
    for (const auto& row : m_Panel.m_MenuLevels[2].items) {
        rows.append(QJsonObject{ { QStringLiteral("label"), row.label },
                                 { QStringLiteral("detail"), row.detail },
                                 { QStringLiteral("enabled"), row.enabled },
                                 { QStringLiteral("toggleState"), row.toggleState } });
    }
    QJsonObject result{ { QStringLiteral("menuRows"), rows },
                        { QStringLiteral("fresh"), view.fresh },
                        { QStringLiteral("submitting"), view.submitting },
                        { QStringLiteral("requestRevision"),
                          view.requestRevision.value_or(QString()) },
                        { QStringLiteral("currentLabel"), m_Panel.transportCurrentLabel() },
                        { QStringLiteral("operationLabel"), m_Panel.transportOperationLabel() } };
    result.insert(QStringLiteral("readOnly"), view.readOnly);
    result.insert(QStringLiteral("canSubmit"), view.canSubmit());
    result.insert(QStringLiteral("queryError"), view.queryError);
    if (const auto statistics = view.networkStatistics()) {
        QJsonObject display{ { QStringLiteral("reason"), statistics->reason },
                             { QStringLiteral("fresh"), statistics->fresh },
                             { QStringLiteral("rawLossPercent"),
                               statistics->rawLossPercent ? QJsonValue(*statistics->rawLossPercent)
                                                          : QJsonValue() },
                             { QStringLiteral("lossLabel"), m_Panel.rawNetworkLossLabel() },
                             { QStringLiteral("windowLabel"), m_Panel.networkSampleLabel() } };
        if (view.status && view.status->networkStatistics) {
            const auto& source = *view.status->networkStatistics;
            display.insert(QStringLiteral("sourceReason"), source.reason);
            display.insert(QStringLiteral("sourceFresh"), source.fresh);
            display.insert(QStringLiteral("sampleTimeUs"), source.sampleTimeUs.value_or(QString()));
            display.insert(QStringLiteral("remainingUs"), source.freshnessRemainingUs.value_or(-1));
        }
        result.insert(QStringLiteral("statistics"), display);
    }
    if (view.status) {
        result.insert(QStringLiteral("sessionId"), view.status->sessionId);
        result.insert(QStringLiteral("connectionEpoch"), view.status->connectionEpoch);
        result.insert(QStringLiteral("controlEpoch"), view.status->controlEpoch);
        result.insert(QStringLiteral("accepted"), view.status->accepted.immutableFields);
        QJsonArray receipts;
        for (const auto& receipt : view.status->receipts) {
            receipts.append(QJsonObject{
                { QStringLiteral("policy"), receipt.policy.immutableFields },
                { QStringLiteral("encoderApplied"), receipt.encoderApplied },
                { QStringLiteral("firstSentFrame"), receipt.firstSentFrame.value_or(QString()) },
                { QStringLiteral("failure"), receipt.failure } });
        }
        result.insert(QStringLiteral("receipts"), receipts);
    }
    return result;
}

void TransportPolicySessionDriver::tickStatistics(const TransportPolicy::View& view,
                                                  const LegacySessionObservation& observation)
{
    if (m_Processing || m_Finished)
        return;
    QScopedValueRollback<bool> guard(m_Processing, true);
    if (!m_StatisticsMode) {
        finish(false, QStringLiteral("Statistics driver mode required"));
        return;
    }
    if (m_Clock.elapsed() > 90000) {
        finish(false, QStringLiteral("Statistics session timeout"));
        return;
    }
    if (!observation.receivedVideo || !view.status)
        return;
    if (!view.readOnly || view.canSubmit() ||
        view.status->connectionEpoch != observation.connectionEpoch ||
        observation.configuredBitrateKbps !=
            m_Steps[m_Index].toObject().value(QStringLiteral("budgetKbps")).toInt() ||
        observation.requestInFlight) {
        finish(false,
               QStringLiteral("Statistics view changed authority, identity or configured budget"));
        return;
    }
    if (!view.queryError.isEmpty()) {
        finish(false, view.queryError);
        return;
    }
    const auto statistics = view.networkStatistics();
    if (!statistics || !view.status->networkStatistics || !view.networkDeadline)
        return;
    const auto& source = *view.status->networkStatistics;
    if (source.reason != QStringLiteral("valid") || !source.fresh)
        return;
    const bool valid =
        statistics->reason == QStringLiteral("valid") && statistics->rawLossPercent.has_value();
    const bool expired =
        statistics->reason == QStringLiteral("feedback_stale") && !statistics->rawLossPercent;
    if (m_Index == 0 && (!valid || m_Clock.elapsed() < 1800))
        return;
    if (m_Index == 1 &&
        (!expired || view.networkDeadline != m_StatisticsDeadline ||
         !QFile::exists(QDir(m_Output).filePath(QStringLiteral("statistics-proxy-held.json")))))
        return;
    if (m_Index == 2 && (!expired || view.networkDeadline == m_StatisticsDeadline))
        return;
    if (m_Index == 3 && (!valid || m_Clock.elapsed() < 12000))
        return;
    m_ShowMenu();
    m_Panel.navigateToLevel(2);
    if (m_Index == 0)
        m_StatisticsDeadline = view.networkDeadline;
    const int observed = m_Index;
    recordStep(view);
    if (observed == 0 || observed == 1) {
        QSaveFile signal(QDir(m_Output).filePath(observed == 0
                                                     ? QStringLiteral("statistics-hold.json")
                                                     : QStringLiteral("statistics-release.json")));
        if (!signal.open(QIODevice::WriteOnly)) {
            finish(false, QStringLiteral("Cannot record statistics fault signal"));
            return;
        }
        signal.write(
            QJsonDocument(
                QJsonObject{ { QStringLiteral("connectionEpoch"), observation.connectionEpoch },
                             { QStringLiteral("elapsedMs"), QString::number(m_Clock.elapsed()) } })
                .toJson());
        if (!signal.commit())
            finish(false, QStringLiteral("Cannot commit statistics fault signal"));
    }
}

void TransportPolicySessionDriver::tick(const TransportPolicy::View& view)
{
    if (m_LegacyMode) {
        finish(false, QStringLiteral("Legacy driver cannot consume a policy view"));
        return;
    }
    if (m_Processing || m_Finished)
        return;
    QScopedValueRollback<bool> guard(m_Processing, true);
    if (m_Clock.elapsed() > 180000) {
        finish(false, QStringLiteral("Session action timeout"));
        return;
    }
    if (m_Waiting && !view.requestError.isEmpty()) {
        finish(false, view.requestError);
        return;
    }
    if (!view.canSubmit())
        return;
    if (m_WaitingReconnect) {
        if (view.status->connectionEpoch == m_PreviousEpoch)
            return;
        const auto& status = *view.status;
        const auto automatic = status.accepted.automatic;
        if (bool(automatic && automatic->bitrate) != m_ExpectedBitrate ||
            bool(automatic && automatic->fec) != m_ExpectedFec ||
            status.accepted.totalKbps != m_ExpectedBudget)
            return;
        bool sent = false;
        for (const auto& receipt : status.receipts) {
            if (receipt.policy.revision == status.accepted.revision && receipt.encoderApplied &&
                receipt.firstSentFrame && receipt.failure == QStringLiteral("none"))
                sent = true;
        }
        if (!sent)
            return;
        if (recordStep(view, true))
            return;
    }
    if (m_Waiting) {
        if (!view.requestRevision || *view.requestRevision == m_PreviousRequest)
            return;
        const auto& status = *view.status;
        const auto automatic = status.accepted.automatic;
        if (bool(automatic && automatic->bitrate) != m_ExpectedBitrate ||
            bool(automatic && automatic->fec) != m_ExpectedFec ||
            (automatic ? automatic->maximumKbps : status.accepted.totalKbps) != m_ExpectedBudget)
            return;
        // Automatic candidates can supersede the request before first send.
        // Require an actual applied/sent receipt in its control epoch with the
        // exact requested mode/cap; preserve the original request's own phase.
        bool sentIntent = false;
        for (const auto& receipt : status.receipts) {
            const auto a = receipt.policy.automatic;
            if (receipt.policy.controlEpoch == status.controlEpoch && receipt.encoderApplied &&
                receipt.firstSentFrame && receipt.failure == QStringLiteral("none") &&
                bool(a && a->bitrate) == m_ExpectedBitrate && bool(a && a->fec) == m_ExpectedFec &&
                (a ? a->maximumKbps : receipt.policy.totalKbps) == m_ExpectedBudget) {
                sentIntent = true;
            }
        }
        if (!sentIntent || m_Clock.elapsed() - m_ActionAt < 600)
            return;
        if (recordStep(view))
            return;
    }
    if (m_Clock.elapsed() < 1000)
        return;
    const auto step = m_Steps[m_Index].toObject();
    m_ExpectedBitrate = step.value(QStringLiteral("expectBitrate")).toBool();
    m_ExpectedFec = step.value(QStringLiteral("expectFec")).toBool();
    m_ExpectedBudget = step.value(QStringLiteral("budgetKbps")).toInt();
    m_PreviousRequest = view.requestRevision.value_or(QString());
    m_ActionAt = m_Clock.elapsed();
    m_Waiting = true;
    m_ShowMenu();
    m_Panel.navigateToLevel(2);
    const auto action = step.value(QStringLiteral("action")).toString();
    if (action == QStringLiteral("reconnect")) {
        m_Waiting = false;
        m_WaitingReconnect = true;
        m_PreviousEpoch = view.status->connectionEpoch;
        requestOwnedHostRestart(m_PreviousEpoch, view.status->accepted.totalKbps);
    } else if (action == QStringLiteral("budget")) {
        m_Panel.setBitrateKbps(m_ExpectedBudget);
        m_Panel.commitBitrateNow();
    } else {
        m_Panel.updateBitrateState(m_ExpectedBudget);
        const auto target = action == QStringLiteral("bitrate")
                                ? OverlayMenuPanel::MenuAction::ToggleAutomaticBitrate
                            : action == QStringLiteral("fec")
                                ? OverlayMenuPanel::MenuAction::ToggleAutomaticFec
                                : OverlayMenuPanel::MenuAction::TakeManualTransportControl;
        const auto& items = m_Panel.m_MenuLevels[2].items;
        for (int i = 0; i < static_cast<int>(items.size()); ++i) {
            if (items[i].action != target)
                continue;
            m_Panel.m_HoveredIndex = i;
            m_Panel.gamepadSelect();
            return;
        }
        finish(false, QStringLiteral("Production menu action missing"));
    }
}

void TransportPolicySessionDriver::requestOwnedHostRestart(const QString& epoch, int budget)
{
    QSaveFile signal(QDir(m_Output).filePath(QStringLiteral("transport-session-fault.json")));
    if (!signal.open(QIODevice::WriteOnly)) {
        finish(false, QStringLiteral("Cannot request private host fault"));
        return;
    }
    signal.write(
        QJsonDocument(QJsonObject{ { QStringLiteral("action"), QStringLiteral("restartOwnedHost") },
                                   { QStringLiteral("step"), m_Index },
                                   { QStringLiteral("oldConnectionEpoch"), epoch },
                                   { QStringLiteral("oldBudgetKbps"), budget } })
            .toJson());
    if (!signal.commit())
        finish(false, QStringLiteral("Cannot commit private host fault"));
}

void TransportPolicySessionDriver::tickLegacy(const LegacySessionObservation& observation)
{
    if (m_Processing || m_Finished)
        return;
    QScopedValueRollback<bool> guard(m_Processing, true);
    if (!m_LegacyMode) {
        finish(false, QStringLiteral("Policy driver cannot consume legacy observations"));
        return;
    }
    if (m_Clock.elapsed() > 180000) {
        finish(false, QStringLiteral("Legacy session action timeout"));
        return;
    }
    if (!LegacyTransportScope::valid(observation.sessionId, observation.connectionEpoch) ||
        !observation.receivedVideo)
        return;
    if (m_WaitingReconnect) {
        if (m_Steps[m_Index].toObject().value(QStringLiteral("action")).toString() ==
                QStringLiteral("fault") &&
            observation.connectionEpoch == m_PreviousEpoch && observation.requestInFlight &&
            m_FaultInFlightObservation.isEmpty()) {
            m_FaultInFlightObservation = QJsonObject{
                { QStringLiteral("connectionEpoch"), observation.connectionEpoch },
                { QStringLiteral("configuredBitrateKbps"), observation.configuredBitrateKbps },
                { QStringLiteral("acknowledgedBitrateKbps"), observation.acknowledgedBitrateKbps },
                { QStringLiteral("requestInFlight"), observation.requestInFlight },
                { QStringLiteral("receivedVideo"), observation.receivedVideo },
                { QStringLiteral("elapsedMs"), QString::number(m_Clock.elapsed()) }
            };
            save();
        }
        if (observation.connectionEpoch == m_PreviousEpoch ||
            observation.configuredBitrateKbps != m_ExpectedBudget || observation.requestInFlight)
            return;
        if (recordLegacyStep(observation, true))
            return;
    }
    if (m_Waiting) {
        if (observation.acknowledgedBitrateKbps != m_ExpectedBudget ||
            observation.configuredBitrateKbps != m_ExpectedBudget || observation.requestInFlight ||
            m_Clock.elapsed() - m_ActionAt < 1800)
            return;
        if (recordLegacyStep(observation))
            return;
    }
    if (m_Clock.elapsed() < 1000)
        return;
    const auto step = m_Steps[m_Index].toObject();
    m_ExpectedBudget = step.value(QStringLiteral("budgetKbps")).toInt();
    m_ActionAt = m_Clock.elapsed();
    m_ShowMenu();
    m_Panel.navigateToLevel(2);
    if (step.value(QStringLiteral("action")).toString() == QStringLiteral("reconnect")) {
        m_WaitingReconnect = true;
        m_PreviousEpoch = observation.connectionEpoch;
        requestOwnedHostRestart(m_PreviousEpoch, observation.configuredBitrateKbps);
    } else {
        const bool fault =
            step.value(QStringLiteral("action")).toString() == QStringLiteral("fault");
        const int requested =
            fault ? step.value(QStringLiteral("requestedBudgetKbps")).toInt() : m_ExpectedBudget;
        if (observation.acknowledgedBitrateKbps == requested) {
            finish(false, QStringLiteral("Legacy test requires a distinct acknowledged target"));
            return;
        }
        m_Waiting = !fault;
        m_WaitingReconnect = fault;
        if (fault)
            m_PreviousEpoch = observation.connectionEpoch;
        m_Panel.setBitrateKbps(requested);
        if (m_Panel.m_BitrateKbps != requested) {
            finish(false,
                   QStringLiteral(
                       "Legacy target is not representable by the production bitrate slider"));
            return;
        }
        m_Panel.commitBitrateNow();
    }
}

bool TransportPolicySessionDriver::takeOwnerReconnectRequest()
{
    if (!m_LegacyMode || m_Finished || !m_WaitingReconnect || m_OwnerReconnectRequested ||
        m_FaultInFlightObservation.isEmpty() ||
        m_Steps[m_Index].toObject().value(QStringLiteral("action")).toString() !=
            QStringLiteral("fault"))
        return false;
    QFile file(QDir(m_Output).filePath(QStringLiteral("transport-session-owner-fault.json")));
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const auto request = QJsonDocument::fromJson(file.readAll()).object();
    if (request.value(QStringLiteral("action")).toString() != QStringLiteral("owner_reconnect") ||
        request.value(QStringLiteral("connectionEpoch")).toString() != m_PreviousEpoch)
        return false;
    m_OwnerReconnectRequested = true;
    return true;
}

bool TransportPolicySessionDriver::recordLegacyStep(const LegacySessionObservation& observation,
                                                    bool reconnected)
{
    QJsonObject result{
        { QStringLiteral("step"), m_Index },
        { QStringLiteral("action"), m_Steps[m_Index].toObject() },
        { QStringLiteral("mode"), QStringLiteral("legacy") },
        { QStringLiteral("sessionId"), observation.sessionId },
        { QStringLiteral("connectionEpoch"), observation.connectionEpoch },
        { QStringLiteral("configuredBitrateKbps"), observation.configuredBitrateKbps },
        { QStringLiteral("acknowledgedBitrateKbps"), observation.acknowledgedBitrateKbps },
        { QStringLiteral("requestInFlight"), observation.requestInFlight },
        { QStringLiteral("receivedVideo"), observation.receivedVideo },
        { QStringLiteral("elapsedMs"), QString::number(m_Clock.elapsed()) },
        { QStringLiteral("proofScope"),
          QStringLiteral(
              "client observation; host SDK/first-send require independent paired validation") }
    };
    if (reconnected)
        result.insert(QStringLiteral("previousConnectionEpoch"), m_PreviousEpoch);
    if (!m_FaultInFlightObservation.isEmpty())
        result.insert(QStringLiteral("faultInFlightObservation"), m_FaultInFlightObservation);
    QJsonArray rows;
    for (const auto& row : m_Panel.m_MenuLevels[2].items)
        rows.append(QJsonObject{ { QStringLiteral("label"), row.label },
                                 { QStringLiteral("detail"), row.detail },
                                 { QStringLiteral("enabled"), row.enabled },
                                 { QStringLiteral("toggleState"), row.toggleState } });
    result.insert(QStringLiteral("menuRows"), rows);
    const QString screenshot =
        QStringLiteral("legacy-menu-%1.png").arg(m_Index, 2, 10, QLatin1Char('0'));
    QImage image(m_Panel.size() * m_Panel.devicePixelRatio(), QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(m_Panel.devicePixelRatio());
    QPainter painter(&image);
    m_Panel.paintContents(painter);
    painter.end();
    result.insert(QStringLiteral("screenshotSaved"),
                  image.save(QDir(m_Output).filePath(screenshot)));
    result.insert(QStringLiteral("screenshotMethod"),
                  QStringLiteral("production paintContents into QImage"));
    result.insert(QStringLiteral("visualAcceptance"),
                  QStringLiteral("requires review; desktop composition unverified"));
    m_Results.append(result);
    ++m_Index;
    m_Waiting = m_WaitingReconnect = false;
    save();
    if (m_Index == m_Steps.size()) {
        finish(true);
        return true;
    }
    return false;
}

bool TransportPolicySessionDriver::recordStep(const TransportPolicy::View& view, bool reconnected)
{
    auto result = snapshot(view);
    result.insert(QStringLiteral("step"), m_Index);
    result.insert(QStringLiteral("action"), m_Steps[m_Index].toObject());
    result.insert(QStringLiteral("elapsedMs"), QString::number(m_Clock.elapsed()));
    if (reconnected)
        result.insert(QStringLiteral("previousConnectionEpoch"), m_PreviousEpoch);
    const QString screenshot = QStringLiteral("menu-%1.png").arg(m_Index, 2, 10, QLatin1Char('0'));
    // Use the exact production paint path, with its current layout and state.
    // This proves a rendered menu layout, not desktop/window composition.
    QImage image(m_Panel.size() * m_Panel.devicePixelRatio(), QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(m_Panel.devicePixelRatio());
    QPainter painter(&image);
    m_Panel.paintContents(painter);
    painter.end();
    result.insert(QStringLiteral("screenshotSaved"),
                  image.save(QDir(m_Output).filePath(screenshot)));
    result.insert(QStringLiteral("screenshotMethod"),
                  QStringLiteral("production paintContents into QImage"));
    result.insert(QStringLiteral("visualAcceptance"),
                  QStringLiteral("requires review; desktop composition unverified"));
    m_Results.append(result);
    ++m_Index;
    m_Waiting = m_WaitingReconnect = false;
    save();
    if (m_Index == m_Steps.size()) {
        finish(true);
        return true;
    }
    return false;
}

void TransportPolicySessionDriver::save()
{
    QSaveFile file(QDir(m_Output).filePath(QStringLiteral("transport-session-progress.json")));
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(QJsonObject{ { QStringLiteral("results"), m_Results },
                                              { QStringLiteral("faultInFlightObservation"),
                                                m_FaultInFlightObservation },
                                              { QStringLiteral("finished"), m_Finished } })
                       .toJson());
        file.commit();
    }
}
void TransportPolicySessionDriver::finish(bool passed, QString error)
{
    m_Finished = true;
    save();
    QSaveFile file(QDir(m_Output).filePath(QStringLiteral("transport-session-result.json")));
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(QJsonObject{ { QStringLiteral("passed"), passed },
                                              { QStringLiteral("error"), error },
                                              { QStringLiteral("completedSteps"), m_Index },
                                              { QStringLiteral("results"), m_Results } })
                       .toJson());
        file.commit();
    }
}
bool TransportPolicySessionDriver::takeExitRequest()
{
    if (!m_Finished || m_ExitRequested)
        return false;
    m_ExitRequested = true;
    return true;
}
