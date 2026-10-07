#include "streaming/video/overlaymenupanel.h"
#include <QFile>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QDir>
#include <QImage>
#include <QtTest>

class TransportPolicyMenuTest : public QObject
{
    Q_OBJECT
private:
    TransportPolicy::View sample()
    {
        QFile file(QStringLiteral(TEST_DATA_DIR "/policy-final.json"));
        if (!file.open(QIODevice::ReadOnly))
            qFatal("Cannot open captured host state");
        TransportPolicy::View view;
        view.status =
            TransportPolicy::parseStatus(QJsonDocument::fromJson(file.readAll()).object());
        view.fresh = true;
        return view;
    }
    void openPolicy(OverlayMenuPanel& panel)
    {
        panel.showAtCursor(0, 0, 1600, 1200, QPoint(400, 300), false);
        panel.navigateToLevel(2);
        QTest::qWait(20);
    }
    void click(OverlayMenuPanel& panel, OverlayMenuPanel::MenuAction action)
    {
        const auto& items = panel.m_MenuLevels[2].items;
        for (int i = 0; i < static_cast<int>(items.size()); i++) {
            if (items[i].action != action)
                continue;
            QTest::mouseClick(&panel, Qt::LeftButton, Qt::NoModifier,
                              QPoint(100, panel.m_ShadowMargin + panel.m_TitleHeight +
                                              panel.m_Padding + i * panel.m_ItemHeight + 19));
            return;
        }
        qFatal("Missing production control");
    }
private slots:
    void renderStatisticsAvailabilityWithProductionPainting()
    {
        const auto output = qEnvironmentVariable("MOONLIGHT_TRANSPORT_STATS_SCREENSHOTS");
        for (const bool control : { false, true }) {
            for (const QString& reason :
                 { QStringLiteral("valid"), QStringLiteral("feedback_stale"),
                   QStringLiteral("history_truncated"), QStringLiteral("coverage_incomplete") }) {
                OverlayMenuPanel panel;
                auto view = sample();
                TransportPolicy::NetworkStatistics statistics;
                statistics.connectionEpoch = view.status->connectionEpoch;
                statistics.reason =
                    reason == QStringLiteral("feedback_stale") ? QStringLiteral("valid") : reason;
                statistics.fresh = true;
                statistics.receivedPackets = 26731;
                statistics.missingPackets = 270;
                statistics.windowDurationMs = 1800;
                if (statistics.reason == QStringLiteral("valid"))
                    statistics.rawLossPercent = 1.0;
                view.status->networkStatistics = statistics;
                view.readOnly = !control;
                view.networkDeadline =
                    std::chrono::steady_clock::now() +
                    std::chrono::seconds(reason == QStringLiteral("feedback_stale") ? -1 : 2);
                panel.updateTransportPolicyState(control, view);
                openPolicy(panel);
                panel.m_ContentSlideAnim->stop();
                panel.m_ContentOffset = 0;
                QVERIFY(!panel.rawNetworkLossLabel().isEmpty());
                if (reason == QStringLiteral("valid"))
                    QCOMPARE(panel.rawNetworkLossLabel(), QStringLiteral("1.00%"));
                else
                    QVERIFY(!panel.rawNetworkLossLabel().contains('%'));
                if (!output.isEmpty()) {
                    QVERIFY(QDir().mkpath(output));
                    QImage preview(panel.size(), QImage::Format_ARGB32_Premultiplied);
                    preview.fill(Qt::transparent);
                    QPainter painter(&preview);
                    panel.paintOverlay(painter);
                    painter.end();
                    QVERIFY(preview.save(output +
                                         QStringLiteral("/menu-%1-%2.png")
                                             .arg(control ? "control" : "stats-only", reason)));
                }
                panel.closeMenu();
            }
        }
    }
    void rawLossExpiresInTheProductionMenuWithoutAnotherHttpResponse()
    {
        OverlayMenuPanel panel;
        auto view = sample();
        TransportPolicy::NetworkStatistics statistics;
        statistics.connectionEpoch = view.status->connectionEpoch;
        statistics.reason = QStringLiteral("valid");
        statistics.fresh = true;
        statistics.rawLossPercent = 0.0;
        statistics.receivedPackets = 250;
        statistics.windowDurationMs = 1800;
        view.status->networkStatistics = statistics;
        view.readOnly = true;
        view.networkDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        panel.updateTransportPolicyState(false, view);
        QCOMPARE(panel.rawNetworkLossLabel(), QStringLiteral("0.00%"));
        QCOMPARE(panel.networkSampleLabel(), QStringLiteral("1800 ms; 250 packets"));
        const auto previous = panel.m_LastTransportFingerprint;
        QTest::qWait(150);
        panel.updateTransportPolicyState(false, view);
        QCOMPARE(panel.rawNetworkLossLabel(), QStringLiteral("Feedback expired"));
        QVERIFY(panel.m_LastTransportFingerprint != previous);
        const auto& rows = panel.m_MenuLevels[2].items;
        QCOMPARE(rows[rows.size() - 2].detail, QStringLiteral("Feedback expired"));
        QVERIFY(!rows[rows.size() - 2].enabled);
        QCOMPARE(rows[0].type, OverlayMenuPanel::MenuItemType::Slider);
    }
    void initTestCase()
    {
        QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Manrope-Regular.ttf"));
        QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Manrope-SemiBold.ttf"));
        QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/DMMono-Regular.ttf"));
    }
    void independentModesDoNotChangeBeforeHostAcknowledgement()
    {
        for (bool bitrate : { false, true })
            for (bool fec : { false, true }) {
                OverlayMenuPanel panel;
                auto view = sample();
                view.status->accepted.automatic =
                    TransportPolicy::AutomaticControl{ bitrate, fec, 12000 };
                view.status->automaticFecAvailable = true;
                panel.updateTransportPolicyState(true, view);
                openPolicy(panel);
                int calls = 0;
                panel.setTransportChangeCallback([&](bool b, bool f, int maximum, bool manual) {
                    ++calls;
                    QCOMPARE(b, !bitrate);
                    QCOMPARE(f, fec);
                    QCOMPARE(maximum, 12000);
                    QVERIFY(!manual);
                });
                click(panel, OverlayMenuPanel::MenuAction::ToggleAutomaticBitrate);
                QCOMPARE(calls, 1);
                QCOMPARE(panel.m_MenuLevels[2].items[0].toggleState, bitrate);
                panel.setTransportChangeCallback([&](bool b, bool f, int maximum, bool manual) {
                    ++calls;
                    QCOMPARE(b, bitrate);
                    QCOMPARE(f, !fec);
                    QCOMPARE(maximum, 12000);
                    QVERIFY(!manual);
                });
                panel.m_HoveredIndex = 1;
                panel.gamepadSelect();
                QCOMPARE(calls, 2);
                QCOMPARE(panel.m_MenuLevels[2].items[1].toggleState, fec);
                QVERIFY(panel.isMenuVisible());
                panel.closeMenu();
            }
    }
    void currentPolicyAndOperationUseDifferentRevisions()
    {
        OverlayMenuPanel panel;
        auto view = sample();
        const auto previous = view.status->accepted;
        view.requestRevision = previous.revision;
        auto pending = previous;
        pending.revision = QString::number(previous.revision.toULongLong() + 1);
        view.status->accepted = pending;
        view.status->receipts.push_back({ pending, false, std::nullopt, QStringLiteral("none") });
        panel.updateTransportPolicyState(true, view);
        QVERIFY(panel.transportCurrentLabel().contains(QStringLiteral("Pending")));
        QVERIFY(panel.transportOperationLabel().contains(QStringLiteral("First packet sent")));
        auto& receipt = view.status->receipts.last();
        receipt.encoderApplied = true;
        panel.updateTransportPolicyState(true, view);
        QVERIFY(panel.transportCurrentLabel().contains(QStringLiteral("SDK applied")));
        receipt.firstSentFrame = QStringLiteral("900");
        panel.updateTransportPolicyState(true, view);
        QVERIFY(panel.transportCurrentLabel().contains(QStringLiteral("First packet sent")));
        receipt.failure = QStringLiteral("backend_failure");
        panel.updateTransportPolicyState(true, view);
        QVERIFY(panel.transportCurrentLabel().contains(QStringLiteral("Failed")));
        view.submitting = true;
        panel.updateTransportPolicyState(true, view);
        QCOMPARE(panel.transportOperationLabel(), QStringLiteral("Submitting"));
        view.submitting = false;
        view.requestError = QStringLiteral("Reply lost");
        view.requestRevision.reset();
        panel.updateTransportPolicyState(true, view);
        QCOMPARE(panel.transportOperationLabel(), QStringLiteral("Result unconfirmed"));
        view.requestError.clear();
        view.requestRevision = QStringLiteral("999");
        panel.updateTransportPolicyState(true, view);
        QVERIFY(panel.transportOperationLabel().contains(QStringLiteral("Result unknown")));
    }
    void unavailableAutomaticFecCannotBeActivatedAndCapabilityChangesRebuildMenu()
    {
        OverlayMenuPanel panel;
        auto view = sample();
        view.status->accepted.automatic = TransportPolicy::AutomaticControl{ false, false, 12000 };
        panel.updateTransportPolicyState(true, view);
        openPolicy(panel);
        int calls = 0;
        panel.setTransportChangeCallback([&](bool, bool fec, int, bool) {
            QVERIFY(!fec);
            ++calls;
        });
        QVERIFY(!panel.m_MenuLevels[2].items[1].enabled);
        view.status->accepted.automatic->fec = true;
        panel.updateTransportPolicyState(true, view);
        QVERIFY(
            panel.dispatchTransportAction(OverlayMenuPanel::MenuAction::ToggleAutomaticBitrate));
        QCOMPARE(calls, 1);
        click(panel, OverlayMenuPanel::MenuAction::ToggleAutomaticFec);
        QVERIFY(panel.dispatchTransportAction(OverlayMenuPanel::MenuAction::ToggleAutomaticFec));
        QCOMPARE(calls, 1);
        QVERIFY(panel.m_MenuLevels[2].items[0].enabled);
        view.status->automaticFecAvailable = true;
        panel.updateTransportPolicyState(true, view);
        QVERIFY(panel.m_MenuLevels[2].items[1].enabled);
        view.status->automaticFecAvailable = false;
        panel.updateTransportPolicyState(true, view);
        QVERIFY(!panel.m_MenuLevels[2].items[1].enabled);
    }

    void unavailableBusyAndStaleDisableEveryWriteAndCancelDrafts()
    {
        for (int reason = 0; reason < 5; ++reason) {
            OverlayMenuPanel panel;
            auto view = sample();
            view.status->accepted.automatic =
                TransportPolicy::AutomaticControl{ true, true, 12000 };
            panel.updateTransportPolicyState(true, view);
            openPolicy(panel);
            int calls = 0;
            panel.setTransportChangeCallback([&](bool, bool, int, bool) { ++calls; });
            panel.setBitrateChangeCallback([&](int) { ++calls; });
            panel.setBitrateKbps(20000);
            QVERIFY(panel.m_BitrateCommitTimer.isActive());
            if (reason == 0)
                view.fresh = false;
            if (reason == 1)
                view.submitting = true;
            if (reason == 2)
                view.status->stopped = true;
            if (reason == 3)
                view.status->liveControlAvailable = false;
            if (reason == 4)
                view.status.reset();
            panel.updateTransportPolicyState(true, view);
            for (const auto& item : panel.m_MenuLevels[2].items)
                QVERIFY(!item.enabled);
            click(panel, OverlayMenuPanel::MenuAction::ToggleAutomaticBitrate);
            click(panel, OverlayMenuPanel::MenuAction::TakeManualTransportControl);
            panel.setBitrateKbps(30000);
            panel.commitBitrateNow();
            QTest::qWait(500);
            QCOMPARE(calls, 0);
            QVERIFY(!panel.m_BitrateCommitTimer.isActive());
            panel.closeMenu();
        }
    }
    void manualControlFoldsOneDraftAndKeepsMenuOpen()
    {
        OverlayMenuPanel panel;
        auto view = sample();
        panel.updateTransportPolicyState(true, view);
        openPolicy(panel);
        int manualCalls = 0, bitrateCalls = 0;
        panel.setBitrateChangeCallback([&](int) { ++bitrateCalls; });
        panel.setTransportChangeCallback([&](bool, bool, int total, bool manual) {
            ++manualCalls;
            QCOMPARE(total, 20000);
            QVERIFY(manual);
        });
        panel.setBitrateKbps(20000);
        click(panel, OverlayMenuPanel::MenuAction::TakeManualTransportControl);
        QTest::qWait(500);
        QCOMPARE(manualCalls, 1);
        QCOMPARE(bitrateCalls, 0);
        QVERIFY(panel.isMenuVisible());
        panel.closeMenu();
    }
    void queryChangesDoNotRebuildUnchangedMenuAndLegacyLayoutSurvives()
    {
        OverlayMenuPanel panel;
        auto view = sample();
        panel.updateTransportPolicyState(true, view);
        const auto data = panel.m_MenuLevels[2].items.data();
        panel.updateTransportPolicyState(true, view);
        QCOMPARE(panel.m_MenuLevels[2].items.data(), data);
        view.status->confirmed.reset();
        view.status->encoderReady = false;
        panel.updateTransportPolicyState(true, view);
        QCOMPARE(panel.transportEncoderLabel(), QStringLiteral("Unconfirmed"));
        panel.updateTransportPolicyState(false, {});
        QCOMPARE(panel.m_MenuLevels[2].items.size(), size_t(11));
        QCOMPARE(panel.m_MenuLevels[2].items[0].type, OverlayMenuPanel::MenuItemType::Slider);
    }
};
QTEST_MAIN(TransportPolicyMenuTest)
#include "main.moc"
