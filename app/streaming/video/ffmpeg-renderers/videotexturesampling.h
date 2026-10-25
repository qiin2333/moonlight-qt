#pragma once

// Sampling coordinates describe the texture being displayed, not necessarily
// the decoder surface: enhancement produces a new, unpadded image.
namespace VideoTextureSampling {
struct Coordinates
{
    float uMax;
    float vMax;
    float chromaUMax;
    float chromaVMax;
};

inline Coordinates calculate(int frameWidth, int frameHeight, int textureWidth, int textureHeight,
                             bool enhancedOutput)
{
    if (enhancedOutput) {
        return { 1.0f, 1.0f, 1.0f, 1.0f };
    }

    return {
        static_cast<float>(frameWidth) / textureWidth,
        static_cast<float>(frameHeight) / textureHeight,
        frameWidth != textureWidth ? static_cast<float>(frameWidth - 1) / textureWidth : 1.0f,
        frameHeight != textureHeight ? static_cast<float>(frameHeight - 1) / textureHeight : 1.0f,
    };
}
}
