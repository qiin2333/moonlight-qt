#include "streaming/input/cursormodepolicy.h"

#include <cstdio>

using namespace CursorModePolicy;

static int g_Failures;

static void require(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        g_Failures++;
    }
}

static constexpr int kWidth = 1920;
static constexpr int kHeight = 1080;

static void host(Policy& p, bool visible, int clipW = -1, int clipH = -1)
{
    p.setHostState(true, visible, clipW >= 0, clipW, clipH, kWidth, kHeight);
}

// A policy that has seen the host's first state with the cursor visible.
static Policy readyAbsolute()
{
    Policy p;
    host(p, true);
    p.update(0);
    return p;
}

// The host hides its cursor and the mouse moves: relative from that motion on.
static Policy readyRelative()
{
    Policy p = readyAbsolute();
    host(p, false);
    p.update(0);
    p.noteLocalMouseMotion(0);
    p.update(0);
    return p;
}

static void testFallbackUntilAvailable()
{
    Policy p;
    Decision d = p.update(0);
    require(d.mode == Mode::Fallback && d.source == Source::Fallback && d.nextUpdateMs == 0, "no host state is fallback");
    require(!p.noteLocalMouseMotion(0), "motion in fallback needs no update");

    p.setHostState(false, true, false, 0, 0, kWidth, kHeight);
    d = p.update(10);
    require(d.mode == Mode::Fallback, "host FALLBACK is fallback");

    // The first usable state applies at once, hidden included: there is no absolute
    // input in progress to keep.
    host(p, false);
    d = p.update(20);
    require(d.mode == Mode::Relative && d.source == Source::Automatic, "first state hidden -> relative now");

    Policy q;
    host(q, true);
    require(q.update(20).mode == Mode::Absolute, "first state visible -> absolute now");
}

static void testRelativeOnFirstMotion()
{
    Policy p = readyAbsolute();
    host(p, false);
    Decision d = p.update(1000);
    require(d.mode == Mode::Absolute && d.nextUpdateMs == 0, "hidden, mouse still: nothing to send yet");
    require(p.update(9000).mode == Mode::Absolute, "however long the mouse stays still");

    require(p.noteLocalMouseMotion(9000), "the first motion asks for an update");
    require(p.update(9000).mode == Mode::Relative, "relative before that motion is sent");
    require(!p.noteLocalMouseMotion(9001), "in relative mode motion needs no update");

    // Motion before the host hid its cursor doesn't count.
    Policy q = readyAbsolute();
    require(!q.noteLocalMouseMotion(500), "visible cursor: motion needs no update");
    host(q, false);
    require(q.update(1000).mode == Mode::Absolute, "earlier motion doesn't switch");

    // The cursor comes back before the mouse moves: nothing happened, nothing pending.
    Policy r = readyAbsolute();
    host(r, false);
    r.update(1000);
    host(r, true);
    require(r.update(1010).mode == Mode::Absolute, "still absolute");
    require(!r.noteLocalMouseMotion(1020), "and motion needs no update");
}

static void testAwaitingRelative()
{
    // Between the host hiding its cursor and the first motion, clicks and the wheel
    // are meant for the game (the client doesn't drop them in the letterbox).
    Policy p = readyAbsolute();
    require(!p.awaitingRelative(0) && !p.hostWantsRelative(0), "visible cursor: nothing pending");
    host(p, false);
    p.update(100);
    require(p.awaitingRelative(100) && p.hostWantsRelative(100), "hidden, mouse still: awaiting relative");
    p.noteLocalMouseMotion(200);
    p.update(200);
    require(!p.awaitingRelative(200) && p.hostWantsRelative(200), "relative now: no longer awaiting");

    // A lock to absolute, or fallback, never awaits relative.
    Policy q = readyAbsolute();
    q.lock(Mode::Absolute);
    host(q, false);
    q.update(100);
    require(!q.awaitingRelative(100), "locked absolute doesn't await relative");
    q.setHostState(false, false, false, 0, 0, kWidth, kHeight);
    q.unlock();
    q.update(200);
    require(!q.awaitingRelative(200), "fallback doesn't await relative");
}

static void testAbsoluteImmediately()
{
    Policy p = readyRelative();
    host(p, true);
    Decision d = p.update(1000);
    require(d.mode == Mode::Absolute && d.nextUpdateMs == 0, "visible again -> absolute now");
}

static void testFollowsEveryChange()
{
    // A cursor flashed for a single poll is followed both ways: there is no delay.
    Policy p = readyRelative();
    host(p, true);
    require(p.update(1000).mode == Mode::Absolute, "flash shown");
    host(p, false);
    p.update(1005);
    require(p.noteLocalMouseMotion(1006) && p.update(1006).mode == Mode::Relative, "flash hidden, next motion relative");

    // Every hide with motion and every show switches once.
    Policy q = readyAbsolute();
    int switches = 0;
    Mode last = Mode::Absolute;
    for (uint32_t t = 100; t < 5000; t += 5) {
        host(q, ((t / 1000) % 2) == 0);
        q.update(t);
        if (q.noteLocalMouseMotion(t)) {
            q.update(t);
        }
        Mode m = q.update(t).mode;
        switches += m != last;
        last = m;
    }
    require(switches == 4, "two hides and two shows, four switches");
}

static void testHeldButtonDefersReturnToAbsolute()
{
    // An MMO camera drag: the game hides the cursor while the right button is held,
    // and may show it again before the button is released. Switching back to
    // absolute mid-drag would turn the rest of the drag into a jump.
    Policy p = readyRelative();
    p.setMouseButtonsHeld(true);
    host(p, true);
    Decision d = p.update(1000);
    require(d.mode == Mode::Relative && d.nextUpdateMs == 0, "no return to absolute while a button is held");
    require(p.update(5000).mode == Mode::Relative, "however long it is held");
    p.setMouseButtonsHeld(false);
    require(p.update(5001).mode == Mode::Absolute, "released: absolute at once");

    // Entering relative mid-drag is what makes camera drags work, so it is not held.
    Policy q = readyAbsolute();
    q.setMouseButtonsHeld(true);
    host(q, false);
    q.update(1000);
    require(q.noteLocalMouseMotion(1001) && q.update(1001).mode == Mode::Relative, "relative while a button is held");
}

static void testClips()
{
    // An RTS confining a visible cursor to its 800x600 window stays absolute.
    Policy p = readyAbsolute();
    host(p, true, 800, 600);
    p.update(1000);
    require(!p.noteLocalMouseMotion(1000) && p.update(5000).mode == Mode::Absolute, "large clip stays absolute");

    // A visible cursor clipped to a tiny rect is locked.
    host(p, true, 40, 40);
    p.update(6000);
    require(p.noteLocalMouseMotion(6001) && p.update(6001).mode == Mode::Relative, "tiny clip -> relative");

    // Narrow in one dimension only is not a lock.
    Policy q = readyAbsolute();
    host(q, true, 1920, 20);
    q.update(1000);
    require(!q.noteLocalMouseMotion(1000) && q.update(5000).mode == Mode::Absolute, "a clip tiny in one dimension only stays absolute");

    // A clip that misses the stream entirely (empty) is a lock.
    Policy r = readyAbsolute();
    host(r, true, 0, 0);
    r.update(1000);
    require(r.noteLocalMouseMotion(1001) && r.update(1001).mode == Mode::Relative, "empty clip -> relative");
}

static void testClipConfinement()
{
    // Only a visible cursor kept inside a large rect confines the local cursor.
    Policy p = readyAbsolute();
    require(!p.hostClipConfinesCursor(0), "no clip, nothing to confine");
    host(p, true, 800, 600);
    require(p.hostClipConfinesCursor(0), "RTS window clip confines");

    // MSFS's right-drag hides the cursor and locks it to one pixel. Pinning the local
    // cursor there stopped all motion, so it never switched to relative.
    host(p, true, 1, 1);
    require(!p.hostClipConfinesCursor(0), "a lock doesn't confine");
    host(p, false, 1, 1);
    require(!p.hostClipConfinesCursor(0), "a hidden locked cursor doesn't confine");
    host(p, false, 800, 600);
    require(!p.hostClipConfinesCursor(0), "a hidden cursor doesn't confine");

    p.setHostState(false, true, true, 800, 600, kWidth, kHeight);
    require(!p.hostClipConfinesCursor(0), "nothing confines in fallback");
}

static void testRecentring()
{
    Policy p = readyAbsolute();
    // The host keeps putting a visible cursor back at the centre.
    p.noteHostPosition(1000, 960, 540);
    p.noteHostPosition(1016, 961, 540);
    require(!p.noteLocalMouseMotion(1016) && p.update(1016).nextUpdateMs == 0, "two returns are not recentring");
    p.noteHostPosition(1033, 960, 541);
    Decision d = p.update(1033);
    require(d.mode == Mode::Absolute && d.nextUpdateMs == 1000 + kRecentreWindowMs + 1, "three returns: recentring until the oldest ages out");
    require(p.noteLocalMouseMotion(1040) && p.update(1040).mode == Mode::Relative, "recentred cursor -> relative on motion");

    // Recentring stops when the oldest of the three reports ages out.
    d = p.update(1100);
    require(d.mode == Mode::Relative && d.nextUpdateMs == 1000 + kRecentreWindowMs + 1, "timer for the recentring to expire");
    require(p.update(1000 + kRecentreWindowMs + 1).mode == Mode::Absolute, "free again -> absolute at once");

    // Reports that wander (a cursor moving slowly) are not recentring.
    Policy q = readyAbsolute();
    for (int i = 0; i < 10; i++) {
        q.noteHostPosition(1000 + i * 20, 900 + i * 6, 500);
    }
    require(!q.noteLocalMouseMotion(1200) && q.update(1200).nextUpdateMs == 0, "slow movement is not recentring");
}

static void testLocks()
{
    Policy p = readyAbsolute();
    p.lock(Mode::Relative);
    Decision d = p.update(10);
    require(d.mode == Mode::Relative && d.source == Source::Locked && d.nextUpdateMs == 0, "lock applies at once");
    host(p, true);
    require(p.update(5000).mode == Mode::Relative, "lock ignores the host");

    // The host losing cursor information still falls back; the lock resumes after.
    p.setHostState(false, true, false, 0, 0, kWidth, kHeight);
    require(p.update(6000).mode == Mode::Fallback, "fallback overrides a lock");
    host(p, true);
    require(p.update(7000).mode == Mode::Relative, "lock resumes");

    p.unlock();
    d = p.update(7001);
    require(d.mode == Mode::Absolute && d.source == Source::Automatic, "unlocked: the host's mode at once");

    // Locked absolute while the host hides its cursor: motion never switches.
    Policy q = readyAbsolute();
    q.lock(Mode::Absolute);
    host(q, false);
    q.update(1000);
    require(!q.noteLocalMouseMotion(1001) && q.update(1001).mode == Mode::Absolute, "locked absolute ignores motion");
}

static void testTickWrap()
{
    // The recentring timer wraps with SDL_GetTicks().
    const uint32_t start = 0xFFFFFFF0u;
    Policy p = readyAbsolute();
    p.noteHostPosition(start, 960, 540);
    p.noteHostPosition(start + 8, 960, 540);
    p.noteHostPosition(start + 16, 960, 540);
    Decision d = p.update(start + 16);
    require(d.nextUpdateMs == (uint32_t)(start + kRecentreWindowMs + 1), "expiry wraps");
    require(p.noteLocalMouseMotion(start + 20) && p.update(start + 20).mode == Mode::Relative, "relative across the wrap");
    require(p.update((uint32_t)(start + kRecentreWindowMs)).mode == Mode::Relative, "not expired across the wrap");
    require(p.update((uint32_t)(start + kRecentreWindowMs + 1)).mode == Mode::Absolute, "expired across the wrap");
}

int main()
{
    testFallbackUntilAvailable();
    testRelativeOnFirstMotion();
    testAwaitingRelative();
    testAbsoluteImmediately();
    testFollowsEveryChange();
    testHeldButtonDefersReturnToAbsolute();
    testClips();
    testClipConfinement();
    testRecentring();
    testLocks();
    testTickWrap();
    if (g_Failures == 0) {
        std::printf("PASS cursor mode policy\n");
    }
    return g_Failures == 0 ? 0 : 1;
}
