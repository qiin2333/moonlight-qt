#pragma once

// Pure helpers for cursor protocol v2: mapping host stream coordinates into the
// letterboxed video region of the window, and sizing host cursor shapes. No SDL or
// Qt dependencies, so they are unit tested directly (tests/host_cursor_math).

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace HostCursorMath {

struct Rect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;

    bool operator==(const Rect& other) const
    {
        return x == other.x && y == other.y && w == other.w && h == other.h;
    }
};

struct Point {
    int x = 0;
    int y = 0;
};

// Inverse of the absolute mouse mapping in mouse.cpp: the host receives
// (windowX - video.x) on a video.w-wide reference plane, which it scales to the
// stream's referenceWidth. `video` is the letterboxed region in window units.
inline Point streamToWindow(const Rect& video, int referenceWidth, int referenceHeight, int32_t x, int32_t y)
{
    Point p;
    if (referenceWidth <= 0 || referenceHeight <= 0) {
        return p;
    }
    // Host coordinates are int32 and may be far off the stream; keep the result in int range.
    const double limit = 1e9;
    p.x = video.x + (int)std::lround(std::clamp((double)x * video.w / referenceWidth, -limit, limit));
    p.y = video.y + (int)std::lround(std::clamp((double)y * video.h / referenceHeight, -limit, limit));
    return p;
}

// A host clip rect (exclusive right/bottom) in window units, clamped to the video.
inline Rect streamRectToWindow(const Rect& video, int referenceWidth, int referenceHeight,
                               int32_t left, int32_t top, int32_t right, int32_t bottom)
{
    Point a = streamToWindow(video, referenceWidth, referenceHeight, left, top);
    Point b = streamToWindow(video, referenceWidth, referenceHeight, right, bottom);
    Rect r;
    r.x = std::clamp(a.x, video.x, video.x + video.w);
    r.y = std::clamp(a.y, video.y, video.y + video.h);
    r.w = std::clamp(b.x, video.x, video.x + video.w) - r.x;
    r.h = std::clamp(b.y, video.y, video.y + video.h) - r.y;
    r.w = (std::max)(r.w, 0);
    r.h = (std::max)(r.h, 0);
    return r;
}

// Window units per host cursor pixel: the host's stream pixels per shape pixel
// (16.16) times the window units per stream pixel. On macOS window units are
// points, so this also replaces the backing-scale correction.
inline double cursorScale(uint32_t scaleQ16, const Rect& video, int referenceWidth)
{
    if (referenceWidth <= 0 || video.w <= 0 || scaleQ16 == 0) {
        return 1.0;
    }
    return (scaleQ16 / 65536.0) * video.w / referenceWidth;
}

// Largest local cursor we create, in either dimension.
constexpr int kMaxCursorDimension = 512;

inline int scaledDimension(int size, double scale)
{
    return std::clamp((int)std::lround(size * scale), 1, kMaxCursorDimension);
}

}
