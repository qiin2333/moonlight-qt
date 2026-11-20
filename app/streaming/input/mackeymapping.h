#pragma once

#include <Limelight.h>
#include <QHash>
#include <QSet>
#include <cstdint>

namespace MacKeyMapping {
struct Key
{
    short code;
    bool extended;
};

inline Key modifier(char target, bool right)
{
    const bool win = target == 'W';
    return { static_cast<short>((win ? 0x5B : target == 'C' ? 0xA2 : 0xA4) + right), win || right };
}

inline Key deleteKey(char target)
{
    const bool forward = target == 'D';
    return { static_cast<short>(forward ? 0x2E : 0x08), forward };
}

inline char modifierFlag(char target)
{
    return target == 'W' ? MODIFIER_META : target == 'C' ? MODIFIER_CTRL : MODIFIER_ALT;
}

inline bool ignoreCommand(bool mapsToWin, bool captureActive, bool pressed, bool wasForwarded)
{
    return mapsToWin && !captureActive && (pressed || !wasForwarded);
}

inline void recordSend(QHash<int, uint32_t>& physicalKeys, QSet<uint32_t>& localKeys, int scancode,
                       uint32_t state, bool pressed, int result)
{
    if (pressed) {
        if (result == 0) {
            physicalKeys.insert(scancode, state);
        }
    } else if (result == 0) {
        physicalKeys.remove(scancode);
    } else if (physicalKeys.contains(scancode)) {
        // The physical key is already up, but a failed release still needs retrying.
        localKeys.insert(state);
    }
}

inline void reconcileReleased(QHash<int, uint32_t>& physicalKeys, const QSet<uint32_t>& localKeys)
{
    for (auto it = physicalKeys.begin(); it != physicalKeys.end();) {
        if (!localKeys.contains(it.value())) {
            it = physicalKeys.erase(it);
        } else {
            ++it;
        }
    }
}
}
