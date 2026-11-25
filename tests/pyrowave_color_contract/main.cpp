#include "streaming/video/pyrowavecolor.h"

#include <climits>
#include <cstdio>
#include <cstring>
#include <initializer_list>

extern "C" {
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
}

static int failures = 0;

static bool check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
    return condition;
}

static void checkColorModes()
{
    const AVColorTransferCharacteristic transfers[] = { AVCOL_TRC_BT709, AVCOL_TRC_SMPTE2084,
                                                        AVCOL_TRC_ARIB_STD_B67 };
    const char* labels[] = { "SDR", "HDR10", "HLG" };
    for (int mode = 0; mode <= 2; mode++) {
        PyrowaveColor::Contract color;
        check(PyrowaveColor::describe(mode, color), "negotiated mode is supported");
        check(color.hdrMode == mode && std::strcmp(color.label, labels[mode]) == 0,
              "mode and HUD label agree");
        check(color.cpuFormat == (mode == 0 ? PYROWAVE_CPU_BUFFER_FORMAT_YUV420P
                                            : PYROWAVE_CPU_BUFFER_FORMAT_YUV420P10),
              "HDR readback retains ten bits");
        check(color.useFragmentPath(true) == (mode == 0),
              "HDR avoids unsupported fragment P10 conversion");
        check(!color.useFragmentPath(false), "compute preference is preserved");
        check(PyrowaveColor::matches(color, color.metadata), "matching bitstream is accepted");

        auto wrong = color.metadata;
        wrong.primaries =
            mode == 0 ? PYROWAVE_COLOR_PRIMARIES_BT2020 : PYROWAVE_COLOR_PRIMARIES_BT709;
        check(!PyrowaveColor::matches(color, wrong), "mismatched primaries are rejected");
        wrong = color.metadata;
        wrong.transfer = mode == 1 ? PYROWAVE_TRANSFER_HLG : PYROWAVE_TRANSFER_PQ;
        check(!PyrowaveColor::matches(color, wrong), "PQ/HLG/SDR transfer mismatch is rejected");
        wrong = color.metadata;
        wrong.transform = mode == 0 ? PYROWAVE_YCBCR_BT2020 : PYROWAVE_YCBCR_BT709;
        check(!PyrowaveColor::matches(color, wrong), "mismatched matrix is rejected");
        wrong = color.metadata;
        wrong.range = PYROWAVE_YCBCR_LIMITED;
        check(!PyrowaveColor::matches(color, wrong), "unnegotiated limited range is rejected");
        wrong = color.metadata;
        wrong.chroma_siting = 2;
        check(!PyrowaveColor::matches(color, wrong), "unknown chroma siting is rejected");

        AVFrame* frame = av_frame_alloc();
        if (!check(frame != nullptr, "frame allocation")) {
            continue;
        }
        PyrowaveColor::applyToFrame(color, color.metadata, frame);
        const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(color.pixelFormat);
        check(descriptor != nullptr && descriptor->comp[0].depth == (mode == 0 ? 8 : 10),
              "AVFrame pixel format matches readback precision");
        check(frame->format == color.pixelFormat && frame->color_range == AVCOL_RANGE_JPEG,
              "AVFrame uses the negotiated full-range pixel format");
        check(frame->colorspace == (mode == 0 ? AVCOL_SPC_BT709 : AVCOL_SPC_BT2020_NCL) &&
                  frame->color_primaries == (mode == 0 ? AVCOL_PRI_BT709 : AVCOL_PRI_BT2020) &&
                  frame->color_trc == transfers[mode],
              "renderer receives the matching matrix, primaries and transfer");
        check(frame->chroma_location == AVCHROMA_LOC_CENTER, "center chroma is signalled");
        auto left = color.metadata;
        left.chroma_siting = 1;
        check(PyrowaveColor::matches(color, left), "valid left chroma siting is accepted");
        PyrowaveColor::applyToFrame(color, left, frame);
        check(frame->chroma_location == AVCHROMA_LOC_LEFT, "left chroma is signalled");

        frame->width = 64;
        frame->height = 4;
        if (check(av_frame_get_buffer(frame, 32) == 0, "actual readback frame allocation")) {
            const uint8_t* planes[4] = { frame->data[0], frame->data[1], frame->data[2], nullptr };
            for (int plane = 0; plane < 3; plane++) {
                const int planeWidth = plane == 0 ? frame->width : frame->width / 2;
                check(frame->linesize[plane] >= planeWidth * (mode == 0 ? 1 : 2),
                      "readback stride accommodates each sample");
                const uint16_t value = mode == 0 ? 255 : 1023;
                if (mode == 0) {
                    frame->data[plane][0] = static_cast<uint8_t>(value);
                } else {
                    std::memcpy(frame->data[plane], &value, sizeof(value));
                }
                uint16_t decoded = 0;
                av_read_image_line(&decoded, planes, frame->linesize, descriptor, 0, 0, plane, 1,
                                   0);
                check(decoded == value, "native-endian P10 readback retains the full 1023 value");
            }
        }
        av_frame_free(&frame);
    }

    PyrowaveColor::Contract invalid;
    check(!PyrowaveColor::describe(-1, invalid) && !PyrowaveColor::describe(3, invalid) &&
              !PyrowaveColor::describe(INT_MAX, invalid),
          "unknown HDR modes cannot silently become SDR");
}

static void checkStaticMetadata()
{
    AVFrame* frame = av_frame_alloc();
    if (!check(frame != nullptr, "metadata frame allocation")) {
        return;
    }
    SS_HDR_METADATA metadata = {};
    check(PyrowaveColor::attachHdrMetadata(frame, metadata), "absent mastering data is allowed");
    check(frame->nb_side_data == 0, "absent metadata does not invent a zero-nit display");

    metadata.displayPrimaries[0] = { 35400, 14600 };
    metadata.displayPrimaries[1] = { 8500, 39850 };
    metadata.displayPrimaries[2] = { 6550, 2300 };
    metadata.whitePoint = { 15635, 16450 };
    metadata.minDisplayLuminance = 5;
    metadata.maxDisplayLuminance = 1000;
    metadata.maxContentLightLevel = 900;
    metadata.maxFrameAverageLightLevel = 350;
    check(PyrowaveColor::attachHdrMetadata(frame, metadata), "static metadata is attached");
    auto* masteringSideData =
        av_frame_get_side_data(frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
    auto* lightSideData = av_frame_get_side_data(frame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
    if (check(masteringSideData != nullptr, "mastering side data exists")) {
        const auto* mastering =
            reinterpret_cast<AVMasteringDisplayMetadata*>(masteringSideData->data);
        check(mastering->display_primaries[0][0].num == 35400 &&
                  mastering->display_primaries[0][0].den == 50000 &&
                  mastering->white_point[1].num == 16450 && mastering->white_point[1].den == 50000,
              "chromaticities preserve the host units");
        check(mastering->min_luminance.num == 5 && mastering->min_luminance.den == 10000 &&
                  mastering->max_luminance.num == 1000 && mastering->max_luminance.den == 1 &&
                  mastering->has_luminance && mastering->has_primaries,
              "mastering luminance and validity flags are correct");
        metadata.maxDisplayLuminance = 2000;
        check(PyrowaveColor::attachHdrMetadata(frame, metadata) &&
                  mastering->max_luminance.num == 1000 && frame->nb_side_data == 2,
              "existing frame metadata takes precedence");
    }
    if (check(lightSideData != nullptr, "content-light side data exists")) {
        const auto* light = reinterpret_cast<AVContentLightMetadata*>(lightSideData->data);
        check(light->MaxCLL == 900 && light->MaxFALL == 350, "content light levels are preserved");
    }
    av_frame_free(&frame);

    // Exercise both allocation-failure branches using libavutil's real allocator.
    for (bool contentLightOnly : { false, true }) {
        frame = av_frame_alloc();
        if (!check(frame != nullptr, "allocation-failure test frame")) {
            continue;
        }
        SS_HDR_METADATA failingMetadata = {};
        if (contentLightOnly) {
            failingMetadata.maxContentLightLevel = 900;
        } else {
            failingMetadata.maxDisplayLuminance = 1000;
        }
        av_max_alloc(1);
        const bool attached = PyrowaveColor::attachHdrMetadata(frame, failingMetadata);
        av_max_alloc(INT_MAX);
        check(!attached, "side-data allocation failure is returned without dereferencing NULL");
        av_frame_free(&frame);
    }
}

int main()
{
    checkColorModes();
    checkStaticMetadata();
    if (failures == 0) {
        std::puts("pyrowave_color_contract=passed");
    }
    return failures == 0 ? 0 : 1;
}
