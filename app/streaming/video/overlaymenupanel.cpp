#include "overlaymenupanel.h"
#include "uifont.h"

#include <QScreen>
#include <QGuiApplication>
#include <QCoreApplication>
#include <QCursor>
#include <QFontMetrics>
#include <memory>

namespace {
constexpr qint64 PointerGracePeriodMs = 300;
constexpr int PointerCheckIntervalMs = 150;
// Raster counterpart of gui/theme/Theme.qml (same palette and hard edges).
const QColor MenuSurface("#171A20");
const QColor MenuHover("#1F232B");
const QColor MenuLine("#3C434E");
const QColor MenuText("#EEF0EC");
const QColor MenuDim("#AEB3AB");
const QColor MenuFaint("#7E858E");
const QColor MenuAccent("#39C5BB");
const QColor MenuDanger("#FF876F");

// Bitrate scrubber range and granularity — log scale like the settings page
// slider. The maximum matches Sunshine's /bitrate runtime endpoint cap
// (800000 Kbps); higher values would be rejected by the host.
constexpr int kBitrateMinKbps = 500;
constexpr int kBitrateMaxKbps = 800000;
constexpr int kBitrateLogSteps = 200;
const double kBitrateLogSpan = qLn(kBitrateMaxKbps / double(kBitrateMinKbps));
// Idle window after the last scrub tick before the change is committed.
constexpr int kBitrateCommitDelayMs = 450;
}

OverlayMenuPanel::OverlayMenuPanel(QWindow* parent)
    : QRasterWindow(parent),
      m_CurrentLevel(0),
      m_HoveredIndex(-1),
      m_Visible(false),
      m_HasGamepads(false),
      m_FileMappingState(FileMappingState::Unknown),
      m_FileMappingDetail(tr("Checking")),
      m_RemoteUsbAvailable(false),
      m_RemoteUsbState(RemoteUsbState::Unavailable),
      m_RemoteUsbDetail(tr("Unavailable")),
      m_ParentX(0), m_ParentY(0), m_ParentW(0), m_ParentH(0),
      m_CloseWhenPointerOutside(false),
      m_ContentOffset(0),
      m_Closing(false),
      m_TargetPosition(),
      m_AnchorMode(AnchorMode::RightEdge),
      m_TriggerPosition(std::nullopt)
{
    setFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
             | Qt::WindowDoesNotAcceptFocus);

    QSurfaceFormat fmt;
    fmt.setAlphaBufferSize(8);
    setFormat(fmt);

    // Logical (unscaled) values — Qt 6 handles DPI automatically
    // Match the application's square industrial panels.
    m_ItemHeight   = 38;
    m_Padding      = 4;
    m_MenuWidth    = 320;
    m_ShadowMargin = 8;
    m_TitleHeight  = 32;
    m_IconAreaWidth = 28;

    m_LabelFont.setFamilies(UiFont::familyChain(QStringLiteral("Manrope")));
    m_LabelFont.setPointSize(10);
    m_LabelFont.setWeight(QFont::DemiBold);

    m_DetailFont.setFamilies(UiFont::familyChain(QStringLiteral("DM Mono")));
    m_DetailFont.setPointSize(8);

    m_TitleFont = m_DetailFont;
    m_TitleFont.setPointSize(9);
    m_TitleFont.setWeight(QFont::DemiBold);
    m_TitleFont.setLetterSpacing(QFont::AbsoluteSpacing, 1.5);

    // Reuse the same bundled Fluent 24 Regular assets as settings/toolbars.
    // Keep QIcon instances alive so Qt can cache rasterizations for each DPI.
    for (const QString &name : {QStringLiteral("tb-settings"), QStringLiteral("menu-position"),
             QStringLiteral("menu-bitrate"), QStringLiteral("menu-files"),
             QStringLiteral("cat-peripherals"), QStringLiteral("cat-display"),
             QStringLiteral("menu-microphone"), QStringLiteral("cat-gamepad"),
             QStringLiteral("menu-close"), QStringLiteral("menu-next"),
             QStringLiteral("tb-back")}) {
        m_MenuIcons.insert(name, QIcon(QStringLiteral(":/res/fluent/%1.svg").arg(name)));
    }

    // --- Animations ---
    m_OpacityAnim = new QPropertyAnimation(this, "opacity", this);
    m_SlideAnim   = new QPropertyAnimation(this, "x", this);

    m_ContentSlideAnim = new QVariantAnimation(this);
    m_ContentSlideAnim->setDuration(150);
    m_ContentSlideAnim->setEasingCurve(QEasingCurve::OutQuad);
    connect(m_ContentSlideAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant& val) {
        m_ContentOffset = val.toReal();
        forceRepaint();
    });
    connect(m_ContentSlideAnim, &QVariantAnimation::finished, this, [this]() {
        m_ContentOffset = 0;
        forceRepaint();
    });

    m_LeaveTimer.setSingleShot(true);
    connect(&m_LeaveTimer, &QTimer::timeout, this, [this]() {
        if (!m_Visible || !m_CloseWhenPointerOutside) {
            return;
        }

        const QRect contentGeometry = geometry().adjusted(
                m_ShadowMargin, m_ShadowMargin,
                -m_ShadowMargin, -m_ShadowMargin);
        if (!contentGeometry.contains(QCursor::pos())) {
            closeMenu();
        }
        else {
            schedulePointerOutsideCheck();
        }
    });

    m_BitrateCommitTimer.setSingleShot(true);
    connect(&m_BitrateCommitTimer, &QTimer::timeout, this, [this]() {
        commitBitrateNow();
    });

    buildMenuLevels();
}

OverlayMenuPanel::~OverlayMenuPanel()
{
}

// ---------------------------------------------------------------------------
// Synchronous repaint — requestUpdate() is async on Windows and may delay
// the visual update by up to 1 second (until the next SDL event arrives).
// This method directly delivers an UpdateRequest event so the paintEvent()
// runs immediately within the current call frame.
// ---------------------------------------------------------------------------

void OverlayMenuPanel::forceRepaint()
{
    if (isExposed()) {
        // Mark entire window as dirty (sets the dirty region for QPaintDeviceWindow)
        update(QRect(0, 0, width(), height()));
        // Synchronously deliver UpdateRequest to trigger paintEvent + backing store flush
        QEvent ev(QEvent::UpdateRequest);
        QCoreApplication::sendEvent(this, &ev);
    }
}

// ---------------------------------------------------------------------------
// Menu structure
// ---------------------------------------------------------------------------

void OverlayMenuPanel::buildMenuLevels()
{
    m_MenuLevels.clear();

    // === Level 0: Top-level categories ===
    MenuLevel top;
    top.title = tr("Overlay Menu");
    top.items.push_back({tr("Quick Actions"), QString(),  MenuItemType::SubMenu,
                         MenuAction::MenuActionMax, 1, true, false, false});
    top.items.push_back({tr("Menu Position"), QString(), MenuItemType::SubMenu,
                         MenuAction::MenuActionMax, 3, true, false, false});
    top.items.push_back({tr("Bitrate"),       QString(),  MenuItemType::SubMenu,
                         MenuAction::MenuActionMax, 2, true, false, false});
    const bool separatorAfterHostFiles =
#ifdef MOONLIGHT_ENABLE_FUNCTION_TESTS
            false;
#else
            !m_RemoteUsbAvailable;
#endif
    top.items.push_back({tr("Host Files"),    m_FileMappingDetail, MenuItemType::Action,
                         MenuAction::ShowHostFiles, 0, true,
                         m_FileMappingState == FileMappingState::Available ||
                         m_FileMappingState == FileMappingState::Open,
                         separatorAfterHostFiles});
    if (m_RemoteUsbAvailable) {
        top.items.push_back({tr("USB Devices"), m_RemoteUsbDetail,
                             MenuItemType::SubMenu,
                             MenuAction::MenuActionMax, 4, true, false,
#ifdef MOONLIGHT_ENABLE_FUNCTION_TESTS
                             false});
#else
                             true});
#endif
    }
#ifdef MOONLIGHT_ENABLE_FUNCTION_TESTS
    top.items.push_back({tr("Function Tests"),
                         tr("Developer"),
                         MenuItemType::Action,
                         MenuAction::OpenStylusReplayPanel, 0, true, false, true});
#endif
    top.items.push_back({tr("Toggle Fullscreen"), QString(), MenuItemType::Action,
                         MenuAction::ToggleFullScreen, 0, true, false, false});
    top.items.push_back({tr("Microphone"),    QString(),  MenuItemType::Toggle,
                         MenuAction::ToggleMicrophone, 0, true, false, !m_HasGamepads}); // separator if no gamepad item follows
    // Only show Gamepad Mouse toggle when a gamepad is actually connected
    if (m_HasGamepads) {
        top.items.push_back({tr("Gamepad Mouse"), QString(),  MenuItemType::Toggle,
                             MenuAction::ToggleGamepadMouse, 0, true, false, true}); // separator
    }
    top.items.push_back({tr("Disconnect"),    QString(),  MenuItemType::Action,
                         MenuAction::Quit, 0, true, false, false});
    m_MenuLevels.push_back(top);

    // === Level 1: Quick Actions (keyboard shortcuts) ===
    MenuLevel shortcuts;
    shortcuts.title = tr("Quick Actions");
    shortcuts.items.push_back({tr("Quit Moonlight"),      "Ctrl+Alt+Shift+E", MenuItemType::Action,
                               MenuAction::QuitAndExit,           0, true, false, true});
    shortcuts.items.push_back({tr("Performance Stats"),   "Ctrl+Alt+Shift+S", MenuItemType::Action,
                               MenuAction::ToggleStatsOverlay,    0, true, false, true});
    shortcuts.items.push_back({tr("Mouse Mode"),          "Ctrl+Alt+Shift+M", MenuItemType::Action,
                               MenuAction::ToggleMouseMode,       0, true, false, false});
    shortcuts.items.push_back({tr("Show/Hide Cursor"),    "Ctrl+Alt+Shift+C", MenuItemType::Action,
                               MenuAction::ToggleCursorHide,      0, true, false, false});
    shortcuts.items.push_back({tr("Minimize"),            "Ctrl+Alt+Shift+D", MenuItemType::Action,
                               MenuAction::ToggleMinimize,        0, true, false, true});
    shortcuts.items.push_back({tr("Ungrab Mouse"),        "Ctrl+Alt+Shift+Z", MenuItemType::Action,
                               MenuAction::UngrabInput,           0, true, false, false});
    shortcuts.items.push_back({tr("Paste Clipboard"),     "Ctrl+Alt+Shift+V", MenuItemType::Action,
                               MenuAction::PasteText,             0, true, false, false});
    shortcuts.items.push_back({tr("Pointer Region Lock"), "Ctrl+Alt+Shift+L", MenuItemType::Action,
                               MenuAction::TogglePointerRegionLock, 0, true, false, false});
    m_MenuLevels.push_back(shortcuts);

    // === Level 2: Bitrate (log-scale scrubber row + presets) ===
    MenuLevel bitrate;
    bitrate.title = tr("Bitrate");
    bitrate.items.push_back({QString(), QString(), MenuItemType::Slider,
                             MenuAction::MenuActionMax, 0, true, false, true});
    static const int kBitratePresets[] = {
        1000, 2000, 5000, 10000, 20000, 30000, 50000, 100000
    };
    for (int kbps : kBitratePresets) {
        bitrate.items.push_back({formatBitrateKbps(kbps), QString(), MenuItemType::Action,
                                 MenuAction::SetBitrate, 0, true, false, false,
                                 QString::number(kbps)});
    }
    m_MenuLevels.push_back(bitrate);

    // === Level 3: Overlay menu placement ===
    MenuLevel placement;
    placement.title = tr("Menu Position");
    placement.items.push_back({tr("Top edge"), QString(), MenuItemType::Action,
                               MenuAction::SetMenuPlacementTop, 0, true, false, false});
    placement.items.push_back({tr("Right edge"), QString(), MenuItemType::Action,
                               MenuAction::SetMenuPlacementRight, 0, true, false, false});
    placement.items.push_back({tr("Left edge"), QString(), MenuItemType::Action,
                               MenuAction::SetMenuPlacementLeft, 0, true, false, false});
    placement.items.push_back({tr("Floating button"), QString(), MenuItemType::Action,
                               MenuAction::SetMenuPlacementButton, 0, true, false, false});
    placement.items.push_back({tr("Disabled"), QString(), MenuItemType::Action,
                               MenuAction::SetMenuPlacementDisabled, 0, true, false, false});
    m_MenuLevels.push_back(placement);

    // === Level 4: Remote USB devices ===
    if (m_RemoteUsbAvailable) {
        MenuLevel usb;
        usb.title = tr("USB Devices");
        if (m_RemoteUsbDevices.empty()) {
            const QString emptyDetail =
                m_RemoteUsbState == RemoteUsbState::Discovering
                    ? tr("Scanning") : tr("No devices found");
            usb.items.push_back({tr("USB Devices"), emptyDetail,
                                 MenuItemType::Action,
                                 MenuAction::MenuActionMax, 0, false, false,
                                 false});
        }
        else {
            for (const RemoteUsbDevice& device : m_RemoteUsbDevices) {
                const bool active = !m_RemoteUsbActiveDeviceId.isEmpty() &&
                                    device.id == m_RemoteUsbActiveDeviceId;
                QString detail = device.detail;
                if (active) {
                    switch (m_RemoteUsbState) {
                    case RemoteUsbState::Opening:
                        detail = tr("Connecting — select to cancel");
                        break;
                    case RemoteUsbState::Open:
                        detail = tr("Connected — select to release");
                        break;
                    case RemoteUsbState::Stopping:
                        detail = tr("Releasing");
                        break;
                    default:
                        break;
                    }
                }
                const bool busy = m_RemoteUsbState == RemoteUsbState::Opening ||
                                  m_RemoteUsbState == RemoteUsbState::Stopping;
                const bool hasOpenDevice =
                    m_RemoteUsbState == RemoteUsbState::Open &&
                    !m_RemoteUsbActiveDeviceId.isEmpty();
                const bool canRelease = active &&
                    (m_RemoteUsbState == RemoteUsbState::Opening ||
                     m_RemoteUsbState == RemoteUsbState::Open);
                usb.items.push_back({device.label, detail,
                                     MenuItemType::Action,
                                     canRelease
                                         ? MenuAction::ReleaseRemoteUsbDevice
                                         : MenuAction::SelectRemoteUsbDevice,
                                     0,
                                     canRelease || (device.supported && !busy &&
                                         (!hasOpenDevice || active)),
                                     active,
                                     false,
                                     device.id});
            }
        }
        m_MenuLevels.push_back(std::move(usb));
    }

    if (m_CurrentLevel >= static_cast<int>(m_MenuLevels.size())) {
        m_CurrentLevel = 0;
    }

    // Rebuilds (gamepad set / USB refresh) wipe derived details; re-stamp
    // the bitrate state so the scrubber row and preset checkmarks survive.
    refreshBitrateDetails();
}

// ---------------------------------------------------------------------------
// Dynamic state updates
// ---------------------------------------------------------------------------

void OverlayMenuPanel::updateMicrophoneState(bool enabled)
{
    if (m_MenuLevels.empty()) return;
    for (auto& item : m_MenuLevels[0].items) {
        if (item.action == MenuAction::ToggleMicrophone) {
            item.toggleState = enabled;
            forceRepaint();
            break;
        }
    }
}

void OverlayMenuPanel::updateGamepadMouseState(bool enabled)
{
    if (m_MenuLevels.empty()) return;
    for (auto& item : m_MenuLevels[0].items) {
        if (item.action == MenuAction::ToggleGamepadMouse) {
            item.toggleState = enabled;
            forceRepaint();
            break;
        }
    }
}

void OverlayMenuPanel::updateBitrateState(int bitrateKbps)
{
    if (m_MenuLevels.empty()) return;

    if (m_BitrateCommitTimer.isActive()) {
        // A scrub is still pending commit; it will land shortly and update
        // the preference. Don't clobber the slider with the stale value.
        return;
    }

    m_BitrateKbps = qBound(kBitrateMinKbps, bitrateKbps, kBitrateMaxKbps);
    m_CommittedBitrateKbps = m_BitrateKbps;
    refreshBitrateDetails();
    forceRepaint();
}

// ---------------------------------------------------------------------------
// Bitrate slider row
// ---------------------------------------------------------------------------

QString OverlayMenuPanel::formatBitrateKbps(int kbps)
{
    if (kbps >= 1000000) {
        const bool whole = kbps % 1000000 == 0;
        return QStringLiteral("%1 Gbps").arg(kbps / 1000000.0, 0, 'f', whole ? 0 : 1);
    }
    if (kbps >= 1000) {
        const bool whole = kbps % 1000 == 0;
        return QStringLiteral("%1 Mbps").arg(kbps / 1000.0, 0, 'f', whole ? 0 : 1);
    }
    return QStringLiteral("%1 kbps").arg(kbps);
}

void OverlayMenuPanel::refreshBitrateDetails()
{
    if (m_MenuLevels.empty()) return;

    // Current bitrate as detail text on the Bitrate category (level 0)
    for (auto& item : m_MenuLevels[0].items) {
        if (item.type == MenuItemType::SubMenu && item.targetLevel == 2) {
            item.detail = formatBitrateKbps(m_BitrateKbps);
            break;
        }
    }

    // Mark the active preset (✓) in level 2; custom values stay unmarked
    // since the scrubber row itself shows the exact value.
    if ((int)m_MenuLevels.size() > 2) {
        for (auto& item : m_MenuLevels[2].items) {
            if (item.action == MenuAction::SetBitrate) {
                item.detail = (item.payload.toInt() == m_BitrateKbps)
                                  ? QString::fromUtf8("\342\234\223") : QString();
            }
        }
    }
}

double OverlayMenuPanel::bitrateFraction() const
{
    return qBound(0.0, qLn(m_BitrateKbps / double(kBitrateMinKbps)) / kBitrateLogSpan, 1.0);
}

void OverlayMenuPanel::setBitrateKbps(int bitrateKbps)
{
    bitrateKbps = qBound(kBitrateMinKbps, bitrateKbps, kBitrateMaxKbps);
    // Round to display-friendly granularity so the label doesn't flicker
    // while scrubbing (also keeps committed values tidy).
    if (bitrateKbps < 10000) {
        bitrateKbps = qRound(bitrateKbps / 50.0) * 50;
    } else if (bitrateKbps < 100000) {
        bitrateKbps = qRound(bitrateKbps / 500.0) * 500;
    } else {
        bitrateKbps = qRound(bitrateKbps / 5000.0) * 5000;
    }
    bitrateKbps = qBound(kBitrateMinKbps, bitrateKbps, kBitrateMaxKbps);
    if (bitrateKbps == m_BitrateKbps) return;

    m_BitrateKbps = bitrateKbps;
    refreshBitrateDetails();
    m_BitrateCommitTimer.start(kBitrateCommitDelayMs);
    forceRepaint();
}

void OverlayMenuPanel::setBitrateFromFraction(double fraction)
{
    fraction = qBound(0.0, fraction, 1.0);
    setBitrateKbps(qRound(kBitrateMinKbps * qExp(fraction * kBitrateLogSpan)));
}

void OverlayMenuPanel::adjustBitrateStep(int direction, int multiplier)
{
    const double step = (kBitrateLogSpan / kBitrateLogSteps) * qMax(1, multiplier);
    setBitrateFromFraction(bitrateFraction() + direction * step);
}

void OverlayMenuPanel::commitBitrateNow()
{
    m_BitrateCommitTimer.stop();
    if (m_BitrateKbps == m_CommittedBitrateKbps) return;

    m_CommittedBitrateKbps = m_BitrateKbps;
    if (m_BitrateChangeCallback) {
        m_BitrateChangeCallback(m_BitrateKbps);
    }
}

void OverlayMenuPanel::selectBitratePreset(const MenuItem& item)
{
    // Presets snap the scrubber and commit immediately, but keep the menu
    // open so the value can be fine-tuned right away.
    beginInteraction();
    m_BitrateKbps = qBound(kBitrateMinKbps, item.payload.toInt(), kBitrateMaxKbps);
    refreshBitrateDetails();
    commitBitrateNow();
    forceRepaint();
}

OverlayMenuPanel::SliderRowRects OverlayMenuPanel::sliderRowRects(int contentWidth, int itemY) const
{
    const int textPad = 16;
    const int buttonSize = 22;
    const int buttonGap = 4;

    SliderRowRects r;
    r.value = QRect(textPad, itemY, 78, m_ItemHeight);
    r.plus = QRect(contentWidth - textPad - buttonSize,
                   itemY + (m_ItemHeight - buttonSize) / 2, buttonSize, buttonSize);
    r.minus = QRect(r.plus.x() - buttonSize - buttonGap, r.plus.y(),
                    buttonSize, buttonSize);
    const int trackLeft = r.value.right() + 1 + 8;
    const int trackRight = r.minus.x() - 6;
    r.track = QRect(trackLeft, itemY + m_ItemHeight / 2 - 3,
                    qMax(0, trackRight - trackLeft), 6);
    return r;
}

OverlayMenuPanel::SliderZone OverlayMenuPanel::sliderZoneAt(const QPoint& localPos, int rowIdx) const
{
    const auto& items = m_MenuLevels[m_CurrentLevel].items;
    if (rowIdx < 0 || rowIdx >= (int)items.size()
            || items[rowIdx].type != MenuItemType::Slider) {
        return SliderZone::None;
    }

    const int itemY = m_TitleHeight + m_Padding + rowIdx * m_ItemHeight;
    const SliderRowRects r = sliderRowRects(width() - 2 * m_ShadowMargin, itemY);

    // Buttons win over the track so their hit area feels solid.
    if (r.minus.adjusted(-2, -2, 2, 2).contains(localPos)) return SliderZone::Minus;
    if (r.plus.adjusted(-2, -2, 2, 2).contains(localPos)) return SliderZone::Plus;
    if (QRect(r.track.x() - 4, itemY, r.track.width() + 8, m_ItemHeight).contains(localPos)) {
        return SliderZone::Track;
    }
    return SliderZone::None;
}

void OverlayMenuPanel::gamepadAdjustSlider(int direction)
{
    if (!m_Visible) return;
    const auto& items = m_MenuLevels[m_CurrentLevel].items;

    // D-pad left/right anywhere in the scrubber's submenu drives the slider;
    // focus follows so the highlight shows what is being adjusted.
    int rowIdx = -1;
    for (int i = 0; i < (int)items.size(); i++) {
        if (items[i].type == MenuItemType::Slider) {
            rowIdx = i;
            break;
        }
    }
    if (rowIdx < 0) return;

    beginInteraction();
    if (m_HoveredIndex != rowIdx) {
        m_HoveredIndex = rowIdx;
        m_SliderAdjustStreak = 0;
        forceRepaint();
    }
    if (m_SliderAdjustClock.isValid() && m_SliderAdjustClock.elapsed() <= 350) {
        m_SliderAdjustStreak++;
    } else {
        m_SliderAdjustStreak = 0;
    }
    m_SliderAdjustClock.start();
    // 1x → 2x → 4x → 8x (capped) while the d-pad is held or rapidly tapped.
    adjustBitrateStep(direction, 1 << qMin(m_SliderAdjustStreak / 2, 3));
}

void OverlayMenuPanel::updateMenuPositionState(MenuAction activePlacementAction)
{
    if (m_MenuLevels.size() <= 3) {
        return;
    }

    QString activeLabel;
    for (auto& item : m_MenuLevels[3].items) {
        const bool active = item.action == activePlacementAction;
        item.detail = active ? QString::fromUtf8("\342\234\223") : QString();
        if (active) {
            activeLabel = item.label;
        }
    }

    for (auto& item : m_MenuLevels[0].items) {
        if (item.type == MenuItemType::SubMenu && item.targetLevel == 3) {
            item.detail = activeLabel;
            break;
        }
    }
    forceRepaint();
}

void OverlayMenuPanel::updateFileMappingState(FileMappingState state, const QString& detail)
{
    m_FileMappingState = state;
    m_FileMappingDetail = detail;

    if (m_MenuLevels.empty()) return;
    for (auto& item : m_MenuLevels[0].items) {
        if (item.action == MenuAction::ShowHostFiles) {
            item.detail = detail;
            item.toggleState = state == FileMappingState::Available ||
                               state == FileMappingState::Open;
            forceRepaint();
            break;
        }
    }
}

void OverlayMenuPanel::updateRemoteUsbState(
    bool available,
    RemoteUsbState state,
    std::vector<RemoteUsbDevice> devices,
    const QString& activeDeviceId,
    const QString& detail)
{
    m_RemoteUsbAvailable = available;
    m_RemoteUsbState = state;
    m_RemoteUsbDevices = std::move(devices);
    m_RemoteUsbActiveDeviceId = activeDeviceId;
    m_RemoteUsbDetail = detail;
    const int previousLevel = m_CurrentLevel;
    buildMenuLevels();
    if (m_Visible && previousLevel == 4 && m_MenuLevels.size() > 4) {
        m_CurrentLevel = 4;
    }
    if (m_Visible) {
        repositionWindow();
    }
    forceRepaint();
}

void OverlayMenuPanel::dispatchActionItem(const MenuItem& item)
{
    // USB callbacks can synchronously rebuild the device list. Own the payload
    // before calling out so a refresh cannot invalidate the selected identity.
    const QString payload = item.payload;
    if (item.action == MenuAction::SelectRemoteUsbDevice) {
        beginInteraction();
        if (m_RemoteUsbDeviceCallback) {
            m_RemoteUsbDeviceCallback(payload);
        }
        return;
    }
    if (item.action == MenuAction::ReleaseRemoteUsbDevice) {
        beginInteraction();
        if (m_RemoteUsbReleaseCallback) {
            m_RemoteUsbReleaseCallback();
        }
        return;
    }
    closeMenu();
    if (m_ActionCallback) {
        m_ActionCallback(item.action);
    }
}

// ---------------------------------------------------------------------------
// Show / hide / navigate
// ---------------------------------------------------------------------------

void OverlayMenuPanel::showAtRightEdge(int parentX, int parentY, int parentW, int parentH,
                                       std::optional<QPoint> pointerGlobalPosition,
                                       bool closeWhenPointerOutside)
{
    m_AnchorMode = AnchorMode::RightEdge;
    m_ParentX = parentX;
    m_ParentY = parentY;
    m_ParentW = parentW;
    m_ParentH = parentH;
    m_TriggerPosition = pointerGlobalPosition;
    m_CloseWhenPointerOutside = closeWhenPointerOutside && pointerGlobalPosition.has_value();
    showInternal();
}

void OverlayMenuPanel::showAtLeftEdge(int parentX, int parentY, int parentW, int parentH,
                                      std::optional<QPoint> pointerGlobalPosition,
                                      bool closeWhenPointerOutside)
{
    m_AnchorMode = AnchorMode::LeftEdge;
    m_ParentX = parentX;
    m_ParentY = parentY;
    m_ParentW = parentW;
    m_ParentH = parentH;
    m_TriggerPosition = pointerGlobalPosition;
    m_CloseWhenPointerOutside = closeWhenPointerOutside && pointerGlobalPosition.has_value();
    showInternal();
}

void OverlayMenuPanel::showAtTopEdge(int parentX, int parentY, int parentW, int parentH,
                                     std::optional<QPoint> pointerGlobalPosition,
                                     bool closeWhenPointerOutside)
{
    m_AnchorMode = AnchorMode::TopEdge;
    m_ParentX = parentX;
    m_ParentY = parentY;
    m_ParentW = parentW;
    m_ParentH = parentH;
    m_TriggerPosition = pointerGlobalPosition;
    m_CloseWhenPointerOutside = closeWhenPointerOutside && pointerGlobalPosition.has_value();
    showInternal();
}

void OverlayMenuPanel::showAtCursor(int parentX, int parentY, int parentW, int parentH,
                                    const QPoint& cursorPosition, bool pointerTriggered)
{
    m_AnchorMode = AnchorMode::AtCursor;
    m_ParentX = parentX;
    m_ParentY = parentY;
    m_ParentW = parentW;
    m_ParentH = parentH;
    m_TriggerPosition = cursorPosition;
    m_CloseWhenPointerOutside = pointerTriggered;
    showInternal();
}

void OverlayMenuPanel::showInternal()
{
    m_LeaveTimer.stop();
    m_CurrentLevel = 0;
    m_HoveredIndex = -1;
    m_ContentOffset = 0;

    // If closing animation is in progress, cancel it
    if (m_Closing) {
        m_OpacityAnim->stop();
        m_SlideAnim->stop();
        m_Closing = false;
    }

    m_Visible = true;
    m_ShowTimer.start();

    // Calculate target geometry
    repositionWindow();
    m_TargetPosition = position();

    // Slide direction depends on anchor mode
    const bool verticalSlide = m_AnchorMode == AnchorMode::TopEdge;
    const int slideDirection = (m_AnchorMode == AnchorMode::LeftEdge || verticalSlide) ? -1 : 1;
    const int slideDistance = 8;
    const int targetCoordinate = verticalSlide ? m_TargetPosition.y() : m_TargetPosition.x();
    const int startCoordinate = targetCoordinate + slideDistance * slideDirection;
    m_SlideAnim->setPropertyName(verticalSlide ? QByteArrayLiteral("y") : QByteArrayLiteral("x"));
    if (verticalSlide) {
        setY(startCoordinate);
    }
    else {
        setX(startCoordinate);
    }
    setOpacity(0.0);

    show();
    raise();

    // Animate slide
    m_SlideAnim->setDuration(120);
    m_SlideAnim->setStartValue(startCoordinate);
    m_SlideAnim->setEndValue(targetCoordinate);
    m_SlideAnim->setEasingCurve(QEasingCurve::OutQuad);

    // Short, mechanical reveal.
    m_OpacityAnim->setDuration(120);
    m_OpacityAnim->setStartValue(0.0);
    m_OpacityAnim->setEndValue(1.0);
    m_OpacityAnim->setEasingCurve(QEasingCurve::OutQuad);

    m_SlideAnim->start();
    m_OpacityAnim->start();

    if (m_CloseWhenPointerOutside) {
        schedulePointerOutsideCheck();
    }

    forceRepaint();
}

void OverlayMenuPanel::schedulePointerOutsideCheck()
{
    if (!m_Visible || !m_CloseWhenPointerOutside) {
        return;
    }

    const qint64 remainingGrace = PointerGracePeriodMs - m_ShowTimer.elapsed();
    if (remainingGrace > 0) {
        m_LeaveTimer.start(static_cast<int>(remainingGrace));
    }
    else {
        m_LeaveTimer.start(PointerCheckIntervalMs);
    }
}

void OverlayMenuPanel::repositionWindow()
{
    int qpX = m_ParentX;
    int qpY = m_ParentY;
    int qpW = m_ParentW;
    int qpH = m_ParentH;

    int itemCount  = (int)m_MenuLevels[m_CurrentLevel].items.size();
    int titleH     = m_TitleHeight;
    int menuHeight = titleH + itemCount * m_ItemHeight + m_Padding * 2;

    const QPoint triggerPosition = m_TriggerPosition.value_or(QPoint());

    int cx, cy; // content top-left position

    switch (m_AnchorMode) {
    case AnchorMode::LeftEdge:
        cx = qpX;
        if (m_TriggerPosition.has_value()) {
            cy = triggerPosition.y() - m_TitleHeight - m_Padding - m_ItemHeight / 2;
        }
        else {
            cy = qpY + (qpH - menuHeight) / 2;
        }
        break;

    case AnchorMode::TopEdge:
        if (m_TriggerPosition.has_value()) {
            cx = triggerPosition.x() - m_MenuWidth / 2;
        }
        else {
            cx = qpX + (qpW - m_MenuWidth) / 2;
        }
        cy = qpY;
        break;

    case AnchorMode::AtCursor: {
        // Position menu so cursor is near top-left corner
        cx = triggerPosition.x();
        cy = triggerPosition.y();
        break;
    }

    case AnchorMode::RightEdge:
    default:
        cx = qpX + qpW - m_MenuWidth;
        if (m_TriggerPosition.has_value()) {
            cy = triggerPosition.y() - m_TitleHeight - m_Padding - m_ItemHeight / 2;
        }
        else {
            cy = qpY + (qpH - menuHeight) / 2;
        }
        break;
    }

    // Clamp within the parent. If the menu is larger than the streaming
    // window, keep its origin visible instead of pushing it past an edge.
    cx = qpW >= m_MenuWidth
            ? qBound(qpX, cx, qpX + qpW - m_MenuWidth)
            : qpX;
    cy = qpH >= menuHeight
            ? qBound(qpY, cy, qpY + qpH - menuHeight)
            : qpY;

    // Window includes shadow margin around content
    setGeometry(cx - m_ShadowMargin, cy - m_ShadowMargin,
                m_MenuWidth + 2 * m_ShadowMargin, menuHeight + 2 * m_ShadowMargin);
}

void OverlayMenuPanel::navigateToLevel(int level)
{
    if (level < 0 || level >= (int)m_MenuLevels.size()) return;

    // Explicit navigation commits to interacting with the menu. A shorter
    // submenu can resize out from under the pointer, so keep it open until
    // an action or explicit dismissal instead of racing a leave timeout.
    beginInteraction();
    bool goingForward = level > m_CurrentLevel;
    m_ContentSlideAnim->stop();
    m_ContentOffset = 0;

    m_CurrentLevel = level;
    m_HoveredIndex = -1;
    repositionWindow();

    if (goingForward) {
        // Forward: content slides in from right
        m_ContentSlideAnim->setStartValue(8.0);
        m_ContentSlideAnim->setEndValue(0.0);
        m_ContentSlideAnim->start();
    } else {
        // Back: instant switch, no animation (avoids jarring resize + slide combo)
        forceRepaint();
    }
}

void OverlayMenuPanel::beginInteraction()
{
    m_CloseWhenPointerOutside = false;
    m_LeaveTimer.stop();
}

void OverlayMenuPanel::dismissOnOutsideClick(const QPoint& globalPosition)
{
    if (m_Visible && !geometry().contains(globalPosition)) {
        closeMenu();
    }
}

void OverlayMenuPanel::closeMenu()
{
    m_LeaveTimer.stop();
    if (!m_Visible) return;
    if (m_Closing) return;  // already animating close

    m_Visible = false;
    m_Closing = true;
    m_HoveredIndex = -1;

    // Stop any show/level animations
    m_SlideAnim->stop();
    m_OpacityAnim->stop();
    m_ContentSlideAnim->stop();
    m_ContentOffset = 0;

    // Animate away from the edge that opened the menu.
    const bool verticalSlide = m_AnchorMode == AnchorMode::TopEdge;
    const int slideDirection = (m_AnchorMode == AnchorMode::LeftEdge || verticalSlide) ? -1 : 1;
    const int slideDistance = 8;
    const int startCoordinate = verticalSlide ? y() : x();
    m_SlideAnim->setPropertyName(verticalSlide ? QByteArrayLiteral("y") : QByteArrayLiteral("x"));
    m_SlideAnim->setDuration(120);
    m_SlideAnim->setStartValue(startCoordinate);
    m_SlideAnim->setEndValue(startCoordinate + slideDistance * slideDirection);
    m_SlideAnim->setEasingCurve(QEasingCurve::OutQuad);

    // Animate opacity: current → 0
    m_OpacityAnim->setDuration(120);
    m_OpacityAnim->setStartValue(opacity());
    m_OpacityAnim->setEndValue(0.0);
    m_OpacityAnim->setEasingCurve(QEasingCurve::OutQuad);

    // When fade-out completes, finalize (use disconnect to emulate single-shot for Qt 5 compat)
    auto conn = std::make_shared<QMetaObject::Connection>();
    *conn = connect(m_OpacityAnim, &QPropertyAnimation::finished, this, [this, conn]() {
        disconnect(*conn);
        m_Closing = false;
        m_CurrentLevel = 0;
        hide();
        setOpacity(1.0);   // reset for next show
        if (m_CloseCallback) {
            m_CloseCallback();
        }
    });

    m_SlideAnim->start();
    m_OpacityAnim->start();
}

int OverlayMenuPanel::itemAtPos(const QPoint& pos) const
{
    // Adjust for shadow margin
    int lx = pos.x() - m_ShadowMargin;
    int ly = pos.y() - m_ShadowMargin;
    if (lx < 0 || lx >= m_MenuWidth || ly < 0) return -1;

    int titleH = m_TitleHeight;

    if (ly < titleH) {
        if (lx >= m_MenuWidth - m_TitleHeight) {
            return -3; // close button
        }
        if (m_CurrentLevel > 0) {
            return -2; // back button
        }
        return -1;
    }
    int localY = ly - titleH - m_Padding;
    if (localY < 0) return -1;
    int idx = localY / m_ItemHeight;
    const auto& items = m_MenuLevels[m_CurrentLevel].items;
    if (idx < 0 || idx >= (int)items.size()) return -1;
    return idx;
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

void OverlayMenuPanel::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setRenderHint(QPainter::TextAntialiasing);

    int w = width();
    int h = height();
    int sm = m_ShadowMargin;
    int cw = w - 2 * sm;   // content width
    int ch = h - 2 * sm;   // content height

    // Clear to transparent
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.fillRect(0, 0, w, h, Qt::transparent);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);

    // The same 6px, zero-blur offset shadow used by Panel.qml.
    p.fillRect(QRect(sm + 6, sm + 6, cw, ch), QColor(0, 0, 0, 140));
    p.save();
    p.translate(sm, sm);
    p.fillRect(QRect(0, 0, cw, ch), MenuSurface);
    p.setPen(QPen(MenuLine, 1));
    p.drawRect(QRect(0, 0, cw - 1, ch - 1));
    p.setClipRect(QRect(1, 1, cw - 2, ch - 2));
    p.fillRect(QRect(1, 1, 4, m_TitleHeight - 1), MenuAccent);
    p.fillRect(QRect(1, m_TitleHeight - 1, cw - 2, 1), MenuLine);

    const auto drawIcon = [&](const QString &name, const QRect &rect, qreal opacity = 1.0) {
        const auto icon = m_MenuIcons.constFind(name);
        if (icon == m_MenuIcons.cend()) return;
        p.save();
        p.setOpacity(opacity);
        icon.value().paint(&p, rect, Qt::AlignCenter, QIcon::Normal, QIcon::Off);
        p.restore();
    };

    // --- Title bar: back navigation on sub-levels and close on every level ---
    const auto& level = m_MenuLevels[m_CurrentLevel];
    int textPad = 16;
    int titleH = m_TitleHeight;
    const bool backHovered = m_CurrentLevel > 0 && m_HoveredIndex == -2;
    const bool closeHovered = m_HoveredIndex == -3;

    if (backHovered) {
        p.fillRect(QRect(6, 1, cw - m_TitleHeight - 6, m_TitleHeight - 2), MenuHover);
    }

    const QRect closeRect(cw - m_TitleHeight, 0, m_TitleHeight, m_TitleHeight);
    if (closeHovered) {
        p.fillRect(closeRect, MenuDanger);
    }

    p.setFont(m_TitleFont);
    p.setPen(backHovered ? MenuAccent : MenuDim);
    const int backWidth = m_CurrentLevel > 0 ? 28 : 0;
    if (backWidth) {
        drawIcon(QStringLiteral("tb-back"), QRect(textPad, (titleH - 20) / 2, 20, 20));
    }
    QRect titleRect(textPad + backWidth, 0, cw - textPad - backWidth - m_TitleHeight, titleH);
    p.drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter, level.title.toUpper());
    drawIcon(QStringLiteral("menu-close"),
             QRect(closeRect.center().x() - 9, closeRect.center().y() - 9, 18, 18));

    // Apply content offset for level navigation animation
    if (m_ContentSlideAnim->state() != QAbstractAnimation::Running) {
        m_ContentOffset = 0;
    }
    p.save();
    if (m_ContentOffset != 0) {
        p.translate(m_ContentOffset, 0);
    }

    const auto& items = level.items;
    int contentTop = titleH + m_Padding;

    auto iconForItem = [](const MenuItem& item) -> QString {
        if (item.type == MenuItemType::SubMenu) {
            switch (item.targetLevel) {
            case 1: return QStringLiteral("tb-settings");
            case 2: return QStringLiteral("menu-bitrate");
            case 3: return QStringLiteral("menu-position");
            case 4: return QStringLiteral("cat-peripherals");
            }
        }
        switch (item.action) {
        case MenuAction::ToggleFullScreen: return QStringLiteral("cat-display");
        case MenuAction::ShowHostFiles: return QStringLiteral("menu-files");
        case MenuAction::ToggleMicrophone: return QStringLiteral("menu-microphone");
        case MenuAction::ToggleGamepadMouse: return QStringLiteral("cat-gamepad");
        case MenuAction::Quit:
        case MenuAction::QuitAndExit: return QStringLiteral("menu-close");
#ifdef MOONLIGHT_ENABLE_FUNCTION_TESTS
        case MenuAction::OpenStylusReplayPanel: return QStringLiteral("tb-settings");
#endif
        default: return {};
        }
    };

    // Icon column: only on top-level menu
    bool hasIcons = (m_CurrentLevel == 0);
    int iconW = hasIcons ? m_IconAreaWidth : 0;
    int labelX = textPad + iconW;

    for (int i = 0; i < (int)items.size(); i++) {
        int itemY = contentTop + i * m_ItemHeight;
        const auto& item = items[i];

        // Square focus/hover outline and the shared 4px accent marker.
        if (i == m_HoveredIndex && item.enabled) {
            const QRect row(4, itemY + 1, cw - 8, m_ItemHeight - 2);
            p.fillRect(row, MenuHover);
            p.setPen(QPen(MenuAccent, 1));
            p.drawRect(row.adjusted(0, 0, -1, -1));
            p.fillRect(QRect(4, itemY + 1, 4, m_ItemHeight - 2), MenuAccent);
        }

        if (hasIcons) {
            drawIcon(iconForItem(item),
                     QRect(textPad, itemY + (m_ItemHeight - 20) / 2, 20, 20),
                     item.enabled ? (i == m_HoveredIndex ? 1.0 : 0.85) : 0.4);
        }

        // --- SubMenu item ---
        if (item.type == MenuItemType::SubMenu) {
            p.setFont(m_LabelFont);
            p.setPen(item.enabled ? MenuText : MenuFaint);
            QRect lr(labelX, itemY, cw - labelX - 36, m_ItemHeight);
            p.drawText(lr, Qt::AlignLeft | Qt::AlignVCenter, item.label);

            // Detail text (e.g., "20 Mbps")
            if (!item.detail.isEmpty()) {
                p.setFont(m_DetailFont);
                p.setPen(MenuDim);
                QRect dr(cw / 2, itemY, cw / 2 - textPad - 20, m_ItemHeight);
                p.drawText(dr, Qt::AlignRight | Qt::AlignVCenter, item.detail);
            }

            drawIcon(QStringLiteral("menu-next"),
                     QRect(cw - textPad - 16, itemY + (m_ItemHeight - 16) / 2, 16, 16), 0.85);
        }
        // --- Toggle item ---
        else if (item.type == MenuItemType::Toggle) {
            p.setFont(m_LabelFont);
            p.setPen(item.enabled ? MenuText : MenuFaint);
            QRect lr(labelX, itemY, cw - labelX - 52, m_ItemHeight);
            p.drawText(lr, Qt::AlignLeft | Qt::AlignVCenter, item.label);

            // Square track and square thumb, matching HardSwitch.qml.
            const int trackW = 40, trackH = 20;
            const int trackX = cw - textPad - trackW;
            const int trackY = itemY + (m_ItemHeight - trackH) / 2;
            const QRect track(trackX, trackY, trackW, trackH);
            p.fillRect(track, item.toggleState ? MenuAccent : MenuHover);
            p.setPen(QPen(item.toggleState ? MenuAccent : MenuLine, 1));
            p.drawRect(track.adjusted(0, 0, -1, -1));
            p.fillRect(QRect(trackX + (item.toggleState ? trackW - 16 : 4),
                             trackY + 4, 12, 12), item.toggleState ? MenuSurface : MenuDim);
        }
        // --- Slider item (bitrate scrubber) ---
        else if (item.type == MenuItemType::Slider) {
            const SliderRowRects r = sliderRowRects(cw, itemY);
            const bool sliderFocused = i == m_HoveredIndex && item.enabled;

            // Current value in the shared brand accent.
            p.setFont(m_LabelFont);
            p.setPen(sliderFocused ? MenuAccent : MenuText);
            p.drawText(r.value, Qt::AlignLeft | Qt::AlignVCenter,
                       formatBitrateKbps(m_BitrateKbps));

            // Log-scale track: hover base, accent fill up to the square thumb.
            p.fillRect(r.track, MenuHover);
            p.setPen(QPen(MenuLine, 1));
            p.drawRect(r.track);
            const double frac = bitrateFraction();
            const int fillW = qRound(r.track.width() * frac);
            if (fillW > 0) {
                p.fillRect(QRect(r.track.x(), r.track.y(), fillW, r.track.height()),
                           MenuAccent);
            }
            const int thumbW = 10, thumbH = 16;
            const int thumbX = qBound(r.track.x() - thumbW / 2,
                                      r.track.x() + fillW - thumbW / 2,
                                      r.track.x() + r.track.width() - thumbW / 2);
            const QRect thumb(thumbX, itemY + (m_ItemHeight - thumbH) / 2, thumbW, thumbH);
            p.fillRect(thumb, m_SliderDragging ? MenuAccent : MenuText);
            p.setPen(QPen(MenuLine, 1));
            p.drawRect(thumb);

            // −/+ steppers with hover/pressed feedback
            const QRect zones[] = { r.minus, r.plus };
            const SliderZone zoneIds[] = { SliderZone::Minus, SliderZone::Plus };
            const QString glyphs[] = { QStringLiteral("-"), QStringLiteral("+") };
            for (int z = 0; z < 2; z++) {
                const bool hot = sliderFocused && m_SliderHotZone == zoneIds[z];
                const bool pressed = m_SliderPressedZone == zoneIds[z];
                if (hot || pressed) {
                    p.fillRect(zones[z], pressed ? MenuAccent : MenuHover);
                }
                p.setPen(QPen(pressed ? MenuAccent : MenuLine, 1));
                p.drawRect(zones[z]);
                p.setFont(m_LabelFont);
                p.setPen(hot || pressed ? MenuText : MenuDim);
                p.drawText(zones[z], Qt::AlignCenter, glyphs[z]);
            }
        }
        // --- Action item ---
        else if (item.type == MenuItemType::Action) {
            p.setFont(m_LabelFont);
            p.setPen(item.enabled ? MenuText : MenuFaint);

            bool hasLongDetail = !item.detail.isEmpty() && item.detail.length() > 3;
            bool hasShortDetail = !item.detail.isEmpty() && item.detail.length() <= 3;

            if (hasLongDetail) {
                int topH = qRound(m_ItemHeight * 0.58);
                QRect lb(labelX, itemY, cw - labelX - textPad, topH);
                p.drawText(lb, Qt::AlignLeft | Qt::AlignBottom, item.label);

                p.setFont(m_DetailFont);
                p.setPen(MenuDim);
                QRect sr(labelX, itemY + topH, cw - labelX - textPad, m_ItemHeight - topH);
                p.drawText(sr, Qt::AlignLeft | Qt::AlignTop, item.detail);
            } else {
                int detailWidth = 0;
                if (hasShortDetail) {
                    const QFontMetrics detailMetrics(m_DetailFont);
                    detailWidth = qMax(20, detailMetrics.horizontalAdvance(item.detail) + 8);
                }

                const int detailGap = hasShortDetail ? 8 : 0;
                QRect lr(labelX, itemY,
                         cw - labelX - textPad - detailWidth - detailGap,
                         m_ItemHeight);
                p.drawText(lr, Qt::AlignLeft | Qt::AlignVCenter, item.label);

                if (hasShortDetail) {
                    // Short status text or checkmark in the shared brand accent.
                    p.setFont(m_DetailFont);
                    p.setPen(MenuAccent);
                    QRect cr(cw - textPad - detailWidth, itemY,
                             detailWidth, m_ItemHeight);
                    p.drawText(cr, Qt::AlignRight | Qt::AlignVCenter, item.detail);
                }
            }
        }
        // --- Back item (fallback, normally handled by title bar) ---
        else if (item.type == MenuItemType::Back) {
            p.setFont(m_DetailFont);
            p.setPen(MenuDim);
            QRect lr(labelX, itemY, cw - labelX - textPad, m_ItemHeight);
            p.drawText(lr, Qt::AlignLeft | Qt::AlignVCenter, item.label);
        }

        // Group separator — only where explicitly flagged
        if (item.separatorAfter && i < (int)items.size() - 1) {
            p.setPen(QPen(MenuLine, 1));
            int sepY = itemY + m_ItemHeight - 1;
            p.drawLine(labelX, sepY, cw - textPad, sepY);
        }
    }

    p.restore();  // content offset
    p.restore();  // shadow margin translate
}

// ---------------------------------------------------------------------------
// Mouse input
// ---------------------------------------------------------------------------

void OverlayMenuPanel::mouseMoveEvent(QMouseEvent* event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const QPoint pos = event->position().toPoint();
#else
    const QPoint pos = event->pos();
#endif

    if (m_SliderDragging) {
        // Keep scrubbing even when the pointer leaves the panel (mouse grab).
        const auto& items = m_MenuLevels[m_CurrentLevel].items;
        for (int i = 0; i < (int)items.size(); i++) {
            if (items[i].type != MenuItemType::Slider) continue;
            const int itemY = m_TitleHeight + m_Padding + i * m_ItemHeight;
            const SliderRowRects r = sliderRowRects(width() - 2 * m_ShadowMargin, itemY);
            const double frac = (pos.x() - r.track.x()) / double(r.track.width());
            setBitrateFromFraction(frac);
            break;
        }
        return;
    }

    const int newIdx = itemAtPos(pos);
    const SliderZone hotZone = sliderZoneAt(pos, newIdx);
    if (newIdx != m_HoveredIndex || hotZone != m_SliderHotZone) {
        m_HoveredIndex = newIdx;
        m_SliderHotZone = hotZone;
        setCursor((m_HoveredIndex >= 0 || m_HoveredIndex == -2 || m_HoveredIndex == -3)
                          ? Qt::PointingHandCursor
                          : Qt::ArrowCursor);
        forceRepaint();
    }
}

void OverlayMenuPanel::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) return;
    beginInteraction();

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const QPoint pos = event->position().toPoint();
#else
    const QPoint pos = event->pos();
#endif
    int idx = itemAtPos(pos);

    if (idx == -3) {
        closeMenu();
        return;
    }

    // Title bar click → navigate back
    if (idx == -2) {
        navigateToLevel(0);
        return;
    }

    if (idx < 0) return;

    const auto& items = m_MenuLevels[m_CurrentLevel].items;
    if (idx >= (int)items.size() || !items[idx].enabled) return;

    const auto& item = items[idx];

    switch (item.type) {
    case MenuItemType::Back:
        navigateToLevel(0);
        break;

    case MenuItemType::SubMenu:
        navigateToLevel(item.targetLevel);
        break;

    case MenuItemType::Action:
        if (item.action == MenuAction::SetBitrate) {
            selectBitratePreset(item);
            break;
        }
        dispatchActionItem(item);
        break;

    case MenuItemType::Slider:
    {
        const SliderZone zone = sliderZoneAt(pos, idx);
        m_SliderPressedZone = zone;
        m_SliderHotZone = zone;
        if (zone == SliderZone::Minus) {
            adjustBitrateStep(-1, 1);
        }
        else if (zone == SliderZone::Plus) {
            adjustBitrateStep(1, 1);
        }
        else if (zone == SliderZone::Track) {
            const int itemY = m_TitleHeight + m_Padding + idx * m_ItemHeight;
            const SliderRowRects r = sliderRowRects(width() - 2 * m_ShadowMargin, itemY);
            m_SliderDragging = true;
            setMouseGrabEnabled(true);
            const double frac = (pos.x() - r.track.x()) / double(r.track.width());
            setBitrateFromFraction(frac);
        }
        forceRepaint();
        break;
    }

    case MenuItemType::Toggle:
    {
        // Toggle visual state and dispatch
        auto& mutableItem = m_MenuLevels[m_CurrentLevel].items[idx];
        mutableItem.toggleState = !mutableItem.toggleState;
        forceRepaint();
        if (m_ActionCallback) {
            m_ActionCallback(item.action);
        }
        break;
    }
    }
}

void OverlayMenuPanel::mouseReleaseEvent(QMouseEvent* event)
{
    Q_UNUSED(event);
    if (!m_SliderDragging && m_SliderPressedZone == SliderZone::None) return;

    if (m_SliderDragging) {
        m_SliderDragging = false;
        setMouseGrabEnabled(false);
    }
    m_SliderPressedZone = SliderZone::None;
    m_SliderHotZone = SliderZone::None;
    // The commit timer keeps running; the value lands once the pointer idles.
    forceRepaint();
}

void OverlayMenuPanel::wheelEvent(QWheelEvent* event)
{
    if (!m_Visible) {
        event->ignore();
        return;
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const QPoint pos = event->position().toPoint();
#else
    const QPoint pos = event->pos();
#endif
    const int idx = itemAtPos(pos);
    if (idx < 0 || idx >= (int)m_MenuLevels[m_CurrentLevel].items.size()
            || m_MenuLevels[m_CurrentLevel].items[idx].type != MenuItemType::Slider) {
        event->ignore();
        return;
    }

    // Wheeling the scrubber counts as interaction; don't race the leave timer.
    beginInteraction();

    // Accumulate high-resolution wheel deltas; one notch = one fine step.
    m_WheelAccum += event->angleDelta().y() / 120.0;
    int notches = 0;
    while (m_WheelAccum >= 1.0) { notches++; m_WheelAccum -= 1.0; }
    while (m_WheelAccum <= -1.0) { notches--; m_WheelAccum += 1.0; }
    if (notches != 0) {
        adjustBitrateStep(notches > 0 ? 1 : -1, qAbs(notches));
    }
    event->accept();
}

// ---------------------------------------------------------------------------
// Gamepad navigation
// ---------------------------------------------------------------------------

void OverlayMenuPanel::gamepadMoveUp()
{
    if (!m_Visible) return;
    beginInteraction();
    const auto& items = m_MenuLevels[m_CurrentLevel].items;
    if (items.empty()) return;

    if (m_HoveredIndex <= 0) {
        // Wrap to last item, or move to title bar if on sub-level
        if (m_CurrentLevel > 0 && m_HoveredIndex == 0) {
            m_HoveredIndex = -2; // title bar (back button)
        } else {
            m_HoveredIndex = (int)items.size() - 1;
        }
    } else {
        m_HoveredIndex--;
    }
    // Skip disabled items
    if (m_HoveredIndex >= 0 && !items[m_HoveredIndex].enabled) {
        gamepadMoveUp();
        return;
    }
    forceRepaint();
}

void OverlayMenuPanel::gamepadMoveDown()
{
    if (!m_Visible) return;
    beginInteraction();
    const auto& items = m_MenuLevels[m_CurrentLevel].items;
    if (items.empty()) return;

    if (m_HoveredIndex == -2) {
        // From title bar, move to first item
        m_HoveredIndex = 0;
    } else if (m_HoveredIndex < 0 || m_HoveredIndex >= (int)items.size() - 1) {
        // Wrap to title bar on sub-level, or to first item on top level
        if (m_CurrentLevel > 0) {
            m_HoveredIndex = -2;
        } else {
            m_HoveredIndex = 0;
        }
    } else {
        m_HoveredIndex++;
    }
    // Skip disabled items
    if (m_HoveredIndex >= 0 && !items[m_HoveredIndex].enabled) {
        gamepadMoveDown();
        return;
    }
    forceRepaint();
}

void OverlayMenuPanel::gamepadSelect()
{
    if (!m_Visible) return;

    // Title bar → back
    if (m_HoveredIndex == -2) {
        navigateToLevel(0);
        return;
    }

    if (m_HoveredIndex < 0) return;

    const auto& items = m_MenuLevels[m_CurrentLevel].items;
    if (m_HoveredIndex >= (int)items.size() || !items[m_HoveredIndex].enabled) return;

    const auto& item = items[m_HoveredIndex];

    switch (item.type) {
    case MenuItemType::Back:
        navigateToLevel(0);
        break;
    case MenuItemType::SubMenu:
        navigateToLevel(item.targetLevel);
        break;
    case MenuItemType::Action:
        if (item.action == MenuAction::SetBitrate) {
            selectBitratePreset(item);
            break;
        }
        dispatchActionItem(item);
        break;
    case MenuItemType::Slider:
        // A confirms — flush a pending scrub immediately.
        commitBitrateNow();
        break;
    case MenuItemType::Toggle:
    {
        auto& mutableItem = m_MenuLevels[m_CurrentLevel].items[m_HoveredIndex];
        mutableItem.toggleState = !mutableItem.toggleState;
        forceRepaint();
        if (m_ActionCallback) {
            m_ActionCallback(item.action);
        }
        break;
    }
    }
}

void OverlayMenuPanel::gamepadBack()
{
    if (!m_Visible) return;

    if (m_CurrentLevel > 0) {
        navigateToLevel(0);
    } else {
        closeMenu();
    }
}

bool OverlayMenuPanel::event(QEvent* ev)
{
    if (ev->type() == QEvent::Leave) {
        if (m_Visible && m_CloseWhenPointerOutside) {
            // During the grace period, defer the outside check instead of
            // dropping the Leave event. Otherwise, leaving the panel quickly
            // after it opens would keep it visible until the cursor entered
            // and left the panel again.
            schedulePointerOutsideCheck();
        }
        return true;
    }
    return QRasterWindow::event(ev);
}
