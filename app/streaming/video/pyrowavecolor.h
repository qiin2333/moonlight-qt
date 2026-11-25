#pragma once

#include <Limelight.h>
#include <vulkan/vulkan.h>
#include <pyrowave.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/pixfmt.h>
}

namespace PyrowaveColor {

struct Contract
{
    int hdrMode = 0;
    pyrowave_cpu_buffer_format cpuFormat = PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;
    AVPixelFormat pixelFormat = AV_PIX_FMT_YUV420P;
    AVColorSpace colorSpace = AVCOL_SPC_BT709;
    AVColorPrimaries primaries = AVCOL_PRI_BT709;
    AVColorTransferCharacteristic transfer = AVCOL_TRC_BT709;
    pyrowave_color_metadata metadata = {};
    const char* label = "SDR";

    bool useFragmentPath(bool devicePrefersFragmentPath) const
    {
        // The PyroWave fragment path cannot scale R16_UNORM into 10-bit LSB samples.
        return hdrMode == 0 && devicePrefersFragmentPath;
    }
};

inline bool describe(int hdrMode, Contract& contract)
{
    if (hdrMode < 0 || hdrMode > 2) {
        return false;
    }

    contract = {};
    contract.hdrMode = hdrMode;
    contract.metadata.range = PYROWAVE_YCBCR_FULL;
    if (hdrMode == 0) {
        contract.metadata.primaries = PYROWAVE_COLOR_PRIMARIES_BT709;
        contract.metadata.transfer = PYROWAVE_TRANSFER_BT709;
        contract.metadata.transform = PYROWAVE_YCBCR_BT709;
    } else {
        contract.cpuFormat = PYROWAVE_CPU_BUFFER_FORMAT_YUV420P10;
        // PyroWave's P10 API writes native-endian, 16-bit containers with 10 valid LSBs.
        contract.pixelFormat = AV_PIX_FMT_YUV420P10;
        contract.colorSpace = AVCOL_SPC_BT2020_NCL;
        contract.primaries = AVCOL_PRI_BT2020;
        contract.transfer = hdrMode == 1 ? AVCOL_TRC_SMPTE2084 : AVCOL_TRC_ARIB_STD_B67;
        contract.metadata.primaries = PYROWAVE_COLOR_PRIMARIES_BT2020;
        contract.metadata.transfer = hdrMode == 1 ? PYROWAVE_TRANSFER_PQ : PYROWAVE_TRANSFER_HLG;
        contract.metadata.transform = PYROWAVE_YCBCR_BT2020;
        contract.label = hdrMode == 1 ? "HDR10" : "HLG";
    }
    return true;
}

inline bool matches(const Contract& contract, const pyrowave_color_metadata& metadata)
{
    return metadata.primaries == contract.metadata.primaries &&
           metadata.transfer == contract.metadata.transfer &&
           metadata.transform == contract.metadata.transform &&
           metadata.range == contract.metadata.range && metadata.chroma_siting <= 1;
}

inline void applyToFrame(const Contract& contract, const pyrowave_color_metadata& metadata,
                         AVFrame* frame)
{
    frame->format = contract.pixelFormat;
    frame->color_range = AVCOL_RANGE_JPEG;
    frame->colorspace = contract.colorSpace;
    frame->color_primaries = contract.primaries;
    frame->color_trc = contract.transfer;
    frame->chroma_location = metadata.chroma_siting == 0 ? AVCHROMA_LOC_CENTER : AVCHROMA_LOC_LEFT;
}

inline bool attachHdrMetadata(AVFrame* frame, const SS_HDR_METADATA& metadata)
{
    // Keep the same units and interpretation as the conventional FFmpeg path.
    // Empty host metadata is not evidence of a zero-nit mastering display.
    if ((metadata.maxDisplayLuminance != 0 || metadata.displayPrimaries[0].x != 0) &&
        av_frame_get_side_data(frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA) == nullptr) {
        auto* mastering = av_mastering_display_metadata_create_side_data(frame);
        if (mastering == nullptr) {
            return false;
        }

        for (int i = 0; i < 3; i++) {
            mastering->display_primaries[i][0] = av_make_q(metadata.displayPrimaries[i].x, 50000);
            mastering->display_primaries[i][1] = av_make_q(metadata.displayPrimaries[i].y, 50000);
        }
        mastering->white_point[0] = av_make_q(metadata.whitePoint.x, 50000);
        mastering->white_point[1] = av_make_q(metadata.whitePoint.y, 50000);
        mastering->min_luminance = av_make_q(metadata.minDisplayLuminance, 10000);
        mastering->max_luminance = av_make_q(metadata.maxDisplayLuminance, 1);
        mastering->has_luminance = metadata.maxDisplayLuminance != 0;
        mastering->has_primaries = metadata.displayPrimaries[0].x != 0;
    }

    if ((metadata.maxContentLightLevel != 0 || metadata.maxFrameAverageLightLevel != 0) &&
        av_frame_get_side_data(frame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL) == nullptr) {
        auto* light = av_content_light_metadata_create_side_data(frame);
        if (light == nullptr) {
            return false;
        }
        light->MaxCLL = metadata.maxContentLightLevel;
        light->MaxFALL = metadata.maxFrameAverageLightLevel;
    }
    return true;
}

}
