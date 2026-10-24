#pragma once

#include <Limelight.h>

// moonlight-common-c fills in missing callbacks in place, so a previous
// connection on the same session leaves a fake submitDecodeUnit behind. A
// pull renderer must start without one, or LiStartConnection() rejects it.
static inline void prepareVideoCallbacksForConnection(DECODER_RENDERER_CALLBACKS* callbacks)
{
    if (callbacks->capabilities & CAPABILITY_PULL_RENDERER) {
        callbacks->submitDecodeUnit = nullptr;
    }
}
