#pragma once

#include <Limelight.h>
#include <QSet>
#include <cstdint>

namespace KeyboardState {
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
