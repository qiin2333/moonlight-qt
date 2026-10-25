#include "streaming/video/ffmpeg-renderers/videotexturesampling.h"

#include <cmath>
#include <cstdio>

int main()
{
    // HEVC/AV1 1440p decoder surfaces are padded to 1536 rows. Ordinary
    // decoding must skip the padding; AMF output must keep the whole image.
    const auto decoded = VideoTextureSampling::calculate(2560, 1440, 2560, 1536, false);
    if (decoded.uMax != 1.0f || decoded.vMax != 0.9375f ||
        std::fabs(decoded.chromaVMax - 1439.0f / 1536) > 0.00001f) {
        std::fprintf(stderr, "decoder padding sampling changed\n");
        return 1;
    }

    const int sizes[][4] = {
        { 2560, 1440, 2560, 1536 }, // Original padded frame context must not crop AMF.
        { 1920, 1080, 1920, 1088 }, // H.264 vertical padding.
        { 2720, 1260, 2816, 1280 }, // Both axes padded.
        { 1280, 720, 2560, 1440 },  // Upscaled AMF output.
        { 3840, 2160, 2560, 1440 }, // Downscaled AMF output.
    };
    for (const auto& size : sizes) {
        const auto enhanced =
            VideoTextureSampling::calculate(size[0], size[1], size[2], size[3], true);
        // Luma and chroma must both reach the last row/column. Fixing only
        // luma still smears chroma across the bottom of the enhanced image.
        if (enhanced.uMax != 1.0f || enhanced.vMax != 1.0f || enhanced.chromaUMax != 1.0f ||
            enhanced.chromaVMax != 1.0f) {
            std::fprintf(stderr, "enhanced image is cropped\n");
            return 1;
        }
    }
    const auto unpadded = VideoTextureSampling::calculate(2560, 1440, 2560, 1440, false);
    if (unpadded.uMax != 1.0f || unpadded.vMax != 1.0f || unpadded.chromaUMax != 1.0f ||
        unpadded.chromaVMax != 1.0f) {
        return 1;
    }
    std::puts("video texture sampling tests passed");
    return 0;
}
