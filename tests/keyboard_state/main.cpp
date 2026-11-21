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

    // Sunshine only recognizes N after three real modifier down events. The
    // menu shortcut must not disturb unrelated held keys or leave modifiers down.
    QSet<uint32_t> chordLocal{ enter };
    QSet<uint32_t> chordForwarded;
    QVector<QPair<int, int>> chordEvents;
    auto recordChord = [&](short key, char action) {
        chordEvents.append(
            qMakePair(static_cast<int>(static_cast<uint16_t>(key)), static_cast<int>(action)));
        return 0;
    };
    auto result =
        KeyboardState::sendHostCursorShortcut(chordLocal, chordForwarded, false, recordChord);
    require(result == KeyboardState::ShortcutResult::Queued && chordEvents.size() == 8,
            "host cursor shortcut must send all four downs and ups");
    const int expectedKeys[] = { 0x80A2, 0x80A4, 0x80A0, 0x804E, 0x804E, 0x80A0, 0x80A4, 0x80A2 };
    for (int i = 0; i < chordEvents.size(); ++i) {
        require(chordEvents[i].first == expectedKeys[i] &&
                    chordEvents[i].second == (i < 4 ? KEY_ACTION_DOWN : KEY_ACTION_UP),
                "host cursor shortcut key ordering is incorrect");
    }
    require(chordLocal.size() == 1 && chordLocal.contains(enter),
            "shortcut must preserve unrelated held keys");
    require(chordForwarded.size() == 4, "shortcut downs must enter the connection ledger");

    chordEvents.clear();
    result = KeyboardState::sendHostCursorShortcut(chordLocal, chordForwarded, true, recordChord);
    require(result == KeyboardState::ShortcutResult::ModifierHeld && chordEvents.isEmpty(),
            "shortcut must not interfere with physical modifiers");
    const auto heldShift = KeyboardState::pack(static_cast<short>(0x80A1), 0, 0);
    chordLocal.insert(heldShift);
    result = KeyboardState::sendHostCursorShortcut(chordLocal, chordForwarded, false, recordChord);
    require(result == KeyboardState::ShortcutResult::ModifierHeld && chordEvents.isEmpty() &&
                chordLocal.contains(heldShift),
            "shortcut must not release a tracked modifier");
    chordLocal.remove(heldShift);

    chordEvents.clear();
    result = KeyboardState::sendHostCursorShortcut(
        chordLocal, chordForwarded, false, [&](short key, char action) {
            recordChord(key, action);
            return static_cast<uint16_t>(key) == 0x804E && action == KEY_ACTION_DOWN ? -1 : 0;
        });
    require(result == KeyboardState::ShortcutResult::Failed && chordEvents.size() == 7 &&
                chordLocal.size() == 1 && chordLocal.contains(enter),
            "failed N down must still release all queued modifiers");

    chordEvents.clear();
    result = KeyboardState::sendHostCursorShortcut(
        chordLocal, chordForwarded, false, [&](short key, char action) {
            recordChord(key, action);
            return static_cast<uint16_t>(key) == 0x80A4 && action == KEY_ACTION_UP ? -1 : 0;
        });
    require(result == KeyboardState::ShortcutResult::Failed &&
                chordLocal.contains(KeyboardState::pack(static_cast<short>(0x80A4), 0, 0)),
            "failed modifier release must remain pending for cleanup");
    return 0;
}
