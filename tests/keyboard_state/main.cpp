#include "streaming/input/keyboardstate.h"
#include "streaming/input/mackeymapping.h"
#include <QCoreApplication>
#include <QVector>

namespace {
void require(bool condition, const char* message)
{
    if (!condition) {
        qFatal("%s", message);
    }
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    for (char target : { 'C', 'W', 'A' }) {
        const auto left = MacKeyMapping::modifier(target, false);
        const auto right = MacKeyMapping::modifier(target, true);
        const short expected = target == 'C' ? 0xA2 : target == 'W' ? 0x5B : 0xA4;
        require(left.code == expected && right.code == expected + 1,
                "modifier mapping must preserve left and right sides");
        require(left.extended == (target == 'W') && right.extended,
                "mapped modifiers must use the correct extended flag");
        require(MacKeyMapping::modifierFlag(target) == (target == 'C'   ? MODIFIER_CTRL
                                                        : target == 'W' ? MODIFIER_META
                                                                        : MODIFIER_ALT),
                "modifier flag must match the target key");
    }
    require(MacKeyMapping::deleteKey('B').code == 0x08 && !MacKeyMapping::deleteKey('B').extended &&
                MacKeyMapping::deleteKey('D').code == 0x2E &&
                MacKeyMapping::deleteKey('D').extended,
            "delete mappings must preserve their protocol identities");
    require(!MacKeyMapping::ignoreCommand(false, false, true, false),
            "Command mapped to Ctrl or Alt must work without system capture");
    require(MacKeyMapping::ignoreCommand(true, false, true, false) &&
                MacKeyMapping::ignoreCommand(true, false, false, false),
            "uncaptured Command mapped to Win must be ignored unless previously forwarded");
    require(!MacKeyMapping::ignoreCommand(true, false, false, true) &&
                !MacKeyMapping::ignoreCommand(true, true, true, false),
            "capture changes must not block the release of a forwarded Command key");
    const auto enter = KeyboardState::pack(static_cast<short>(0x800D), MODIFIER_CTRL, 0);
    const auto numpadEnter =
        KeyboardState::pack(static_cast<short>(0x800D), MODIFIER_CTRL | MODIFIER_EXTENDED, 0);
    QHash<int, uint32_t> physicalKeys;
    QSet<uint32_t> mappedLocalKeys;
    MacKeyMapping::recordSend(physicalKeys, mappedLocalKeys, 1, enter, true, -1);
    require(physicalKeys.isEmpty(), "failed key-down must not be marked forwarded");
    MacKeyMapping::recordSend(physicalKeys, mappedLocalKeys, 1, enter, true, 0);
    require(physicalKeys.value(1) == enter, "successful key-down must retain its identity");
    MacKeyMapping::recordSend(physicalKeys, mappedLocalKeys, 1, enter, false, -1);
    require(physicalKeys.contains(1) && mappedLocalKeys.contains(enter),
            "failed key-up must remain available to cleanup");
    KeyboardState::raiseKeys(mappedLocalKeys, true, [](uint32_t) { return -1; });
    MacKeyMapping::reconcileReleased(physicalKeys, mappedLocalKeys);
    require(physicalKeys.contains(1), "failed cleanup must retain the physical mapping");
    KeyboardState::raiseKeys(mappedLocalKeys, true, [](uint32_t) { return 0; });
    MacKeyMapping::reconcileReleased(physicalKeys, mappedLocalKeys);
    require(physicalKeys.isEmpty(), "successful cleanup must clear the physical mapping");
    MacKeyMapping::recordSend(physicalKeys, mappedLocalKeys, 1, enter, true, 0);
    MacKeyMapping::recordSend(physicalKeys, mappedLocalKeys, 1, enter, false, 0);
    require(physicalKeys.isEmpty(), "successful key-up must clear its physical mapping");
    const auto international =
        KeyboardState::pack(static_cast<short>(0x80E2), MODIFIER_SHIFT, SS_KBE_FLAG_NON_NORMALIZED);
    QSet<uint32_t> keys{ enter, numpadEnter, international };
    require(keys.size() == 3, "main and numpad Enter must remain independent held keys");
    keys.remove(KeyboardState::pack(static_cast<short>(0x800D), 0, 0));
    require(keys.contains(numpadEnter), "releasing Ctrl before Enter must still match main Enter");
    require(!keys.contains(enter), "ordinary modifiers must not change key identity");
    keys.insert(enter);

    QVector<uint32_t> released;
    int failures = KeyboardState::raiseKeys(keys, true, [&](uint32_t state) {
        released.append(state);
        return state == numpadEnter ? -1 : 0;
    });
    require(failures == 1 && released.size() == 3, "release must attempt every held key");
    require(keys.size() == 1 && keys.contains(numpadEnter), "only failed releases remain pending");
    require(KeyboardState::code(numpadEnter) == static_cast<short>(0x800D),
            "release must retain the full protocol key code");
    require(KeyboardState::extendedModifier(numpadEnter) == MODIFIER_EXTENDED,
            "numpad release must retain its extended modifier");
    require(KeyboardState::flags(international) == SS_KBE_FLAG_NON_NORMALIZED,
            "international key release must retain its normalization flag");
    released.clear();
    failures = KeyboardState::raiseKeys(keys, false, [&](uint32_t state) {
        released.append(state);
        return 0;
    });
    require(failures == 0 && released.size() == 1 && keys.contains(numpadEnter),
            "temporary release must retain local state for physical key-up");
    require(KeyboardState::raiseKeys(keys, true, [](uint32_t) { return 0; }) == 0 && keys.isEmpty(),
            "successful retry must clear the pending release");

    QSet<uint32_t> localKeys{ enter };
    QSet<uint32_t> forwardedKeys{ numpadEnter, international };
    released.clear();
    failures =
        KeyboardState::raiseKeys(localKeys, forwardedKeys, true, false, false, [&](uint32_t state) {
            released.append(state);
            return 0;
        });
    require(failures == 0 && released.size() == 1 && localKeys.isEmpty(),
            "ordinary cleanup must release only locally held keys");
    require(forwardedKeys.size() == 2 && forwardedKeys.contains(numpadEnter) &&
                forwardedKeys.contains(international),
            "ordinary cleanup must retain the forwarded connection ledger");
    failures = KeyboardState::raiseKeys(localKeys, forwardedKeys, true, true, true,
                                        [](uint32_t) { return 0; });
    require(failures == 0 && forwardedKeys.isEmpty(),
            "connection cleanup must clear successfully released forwarded keys");

    QSet<uint32_t> overlapLocal{ enter };
    QSet<uint32_t> overlapForwarded{ enter };
    int overlapCalls = 0;
    failures =
        KeyboardState::raiseKeys(overlapLocal, overlapForwarded, true, true, true, [&](uint32_t) {
            overlapCalls++;
            return 0;
        });
    require(failures == 0 && overlapCalls == 1 && overlapLocal.isEmpty() &&
                overlapForwarded.isEmpty(),
            "a key held locally and forwarded must be released once");

    QSet<uint32_t> failedForwarded{ numpadEnter };
    failures = KeyboardState::raiseKeys(localKeys, failedForwarded, true, true, true,
                                        [](uint32_t) { return -1; });
    require(failures == 1 && failedForwarded.size() == 1 && failedForwarded.contains(numpadEnter),
            "failed connection cleanup must retain the forwarded release");

    int calls = 0;
    KeyboardState::raiseKeys(keys, true, [&](uint32_t) {
        calls++;
        return 0;
    });
    require(calls == 0, "empty state must not send spurious releases");
    return 0;
}
