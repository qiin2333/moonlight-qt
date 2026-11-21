#pragma once

#include <QByteArray>
#include <cstdint>

struct RemoteCursorUpdate
{
    bool hasShape = false;
    bool visible = false;
    uint32_t shapeId = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    int16_t hotspotX = 0;
    int16_t hotspotY = 0;
    QByteArray bgra;

    // Visibility-only packets must not erase an unconsumed shape update.
    void coalesce(const RemoteCursorUpdate& newer)
    {
        if (newer.hasShape || !hasShape) {
            *this = newer;
        } else {
            visible = newer.visible;
        }
    }
};
