#pragma once

// Automatic mouse mode: picks absolute input with a local cursor while the host
// cursor is visible and free, and relative input while a game hides or locks it.
// It follows the host without delays, as a local mouse would.
// Pure logic with explicit timestamps (no SDL/Qt), unit tested in
// tests/cursor_mode_policy. See Foundation Sunshine's docs/client-cursor-v2-design.md.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>

namespace CursorModePolicy {

// All tuning lives here.
// A clip smaller than this in both dimensions is a lock (FPS games), not a
// confinement (RTS games keep a visible cursor inside their window).
constexpr double kTightClipFraction = 0.10;
constexpr int kTightClipMinPixels = 32;
// A visible cursor the host keeps putting back on the same spot is being recentred.
constexpr uint32_t kRecentreWindowMs = 300;
constexpr int kRecentreReports = 3;
constexpr double kRecentreToleranceFraction = 0.002;
constexpr int kRecentreToleranceMinPixels = 2;

enum class Mode {
    Fallback,   // the user's regular mouse setting; the cursor is in the video
    Absolute,   // absolute input with the host cursor drawn locally
    Relative,   // relative input; the local cursor is hidden
};

enum class Source {
    Fallback,   // automatic mode unavailable (unsupported host, host fallback, no state yet)
    Automatic,
    Locked,     // the user locked a mode for the rest of the session
};

struct Decision {
    Mode mode;
    Source source;
    // Absolute time at which update() must run again even without new input, or 0.
    uint32_t nextUpdateMs;
};

// Switching rules:
// - Relative input starts with the first mouse motion after the host hides or locks
//   its cursor, and that motion is already sent as relative. Until the mouse moves
//   both modes send the same thing, so waiting costs nothing, and an app that only
//   hides the cursor (Windows while typing, a video player) doesn't grab the pointer
//   until the mouse moves over the stream. Such an app shows its cursor again on
//   that motion, and absolute input returns a round trip later.
// - Absolute input returns as soon as the host cursor is visible and free, except
//   while a mouse button is held: a game may show its cursor before a camera drag
//   ends, and switching mid-drag turns the rest of the drag into a jump.
class Policy {
public:
    // The latest host state. `available` is false until the host's first full state
    // and while it reports FALLBACK. Clip sizes are in stream pixels.
    void setHostState(bool available, bool visible,
                      bool hasClip, int clipWidth, int clipHeight,
                      int referenceWidth, int referenceHeight)
    {
        m_Available = available;
        m_Visible = visible;
        m_HasClip = hasClip;
        m_ClipWidth = clipWidth;
        m_ClipHeight = clipHeight;
        m_ReferenceWidth = referenceWidth;
        m_ReferenceHeight = referenceHeight;
    }

    // A position the host reported while its cursor was visible.
    void noteHostPosition(uint32_t nowMs, int x, int y)
    {
        m_Reports[m_NextReport] = { nowMs, x, y, true };
        m_NextReport = (m_NextReport + 1) % m_Reports.size();
    }

    // Local mouse motion. Returns true when update() must run before the motion is
    // sent: the host wants relative input and this is the first motion since.
    bool noteLocalMouseMotion(uint32_t nowMs)
    {
        if (m_Moved || !awaitingRelative(nowMs)) {
            return false;
        }
        m_Moved = true;
        return true;
    }

    // Whether the host cursor is hidden, locked or being recentred.
    bool hostWantsRelative(uint32_t nowMs) const
    {
        uint32_t recentreExpiry;
        return wantsRelative(nowMs, recentreExpiry);
    }

    // Still in absolute mode while automatic mode wants relative: the next mouse
    // motion switches. Input meanwhile is meant for a game holding its cursor.
    bool awaitingRelative(uint32_t nowMs) const
    {
        return m_Available && !m_Locked && m_Mode == Mode::Absolute && hostWantsRelative(nowMs);
    }

    // Whether the local cursor should be confined to the host's clip: a visible cursor
    // kept inside a large rect (an RTS window). A lock or a hidden cursor is not
    // confined, since pinning the local cursor would stop the very motion that
    // switches to relative input.
    bool hostClipConfinesCursor(uint32_t nowMs) const
    {
        return m_Available && m_HasClip && !hostWantsRelative(nowMs);
    }

    // Call update() again when the buttons come up.
    void setMouseButtonsHeld(bool held)
    {
        m_ButtonsHeld = held;
    }

    void lock(Mode mode)
    {
        m_Locked = true;
        m_LockedMode = mode;
    }

    void unlock()
    {
        m_Locked = false;
    }

    bool isLocked() const
    {
        return m_Locked;
    }

    Mode mode() const
    {
        return m_Mode;
    }

    Decision update(uint32_t nowMs)
    {
        if (!m_Available) {
            m_Mode = Mode::Fallback;
            m_Moved = false;
            return { Mode::Fallback, Source::Fallback, 0 };
        }

        if (m_Locked) {
            m_Mode = m_LockedMode;
            m_Moved = false;
            return { m_Mode, Source::Locked, 0 };
        }

        uint32_t recentreExpiry = 0;
        const bool relative = wantsRelative(nowMs, recentreExpiry);

        if (m_Mode == Mode::Fallback) {
            // Leaving fallback (the first host state, a host fallback ending) there is
            // no absolute input in progress to keep, so take the host's state as it is.
            m_Mode = relative ? Mode::Relative : Mode::Absolute;
        }
        else if (relative && m_Mode == Mode::Absolute) {
            if (m_Moved) {
                m_Mode = Mode::Relative;
            }
        }
        else if (!relative && m_Mode == Mode::Relative && !m_ButtonsHeld) {
            m_Mode = Mode::Absolute;
        }

        if (!relative || m_Mode != Mode::Absolute) {
            m_Moved = false;
        }
        return { m_Mode, Source::Automatic, recentreExpiry };
    }

private:
    struct Report {
        uint32_t atMs;
        int x;
        int y;
        bool valid;
    };

    // Timestamps wrap (SDL_GetTicks() is 32-bit); compare by signed difference.
    static bool reached(uint32_t nowMs, uint32_t dueMs)
    {
        return (int32_t)(nowMs - dueMs) >= 0;
    }

    static uint32_t latest(uint32_t a, uint32_t b)
    {
        return reached(a, b) ? a : b;
    }

    bool tightlyClipped() const
    {
        if (!m_HasClip) {
            return false;
        }
        const int maxWidth = (std::max)(kTightClipMinPixels, (int)(m_ReferenceWidth * kTightClipFraction));
        const int maxHeight = (std::max)(kTightClipMinPixels, (int)(m_ReferenceHeight * kTightClipFraction));
        return m_ClipWidth < maxWidth && m_ClipHeight < maxHeight;
    }

    // True if at least kRecentreReports recent reports land on one spot. `expiry`
    // is when that stops being true without new reports.
    bool recentring(uint32_t nowMs, uint32_t& expiry) const
    {
        const int tolerance = (std::max)(kRecentreToleranceMinPixels,
                                       (int)((std::max)(m_ReferenceWidth, m_ReferenceHeight) * kRecentreToleranceFraction));
        bool found = false;
        for (const Report& anchor : m_Reports) {
            if (!anchor.valid || nowMs - anchor.atMs > kRecentreWindowMs) {
                continue;
            }
            std::array<uint32_t, 16> ages {};
            int count = 0;
            for (const Report& r : m_Reports) {
                if (r.valid && nowMs - r.atMs <= kRecentreWindowMs &&
                        std::abs(r.x - anchor.x) <= tolerance && std::abs(r.y - anchor.y) <= tolerance) {
                    ages[count++] = nowMs - r.atMs;
                }
            }
            if (count >= kRecentreReports) {
                // True until the kRecentreReports-th newest of these reports ages out.
                std::sort(ages.begin(), ages.begin() + count);
                const uint32_t candidate = nowMs + (kRecentreWindowMs - ages[kRecentreReports - 1]) + 1;
                expiry = found ? latest(expiry, candidate) : candidate;
                found = true;
            }
        }
        return found;
    }

    bool wantsRelative(uint32_t nowMs, uint32_t& recentreExpiry) const
    {
        recentreExpiry = 0;
        if (!m_Visible || tightlyClipped()) {
            return true;
        }
        return recentring(nowMs, recentreExpiry);
    }

    bool m_Available = false;
    bool m_Visible = false;
    bool m_HasClip = false;
    int m_ClipWidth = 0;
    int m_ClipHeight = 0;
    int m_ReferenceWidth = 0;
    int m_ReferenceHeight = 0;

    std::array<Report, 16> m_Reports {};
    size_t m_NextReport = 0;

    bool m_ButtonsHeld = false;
    bool m_Moved = false;   // the mouse moved while the host wanted relative input

    bool m_Locked = false;
    Mode m_LockedMode = Mode::Absolute;

    Mode m_Mode = Mode::Fallback;
};

}
