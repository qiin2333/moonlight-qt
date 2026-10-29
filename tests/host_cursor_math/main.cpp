#include "streaming/input/hostcursormath.h"

#include <cstdio>
#include <cstdlib>

using namespace HostCursorMath;

static int g_Failures;

static void require(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        g_Failures++;
    }
}

// Forward mapping from mouse.cpp: the host gets (windowX - video.x) on a
// video.w-wide plane and scales it to the stream's reference width.
static int windowToStream(const Rect& video, int reference, int windowX, bool horizontal)
{
    int offset = horizontal ? video.x : video.y;
    int extent = horizontal ? video.w : video.h;
    return (int)std::lround((double)(windowX - offset) * reference / extent);
}

static void testStreamToWindowInvertsAbsoluteInput()
{
    // 1920x1080 stream scaled into a 2560x1440 window: no bars, 4:3 scale.
    const Rect fullscreen { 0, 0, 2560, 1440 };
    Point p = streamToWindow(fullscreen, 1920, 1080, 960, 540);
    require(p.x == 1280 && p.y == 720, "centre of a scaled stream");

    // 1920x1080 stream letterboxed in a 1000x1000 window: 1000x563 video, 219 px bars.
    const Rect letterboxed { 0, 219, 1000, 563 };
    p = streamToWindow(letterboxed, 1920, 1080, 0, 0);
    require(p.x == 0 && p.y == 219, "letterbox top-left");
    p = streamToWindow(letterboxed, 1920, 1080, 1920, 1080);
    require(p.x == 1000 && p.y == 782, "letterbox bottom-right");

    for (int sx = 0; sx <= 1920; sx += 7) {
        Point w = streamToWindow(letterboxed, 1920, 1080, sx, 540);
        // A window pixel spans ~1.9 stream pixels, so the round trip is within one window pixel.
        int back = windowToStream(letterboxed, 1920, w.x, true);
        require(std::abs(back - sx) <= 2, "round trip through the window");
    }

    p = streamToWindow(letterboxed, 0, 1080, 10, 10);
    require(p.x == 0 && p.y == 0, "zero reference is rejected");

    // Positions far off the stream stay in int range.
    p = streamToWindow(fullscreen, 1, 1, INT32_MAX, INT32_MIN);
    require(p.x > 0 && p.y < 0, "extreme host coordinates are clamped, not overflowed");
}

static void testClipRectIsClampedToTheVideo()
{
    const Rect video { 100, 50, 960, 540 };  // 1920x1080 stream at half size
    Rect r = streamRectToWindow(video, 1920, 1080, 200, 100, 1000, 700);
    require(r.x == 200 && r.y == 100 && r.w == 400 && r.h == 300, "clip maps to window units");

    r = streamRectToWindow(video, 1920, 1080, -500, -500, 5000, 5000);
    require(r == video, "clip larger than the stream is the video");

    r = streamRectToWindow(video, 1920, 1080, 3000, 3000, 4000, 4000);
    require(r.w == 0 && r.h == 0, "clip outside the stream is empty");
}

static void testCursorScale()
{
    const Rect video { 0, 0, 2560, 1440 };
    // Host stream at 0.75 stream px per desktop px, shown at 4/3: the cursor keeps its host size.
    double s = cursorScale(49152, video, 1920);
    require(std::abs(s - 1.0) < 1e-9, "scale composes host and window factors");

    // macOS window units are points: a 2560x1440 stream in a 1280-point-wide window
    // draws a 32 px host cursor at 16 points, 32 Retina pixels, as in the video.
    const Rect retina { 0, 0, 1280, 720 };
    require(scaledDimension(32, cursorScale(0x10000, retina, 2560)) == 16, "point-sized cursor on a Retina window");

    require(cursorScale(0x10000, video, 0) == 1.0, "zero reference falls back to 1");
    require(cursorScale(0, video, 1920) == 1.0, "zero scale falls back to 1");
    require(scaledDimension(32, 1.5) == 48, "scaled dimension");
    require(scaledDimension(256, 10.0) == kMaxCursorDimension, "scaled dimension is capped");
    require(scaledDimension(4, 0.01) == 1, "scaled dimension is at least 1");
}

int main()
{
    testStreamToWindowInvertsAbsoluteInput();
    testClipRectIsClampedToTheVideo();
    testCursorScale();
    if (g_Failures == 0) {
        std::printf("PASS host cursor math\n");
    }
    return g_Failures == 0 ? 0 : 1;
}
