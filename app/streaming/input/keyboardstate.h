#pragma once

#include <Limelight.h>
#include <QSet>
#include <cstdint>

namespace KeyboardState {
enum class ShortcutResult
{
    Queued,
    ModifierHeld,
    Failed
};

inline uint32_t pack(short code, char modifiers, char flags)
{
    return static_cast<uint16_t>(code) |
           (static_cast<uint32_t>(static_cast<uint8_t>(modifiers & MODIFIER_EXTENDED)) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(flags)) << 24);
}

inline short code(uint32_t state)
{
    return static_cast<short>(state & 0xFFFF);
}
inline char extendedModifier(uint32_t state)
{
    return static_cast<char>((state >> 16) & 0xFF);
}
inline char flags(uint32_t state)
{
    return static_cast<char>((state >> 24) & 0xFF);
}

// Sunshine detects its shortcuts from the actual modifier transitions. A
// modifier bitmask on the N packet alone does not activate the shortcut.
// Never release a key that was already held by this client, and retain failed
// releases for the existing connection-boundary cleanup.
template <typename Sender>
ShortcutResult sendHostCursorShortcut(QSet<uint32_t>& localKeys, QSet<uint32_t>& forwardedKeys,
                                      bool physicalModifiersDown, Sender send)
{
    if (physicalModifiersDown) {
        return ShortcutResult::ModifierHeld;
    }
    for (uint32_t state : localKeys) {
        const auto key = static_cast<uint16_t>(code(state)) & 0xFF;
        if (key == 0x10 || key == 0x11 || key == 0x12 || key == 0x4E ||
            (key >= 0xA0 && key <= 0xA5)) {
            return ShortcutResult::ModifierHeld;
        }
    }

    // Left Ctrl, left Alt, left Shift, N. Release only successfully queued
    // key-downs, in reverse order, even if a later packet fails to queue.
    constexpr short keys[] = { 0xA2, 0xA4, 0xA0, 0x4E };
    int pressedCount = 0;
    bool failed = false;
    for (short key : keys) {
        if (send(static_cast<short>(0x8000 | key), KEY_ACTION_DOWN) != 0) {
            failed = true;
            break;
        }
        const uint32_t state = pack(static_cast<short>(0x8000 | key), 0, 0);
        localKeys.insert(state);
        forwardedKeys.insert(state);
        pressedCount++;
    }
    while (pressedCount > 0) {
        const short key = keys[--pressedCount];
        if (send(static_cast<short>(0x8000 | key), KEY_ACTION_UP) == 0) {
            localKeys.remove(pack(static_cast<short>(0x8000 | key), 0, 0));
        } else {
            failed = true;
        }
    }
    return failed ? ShortcutResult::Failed : ShortcutResult::Queued;
}

// Keep failed releases for the next attempt. Temporary releases must retain all
// local state so the subsequent physical key-up can still be matched.
template <typename Sender> int raiseKeys(QSet<uint32_t>& keysDown, bool clearKeys, Sender send)
{
    int failedCount = 0;
    const auto snapshot = keysDown;
    for (uint32_t state : snapshot) {
        if (send(state) == 0) {
            if (clearKeys) {
                keysDown.remove(state);
            }
        } else {
            failedCount++;
        }
    }
    return failedCount;
}

// Release local keys and, when requested, keys forwarded during the connection.
// The forwarded set can survive ordinary cleanup so a later connection reset can
// retry a release that was queued before the connection failed.
template <typename Sender>
int raiseKeys(QSet<uint32_t>& localKeys, QSet<uint32_t>& forwardedKeys, bool clearLocalKeys,
              bool includeForwardedKeys, bool clearForwardedKeys, Sender send)
{
    QSet<uint32_t> pendingKeys = localKeys;
    if (includeForwardedKeys) {
        pendingKeys.unite(forwardedKeys);
    }

    const bool clearSentKeys = clearLocalKeys || (includeForwardedKeys && clearForwardedKeys);
    const int failedCount = raiseKeys(pendingKeys, clearSentKeys, send);

    if (clearLocalKeys) {
        localKeys.intersect(pendingKeys);
    }
    if (includeForwardedKeys && clearForwardedKeys) {
        forwardedKeys.intersect(pendingKeys);
    }

    return failedCount;
}
}
