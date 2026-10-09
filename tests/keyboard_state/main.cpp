#include "streaming/input/keyboardstate.h"
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
    const auto enter = KeyboardState::pack(static_cast<short>(0x800D), MODIFIER_CTRL, 0);
    const auto numpadEnter =
        KeyboardState::pack(static_cast<short>(0x800D), MODIFIER_CTRL | MODIFIER_EXTENDED, 0);
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
    int calls = 0;
    KeyboardState::raiseKeys(keys, true, [&](uint32_t) {
        calls++;
        return 0;
    });
    require(calls == 0, "empty state must not send spurious releases");
    return 0;
}
