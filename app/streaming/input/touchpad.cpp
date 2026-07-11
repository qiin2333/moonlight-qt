#include "input.h"

#include <Limelight.h>
#include "streaming/session.h"

void SdlInputHandler::handleNativeTouchpadEvent(SDL_TouchFingerEvent* event)
{
    if (!isCaptureActive()) {
        // Not capturing
        return;
    }

    if (m_NativeTouchpadTransport == NTT_UNKNOWN) {
        uint32_t hostFeatures = LiGetHostFeatureFlags();
        if (hostFeatures & (LI_FF_TOUCHPAD_FRAME_EVENTS | LI_FF_TOUCHPAD_EVENTS)) {
            m_NativeTouchpadTransport = NTT_FRAME;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Native touchpad (id: %llu) transport selected: frame (hostFeatures=0x%08x)",
                        event->touchId, hostFeatures);
        }
        else if (hostFeatures & LI_FF_TOUCHPAD_EVENTS) {
            m_NativeTouchpadTransport = NTT_INDIVIDUAL;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Native touchpad (id: %llu) transport selected: individual (hostFeatures=0x%08x)",
                        event->touchId, hostFeatures);
        }
        else {
            m_NativeTouchpadTransport = NTT_SOFTWARE_POINTER;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Native touchpad (id: %llu) transport selected: software-pointer (hostFeatures=0x%08x)",
                        event->touchId, hostFeatures);
        }
    }

    if (m_NativeTouchpadTransport == NTT_SOFTWARE_POINTER) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Native touchpad input is unavailable; using software pointer fallback");
        handleRelativeFingerEvent(event);
        return;
    }

    uint8_t eventType;
    switch (event->type) {
    case SDL_FINGERDOWN: eventType = LI_TOUCH_EVENT_DOWN; break;
    case SDL_FINGERMOTION: eventType = LI_TOUCH_EVENT_MOVE; break;
    case SDL_FINGERUP: eventType = LI_TOUCH_EVENT_UP; break;
    default: return;
    }

    if ((!m_ActiveTouchpadContacts.isEmpty() || !m_IgnoredTouchpadContacts.isEmpty()) &&
            event->touchId != m_ActiveTouchpadId) {
        // The wire protocol has no device identifier. Finish contacts from the
        // previous device before switching to another physical touchpad.
        cancelNativeTouchpadContacts();
    }
    m_ActiveTouchpadId = event->touchId;

    if (m_IgnoredTouchpadContacts.contains(event->fingerId)) {
        if (eventType == LI_TOUCH_EVENT_UP || eventType == LI_TOUCH_EVENT_CANCEL) {
            m_IgnoredTouchpadContacts.remove(event->fingerId);
            if (m_ActiveTouchpadContacts.isEmpty() && m_IgnoredTouchpadContacts.isEmpty()) {
                m_ActiveTouchpadId = 0;
            }
        }
        return;
    }

    bool contactIsActive = m_ActiveTouchpadContacts.contains(event->fingerId);
    bool hadMultipleContacts = m_ActiveTouchpadContacts.size() >= 2;
    if (eventType == LI_TOUCH_EVENT_DOWN && !contactIsActive &&
            m_ActiveTouchpadContacts.size() >= MAX_TOUCHPAD_FRAME_CONTACTS) {
        m_IgnoredTouchpadContacts.insert(event->fingerId);
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Native touchpad contact limit reached; ignoring fingerId=%lld",
                    static_cast<long long>(event->fingerId));
        return;
    }
    if (eventType != LI_TOUCH_EVENT_DOWN && !contactIsActive) {
        return;
    }

    if (m_PendingTouchpadContactCount > 0 &&
            (event->touchId != m_PendingTouchpadId ||
             event->timestamp != m_PendingTouchpadTimestamp)) {
        sendPendingTouchpadFrame();
    }
    if (m_PendingTouchpadContactCount == MAX_TOUCHPAD_FRAME_CONTACTS) {
        sendPendingTouchpadFrame();
    }
    if (m_PendingTouchpadContactCount == 0) {
        m_PendingTouchpadId = event->touchId;
        m_PendingTouchpadTimestamp = event->timestamp;
    }

    NativeTouchpadContact contact = {
        eventType,
        // The wire protocol uses 32-bit pointer IDs, so keep the low 32 bits of SDL's 64-bit finger ID.
        static_cast<uint32_t>(event->fingerId),
        SDL_clamp(event->x, 0.0f, 1.0f),
        SDL_clamp(event->y, 0.0f, 1.0f),
        SDL_clamp(event->pressure, 0.0f, 1.0f),
    };
    m_PendingTouchpadContacts[m_PendingTouchpadContactCount++] = contact;

    if (eventType == LI_TOUCH_EVENT_UP || eventType == LI_TOUCH_EVENT_CANCEL) {
        m_ActiveTouchpadContacts.remove(event->fingerId);
    }
    else {
        m_ActiveTouchpadContacts.insert(event->fingerId, contact);
    }
    const bool isMultiContactFrame = hadMultipleContacts || m_ActiveTouchpadContacts.size() >= 2;
    if (isMultiContactFrame) {
        m_LastTouchpadScrollTimestamp = event->timestamp;
    }
    if (m_ActiveTouchpadContacts.isEmpty() && m_IgnoredTouchpadContacts.isEmpty()) {
        m_ActiveTouchpadId = 0;
    }

    // A single-contact frame can be sent immediately for the lowest possible
    // pointer latency. SDL reports contacts separately for multi-contact
    // frames, so defer those until the current event batch has been collected.
    if (!isMultiContactFrame) {
        sendPendingTouchpadFrame();
        return;
    }

    // Queue a single event behind the current batch to collect the remaining
    // contacts that belong to this multi-contact frame.
    if (!m_TouchpadFlushEventQueued) {
        if (Session::queueTouchpadFrameFlush()) {
            m_TouchpadFlushEventQueued = true;
        }
        else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Unable to queue touchpad frame flush: %s", SDL_GetError());
            sendPendingTouchpadFrame();
        }
    }
}

void SdlInputHandler::sendNativeTouchpadContacts(const NativeTouchpadContact* contacts,
                                                  int contactCount)
{
    uint8_t eventTypes[MAX_TOUCHPAD_FRAME_CONTACTS];
    uint32_t pointerIds[MAX_TOUCHPAD_FRAME_CONTACTS];
    float x[MAX_TOUCHPAD_FRAME_CONTACTS];
    float y[MAX_TOUCHPAD_FRAME_CONTACTS];
    float pressure[MAX_TOUCHPAD_FRAME_CONTACTS];

    for (int i = 0; i < contactCount; i++) {
        eventTypes[i] = contacts[i].eventType;
        pointerIds[i] = contacts[i].pointerId;
        x[i] = contacts[i].x;
        y[i] = contacts[i].y;
        pressure[i] = contacts[i].pressure;
    }

    if (m_NativeTouchpadTransport == NTT_FRAME) {
        int rc = LiSendTouchpadFrameEvent(static_cast<uint8_t>(contactCount),
                                          eventTypes, pointerIds, x, y, pressure,
                                          LI_ROT_UNKNOWN, 0, 0, 0);
        if (rc == LI_ERR_UNSUPPORTED) {
            if (LiGetHostFeatureFlags() & LI_FF_TOUCHPAD_EVENTS) {
                m_NativeTouchpadTransport = NTT_INDIVIDUAL;
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Native touchpad transport downgraded: frame -> individual");
            }
            else {
                m_NativeTouchpadTransport = NTT_SOFTWARE_POINTER;
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Native touchpad transport downgraded: frame -> software-pointer");
                transitionNativeTouchpadToSoftwarePointer();
                return;
            }
        }
        else {
            if (rc != 0) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "LiSendTouchpadFrameEvent failed: %d", rc);
            }
            return;
        }
    }

    if (m_NativeTouchpadTransport == NTT_INDIVIDUAL) {
        for (int i = 0; i < contactCount; i++) {
            int rc = LiSendTouchpadEvent(eventTypes[i], pointerIds[i], x[i], y[i], pressure[i],
                                         0.0f, 0.0f, LI_ROT_UNKNOWN, 0, 0, 0);
            if (rc == LI_ERR_UNSUPPORTED) {
                m_NativeTouchpadTransport = NTT_SOFTWARE_POINTER;
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Native touchpad transport downgraded: individual -> software-pointer");
                transitionNativeTouchpadToSoftwarePointer();
                return;
            }
            else if (rc != 0) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "LiSendTouchpadEvent failed: %d", rc);
            }
        }
    }
}

void SdlInputHandler::transitionNativeTouchpadToSoftwarePointer()
{
    // Seed the legacy relative-touch path with active contacts because it
    // cannot process Move/Up events until it has seen matching Down events.
    SDL_zero(m_TouchDownEvent);
    int relativeFingerCount = 0;
    for (int i = 0; i < SDL_GetNumTouchFingers(m_ActiveTouchpadId) &&
                    relativeFingerCount < MAX_FINGERS; i++) {
        SDL_Finger* finger = SDL_GetTouchFinger(m_ActiveTouchpadId, i);
        if (finger == nullptr || !m_ActiveTouchpadContacts.contains(finger->id)) {
            continue;
        }

        const NativeTouchpadContact& contact = m_ActiveTouchpadContacts[finger->id];
        SDL_TouchFingerEvent& downEvent = m_TouchDownEvent[relativeFingerCount++];
        downEvent.type = SDL_FINGERDOWN;
        downEvent.touchId = m_ActiveTouchpadId;
        downEvent.fingerId = finger->id;
        downEvent.timestamp = 0;
        downEvent.x = contact.x;
        downEvent.y = contact.y;
        downEvent.pressure = contact.pressure;
    }
    m_NumFingersDown = relativeFingerCount;
}

void SdlInputHandler::sendPendingTouchpadFrame()
{
    if (m_PendingTouchpadContactCount == 0) {
        return;
    }
    sendNativeTouchpadContacts(m_PendingTouchpadContacts, m_PendingTouchpadContactCount);
    m_PendingTouchpadContactCount = 0;
    m_PendingTouchpadId = 0;
    m_PendingTouchpadTimestamp = 0;
}

void SdlInputHandler::flushPendingTouchpadFrameEvent()
{
    m_TouchpadFlushEventQueued = false;
    sendPendingTouchpadFrame();
}

void SdlInputHandler::cancelNativeTouchpadContacts()
{
    sendPendingTouchpadFrame();

    if (!m_ActiveTouchpadContacts.isEmpty()) {
        const NativeTouchpadContact cancelAll = {
            LI_TOUCH_EVENT_CANCEL_ALL, 0, 0.0f, 0.0f, 0.0f,
        };
        sendNativeTouchpadContacts(&cancelAll, 1);
    }

    m_ActiveTouchpadContacts.clear();
    m_IgnoredTouchpadContacts.clear();
    m_ActiveTouchpadId = 0;
}
