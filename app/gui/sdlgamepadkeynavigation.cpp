#include "sdlgamepadkeynavigation.h"

#include <QKeyEvent>
#include <QGuiApplication>
#include <QWindow>

#include "settings/mappingmanager.h"

// 摇杆导航：越过激活阈值才响应，回落到更低阈值才算松开（滞回防抖）
#define AXIS_NAV_ACTIVATE_THRESHOLD 20000
#define AXIS_NAV_RELEASE_THRESHOLD 16000

// 按 AxisNavDir 方向值索引（None 占位不用），恢复轮询时读当前按住状态用
static const SDL_GameControllerButton k_DpadNavButtons[] = {
    SDL_CONTROLLER_BUTTON_INVALID,    SDL_CONTROLLER_BUTTON_DPAD_UP,
    SDL_CONTROLLER_BUTTON_DPAD_DOWN,  SDL_CONTROLLER_BUTTON_DPAD_LEFT,
    SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
};

SdlGamepadKeyNavigation::SdlGamepadKeyNavigation(StreamingPreferences* prefs)
    : m_Prefs(prefs), m_Enabled(false), m_UiNavMode(false), m_UiNavSuspendCount(0),
      m_FirstPoll(false), m_HasFocus(false)
{
    m_PollingTimer = new QTimer(this);
    connect(m_PollingTimer, &QTimer::timeout, this, &SdlGamepadKeyNavigation::onPollingTimerFired);
}

SdlGamepadKeyNavigation::~SdlGamepadKeyNavigation()
{
    disable();
}

void SdlGamepadKeyNavigation::enable()
{
    if (m_Enabled) {
        return;
    }

    // We have to initialize and uninitialize this in enable()/disable()
    // because we need to get out of the way of the Session class. If it
    // doesn't get to reinitialize the GC subsystem, it won't get initial
    // arrival events. Additionally, there's a race condition between
    // our QML objects being destroyed and SDL being deinitialized that
    // this solves too.
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) failed: %s",
                     SDL_GetError());
        return;
    }

    MappingManager mappingManager;
    mappingManager.applyMappings();

    // Drop all pending gamepad add events. SDL will generate these for us
    // on first init of the GC subsystem. We can't depend on them due to
    // overlapping lifetimes of SdlGamepadKeyNavigation instances, so we
    // will attach ourselves.
    //
    // NB: We use SDL_JoystickUpdate() instead of SDL_PumpEvents() because
    // the latter can do a bit more work that we want (like handling video
    // events that we intentionally do not want to process yet).
    SDL_JoystickUpdate();
    SDL_FlushEvent(SDL_CONTROLLERDEVICEADDED);

    // Open all currently attached game controllers
    int numJoysticks = SDL_NumJoysticks();
    for (int i = 0; i < numJoysticks; i++) {
        if (SDL_IsGameController(i)) {
            SDL_GameController* gc = SDL_GameControllerOpen(i);
            if (gc != nullptr) {
                m_Gamepads.append(gc);
            }
        }
    }

    m_Enabled = true;

    // Start the polling timer if the window is focused
    updateTimerState();
}

void SdlGamepadKeyNavigation::disable()
{
    if (!m_Enabled) {
        return;
    }

    m_Enabled = false;
    resetNavRepeatState();
    updateTimerState();
    Q_ASSERT(!m_PollingTimer->isActive());

    while (!m_Gamepads.isEmpty()) {
        SDL_GameControllerClose(m_Gamepads[0]);
        m_Gamepads.removeAt(0);
    }

    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
}

void SdlGamepadKeyNavigation::notifyWindowFocus(bool hasFocus)
{
    m_HasFocus = hasFocus;
    updateTimerState();
}

void SdlGamepadKeyNavigation::onPollingTimerFired()
{
    SDL_Event event;

    // Update joystick state without pumping other events (see enable() comment)
    SDL_JoystickUpdate();

    // Discard any pending button events on the first poll to avoid picking up
    // stale input data from the stream session (like the quit combo).
    if (m_FirstPoll) {
        SDL_FlushEvent(SDL_CONTROLLERBUTTONDOWN);
        SDL_FlushEvent(SDL_CONTROLLERBUTTONUP);
        // 暂停轮询期间手柄可能一直被按住：挂起期间的 DOWN 已被 flush，
        // 不会再有按下事件，直接读当前状态恢复连发（按住时长从恢复点重算）
        resetNavRepeatState();
        Uint32 resumeTime = SDL_GetTicks();
        for (auto gc : std::as_const(m_Gamepads)) {
            for (int dir = AxisNavUp; dir <= AxisNavRight; dir++) {
                if (!m_DpadNav[dir].held &&
                    SDL_GameControllerGetButton(gc, k_DpadNavButtons[dir])) {
                    m_DpadNav[dir].held = true;
                    m_DpadNav[dir].downSince = resumeTime;
                    m_DpadNav[dir].lastFire = resumeTime;
                    // 补发按下：焦点窗口在挂起期间可能已切换，且后面的
                    // 物理释放需要一次配对的 KeyPress
                    sendDirectionKey(QEvent::Type::KeyPress, dir);
                }
            }
        }
        m_FirstPoll = false;
    }

    // Peep events rather than polling to avoid calling SDL_PumpEvents()
    while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT) == 1) {
        switch (event.type) {
        case SDL_QUIT:
            // SDL may send us a quit event since we initialize
            // the video subsystem on startup. If we get one,
            // forward it on for Qt to take care of.
            QCoreApplication::instance()->quit();
            break;
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP:
        {
            QEvent::Type type =
                    event.type == SDL_CONTROLLERBUTTONDOWN ?
                        QEvent::Type::KeyPress : QEvent::Type::KeyRelease;

            // Swap face buttons if needed
            if (m_Prefs->swapFaceButtons) {
                switch (event.cbutton.button) {
                case SDL_CONTROLLER_BUTTON_A:
                    event.cbutton.button = SDL_CONTROLLER_BUTTON_B;
                    break;
                case SDL_CONTROLLER_BUTTON_B:
                    event.cbutton.button = SDL_CONTROLLER_BUTTON_A;
                    break;
                case SDL_CONTROLLER_BUTTON_X:
                    event.cbutton.button = SDL_CONTROLLER_BUTTON_Y;
                    break;
                case SDL_CONTROLLER_BUTTON_Y:
                    event.cbutton.button = SDL_CONTROLLER_BUTTON_X;
                    break;
                }
            }

            switch (event.cbutton.button) {
            case SDL_CONTROLLER_BUTTON_DPAD_UP:
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: {
                int dir;
                switch (event.cbutton.button) {
                case SDL_CONTROLLER_BUTTON_DPAD_UP:
                    dir = AxisNavUp;
                    break;
                case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
                    dir = AxisNavDown;
                    break;
                case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
                    dir = AxisNavLeft;
                    break;
                default:
                    dir = AxisNavRight;
                    break;
                }

                // 记录按住状态，供轮询做连发；首击由这里即时发出
                if (type == QEvent::Type::KeyPress) {
                    m_DpadNav[dir].held = true;
                    m_DpadNav[dir].downSince = SDL_GetTicks();
                    m_DpadNav[dir].lastFire = SDL_GetTicks();
                } else {
                    m_DpadNav[dir].held = false;
                }

                sendDirectionKey(type, dir);
                break;
            }
            case SDL_CONTROLLER_BUTTON_A:
                if (uiNavModeActive()) {
                    sendKey(type, Qt::Key_Space);
                }
                else {
                    sendKey(type, Qt::Key_Return);
                }
                break;
            case SDL_CONTROLLER_BUTTON_B:
                sendKey(type, Qt::Key_Escape);
                break;
            case SDL_CONTROLLER_BUTTON_X:
                sendKey(type, Qt::Key_Menu);
                break;
            case SDL_CONTROLLER_BUTTON_Y:
            case SDL_CONTROLLER_BUTTON_START:
                // HACK: We use this keycode to inform main.qml
                // to show the settings when Key_Menu is handled
                // by the control in focus.
                sendKey(type, Qt::Key_Hangup);
                break;
            case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
                if (uiNavModeActive()) {
                    // Used by SettingsView to switch to the previous category
                    sendKey(type, Qt::Key_PageUp);
                }
                break;
            case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
                if (uiNavModeActive()) {
                    // Used by SettingsView to switch to the next category
                    sendKey(type, Qt::Key_PageDown);
                }
                break;
            default:
                break;
            }
            break;
        }
        case SDL_CONTROLLERDEVICEADDED:
            SDL_GameController* gc = SDL_GameControllerOpen(event.cdevice.which);
            if (gc != nullptr) {
                // SDL_CONTROLLERDEVICEADDED can be reported multiple times for the same
                // gamepad in rare cases, because SDL doesn't fixup the device index in
                // the SDL_CONTROLLERDEVICEADDED event if an unopened gamepad disappears
                // before we've processed the add event.
                if (!m_Gamepads.contains(gc)) {
                    m_Gamepads.append(gc);
                }
                else {
                    // We already have this game controller open
                    SDL_GameControllerClose(gc);
                }
            }
            break;
        }
    }

    // Handle analog sticks by polling, with threshold hysteresis and
    // repeat acceleration while a direction is held
    int stickDir = AxisNavNone;
    for (auto gc : std::as_const(m_Gamepads)) {
        short leftX = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX);
        short leftY = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY);

        if (m_AxisNavDir != AxisNavNone) {
            // The held direction owns the stick until it drops to the lower
            // release threshold. Only after it releases can a new direction
            // activate, and then only past the activation threshold - an
            // orthogonal axis between the two thresholds must not steal
            // navigation mid-hold.
            bool stillHeld;
            switch (m_AxisNavDir) {
            case AxisNavUp:
                stillHeld = leftY < -AXIS_NAV_RELEASE_THRESHOLD;
                break;
            case AxisNavDown:
                stillHeld = leftY > AXIS_NAV_RELEASE_THRESHOLD;
                break;
            case AxisNavLeft:
                stillHeld = leftX < -AXIS_NAV_RELEASE_THRESHOLD;
                break;
            default:
                stillHeld = leftX > AXIS_NAV_RELEASE_THRESHOLD;
                break;
            }

            if (stillHeld) {
                stickDir = m_AxisNavDir;
                break;
            }
        }

        if (leftY < -AXIS_NAV_ACTIVATE_THRESHOLD) {
            stickDir = AxisNavUp;
        } else if (leftY > AXIS_NAV_ACTIVATE_THRESHOLD) {
            stickDir = AxisNavDown;
        } else if (leftX < -AXIS_NAV_ACTIVATE_THRESHOLD) {
            stickDir = AxisNavLeft;
        } else if (leftX > AXIS_NAV_ACTIVATE_THRESHOLD) {
            stickDir = AxisNavRight;
        }

        if (stickDir != AxisNavNone) {
            break;
        }
    }

    Uint32 now = SDL_GetTicks();
    if (stickDir != m_AxisNavDir) {
        m_AxisNavDir = stickDir;
        m_AxisNavDirSince = now;
        if (stickDir != AxisNavNone) {
            // Fire immediately on each new direction
            sendDirectionKey(QEvent::Type::KeyPress, stickDir);
            sendDirectionKey(QEvent::Type::KeyRelease, stickDir);
            m_AxisNavLastFire = now;
        }
    } else if (stickDir != AxisNavNone &&
               now - m_AxisNavLastFire >= axisNavRepeatDelayMs(now - m_AxisNavDirSince)) {
        sendDirectionKey(QEvent::Type::KeyPress, stickDir);
        sendDirectionKey(QEvent::Type::KeyRelease, stickDir);
        m_AxisNavLastFire = now;
    }

    // Held D-pad buttons repeat at the same cadence as the stick
    for (int dir = AxisNavUp; dir <= AxisNavRight; dir++) {
        if (!m_DpadNav[dir].held) {
            continue;
        }

        Uint32 heldMs = now - m_DpadNav[dir].downSince;
        if (now - m_DpadNav[dir].lastFire >= axisNavRepeatDelayMs(heldMs)) {
            sendDirectionKey(QEvent::Type::KeyPress, dir);
            sendDirectionKey(QEvent::Type::KeyRelease, dir);
            m_DpadNav[dir].lastFire = now;
        }
    }
}

void SdlGamepadKeyNavigation::sendKey(QEvent::Type type, Qt::Key key, Qt::KeyboardModifiers modifiers)
{
    QGuiApplication* app = static_cast<QGuiApplication*>(QGuiApplication::instance());
    QWindow* focusWindow = app->focusWindow();
    if (focusWindow != nullptr) {
        QKeyEvent keyPressEvent(type, key, modifiers);
        app->sendEvent(focusWindow, &keyPressEvent);
    }
}

void SdlGamepadKeyNavigation::sendDirectionKey(QEvent::Type type, int dir)
{
    switch (dir) {
    case AxisNavUp:
        if (uiNavModeActive()) {
            // Back-tab
            sendKey(type, Qt::Key_Tab, Qt::ShiftModifier);
        } else {
            sendKey(type, Qt::Key_Up);
        }
        break;
    case AxisNavDown:
        if (uiNavModeActive()) {
            sendKey(type, Qt::Key_Tab);
        } else {
            sendKey(type, Qt::Key_Down);
        }
        break;
    case AxisNavLeft:
        sendKey(type, Qt::Key_Left);
        break;
    case AxisNavRight:
        sendKey(type, Qt::Key_Right);
        break;
    }
}

Uint32 SdlGamepadKeyNavigation::axisNavRepeatDelayMs(Uint32 heldMs)
{
    if (heldMs < 400) {
        return 400;
    } else if (heldMs < 1600) {
        return 150;
    } else {
        return 75;
    }
}

void SdlGamepadKeyNavigation::resetNavRepeatState()
{
    m_AxisNavDir = AxisNavNone;
    for (int dir = AxisNavUp; dir <= AxisNavRight; dir++) {
        m_DpadNav[dir].held = false;
    }
}

GamepadUiStyle SdlGamepadKeyNavigation::detectUiStyle() const
{
    // 与 gamepad.cpp 的 LI_CTYPE 判定使用同一套 SDL 类型与版本守卫
    for (auto gc : std::as_const(m_Gamepads)) {
        switch (SDL_GameControllerGetType(gc)) {
        case SDL_CONTROLLER_TYPE_PS3:
        case SDL_CONTROLLER_TYPE_PS4:
        case SDL_CONTROLLER_TYPE_PS5:
            return GamepadUiStylePlayStation;
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:
#if SDL_VERSION_ATLEAST(2, 24, 0)
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
#endif
            return GamepadUiStyleNintendo;
        default:
            break;
        }
    }
    return GamepadUiStyleXbox;
}

int SdlGamepadKeyNavigation::gamepadUiStyle()
{
    return detectUiStyle();
}

bool SdlGamepadKeyNavigation::gamepadQuitComboEnabled() const
{
    // 与 gamepad.cpp 的 m_GamepadQuitEnabled 同一开关
    return qgetenv("NO_GAMEPAD_QUIT") != "1";
}

QString SdlGamepadKeyNavigation::faceButtonGlyph(int logicalButton)
{
    return gamepadFaceButtonGlyph(detectUiStyle(), logicalButton);
}

QString SdlGamepadKeyNavigation::startButtonName()
{
    return gamepadStartButtonName(detectUiStyle());
}

QString SdlGamepadKeyNavigation::selectButtonName()
{
    return gamepadSelectButtonName(detectUiStyle());
}

QString SdlGamepadKeyNavigation::leftShoulderName()
{
    return gamepadLeftShoulderName(detectUiStyle());
}

QString SdlGamepadKeyNavigation::rightShoulderName()
{
    return gamepadRightShoulderName(detectUiStyle());
}

void SdlGamepadKeyNavigation::updateTimerState()
{
    if (m_PollingTimer->isActive() && (!m_HasFocus || !m_Enabled)) {
        m_PollingTimer->stop();
    }
    else if (!m_PollingTimer->isActive() && m_HasFocus && m_Enabled) {
        // Flush events on the first poll
        m_FirstPoll = true;

        // Poll every 50 ms for a new joystick event
        m_PollingTimer->start(50);
    }
}

void SdlGamepadKeyNavigation::setUiNavMode(bool uiNavMode)
{
    m_UiNavMode = uiNavMode;
}

void SdlGamepadKeyNavigation::suspendUiNavMode()
{
    m_UiNavSuspendCount++;
}

void SdlGamepadKeyNavigation::resumeUiNavMode()
{
    // 夹住下界：QML 侧的 Popup 在某些时序下会重复发 aboutToHide，
    // 计数掉到负数之后就再也回不到挂起状态了。
    if (m_UiNavSuspendCount > 0) {
        m_UiNavSuspendCount--;
    }
}

bool SdlGamepadKeyNavigation::uiNavModeActive() const
{
    return m_UiNavMode && m_UiNavSuspendCount == 0;
}

int SdlGamepadKeyNavigation::getConnectedGamepads()
{
    Q_ASSERT(m_Enabled);

    int count = 0;
    int numJoysticks = SDL_NumJoysticks();
    for (int i = 0; i < numJoysticks; i++) {
        if (SDL_IsGameController(i)) {
            count++;
        }
    }

    return count;
}
