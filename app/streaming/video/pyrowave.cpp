#include "pyrowave.h"

#include "streaming/session.h"

#include <SDL.h>

#include <cstring>
#include <cmath>

extern "C" {
#include <libavutil/pixfmt.h>
}

PyrowaveVideoDecoder::PyrowaveVideoDecoder(bool testOnly)
    : m_TestOnly(testOnly), m_Width(0), m_Height(0), m_Device(nullptr), m_Decoder(nullptr),
      m_Renderer(nullptr), m_Pacer(nullptr), m_OverlayRegistered(false), m_DecoderThread(nullptr),
      m_LastFrameNumber(0), m_LastOverlayUpdateUs(0)
{
    SDL_zero(m_VideoStats);
    SDL_zero(m_LastOverlayStats);
    SDL_AtomicSet(&m_DecoderThreadShouldQuit, 0);
}

PyrowaveVideoDecoder::~PyrowaveVideoDecoder()
{
    if (m_DecoderThread != nullptr) {
        SDL_AtomicSet(&m_DecoderThreadShouldQuit, 1);
        LiWakeWaitForVideoFrame();
        SDL_WaitThread(m_DecoderThread, nullptr);
    }
    delete m_Pacer;
    m_Pacer = nullptr;

    if (m_OverlayRegistered && Session::get() != nullptr) {
        Session::get()->getOverlayManager().setOverlayRenderer(nullptr);
    }

    delete m_Renderer;
    m_Renderer = nullptr;

    if (m_Decoder != nullptr) {
        pyrowave_decoder_destroy(m_Decoder);
        m_Decoder = nullptr;
    }
    if (m_Device != nullptr) {
        pyrowave_device_destroy(m_Device);
        m_Device = nullptr;
    }
}

bool PyrowaveVideoDecoder::initialize(PDECODER_PARAMETERS params)
{
    if (params == nullptr || params->videoFormat != VIDEO_FORMAT_PYROWAVE) {
        return false;
    }

    if (!PyrowaveColor::describe(params->hdrMode, m_ColorContract)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Unsupported PyroWave HDR mode: %d",
                     params->hdrMode);
        return false;
    }

    if (params->width <= 0 || params->height <= 0 || params->width > 16384 ||
        params->height > 16384 || (params->width & 1) != 0 || (params->height & 1) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Invalid PyroWave dimensions: %dx%d",
                     params->width, params->height);
        return false;
    }

    m_Width = params->width;
    m_Height = params->height;

    if (pyrowave_create_default_device(&m_Device) != PYROWAVE_SUCCESS || m_Device == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Unable to create a Vulkan device for PyroWave");
        return false;
    }

    pyrowave_decoder_create_info decoderInfo = {};
    decoderInfo.device = m_Device;
    decoderInfo.width = m_Width;
    decoderInfo.height = m_Height;
    decoderInfo.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;
    decoderInfo.fragment_path =
        m_ColorContract.useFragmentPath(pyrowave_decoder_device_prefers_fragment_path(m_Device));

    if (pyrowave_decoder_create(&decoderInfo, &m_Decoder) != PYROWAVE_SUCCESS ||
        m_Decoder == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Unable to create the PyroWave decoder");
        return false;
    }

    m_Renderer = new PlVkRenderer(AV_HWDEVICE_TYPE_NONE);
    if (!m_Renderer->initialize(params)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Unable to initialize the Vulkan renderer for PyroWave");
        return false;
    }
    if (m_ColorContract.hdrMode != 0 && !isHdrSupported()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Vulkan renderer cannot handle PyroWave HDR");
        return false;
    }

    // Decoder probing creates a renderer only to validate the Vulkan path. Do
    // not start a Pacer thread or post render events until the decoder is used
    // for a live session.
    if (!m_TestOnly) {
        m_Pacer = new Pacer(m_Renderer, &m_VideoStats, &m_VideoStatsMutex);
        if (!m_Pacer->initialize(params->window, params->frameRate,
                                 params->enableFramePacing ||
                                     (params->enableVsync && (m_Renderer->getRendererAttributes() &
                                                              RENDERER_ATTRIBUTE_FORCE_PACING)))) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Unable to initialize the PyroWave frame pacer");
            return false;
        }
    }

    if (!m_TestOnly && Session::get() != nullptr) {
        Session::get()->getOverlayManager().setOverlayRenderer(m_Renderer);
        m_OverlayRegistered = true;
    }
    if (!m_TestOnly) {
        m_Renderer->prepareToRender();
        m_DecoderThread = SDL_CreateThread(decoderThreadProc, "PyrowaveDecoder", this);
        if (m_DecoderThread == nullptr) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Unable to create PyroWave decoder thread: %s", SDL_GetError());
            return false;
        }
    }
    return true;
}

bool PyrowaveVideoDecoder::isHardwareAccelerated()
{
    return true;
}

bool PyrowaveVideoDecoder::isAlwaysFullScreen()
{
    return (m_Renderer != nullptr) &&
           ((m_Renderer->getRendererAttributes() & RENDERER_ATTRIBUTE_FULLSCREEN_ONLY) != 0);
}

bool PyrowaveVideoDecoder::isHdrSupported()
{
    return m_Renderer != nullptr &&
           (m_Renderer->getRendererAttributes() & RENDERER_ATTRIBUTE_HDR_SUPPORT) != 0;
}

int PyrowaveVideoDecoder::getDecoderCapabilities()
{
    return CAPABILITY_PYROWAVE | CAPABILITY_PULL_RENDERER;
}

int PyrowaveVideoDecoder::decoderThreadProc(void* context)
{
    auto* decoder = static_cast<PyrowaveVideoDecoder*>(context);
    while (!SDL_AtomicGet(&decoder->m_DecoderThreadShouldQuit)) {
        VIDEO_FRAME_HANDLE handle;
        PDECODE_UNIT du;
        if (!LiWaitForNextVideoFrame(&handle, &du)) {
            continue;
        }
        Session::markVideoReceived();
        LiCompleteVideoFrame(handle, decoder->submitDecodeUnit(du));
    }
    return 0;
}

int PyrowaveVideoDecoder::getDecoderColorspace()
{
    // This preference also applies to legacy-codec fallback. common-c selects
    // BT.2020 separately once PyroWave HDR is negotiated, and frames carry it explicitly.
    return COLORSPACE_REC_709;
}

int PyrowaveVideoDecoder::getDecoderColorRange()
{
    return COLOR_RANGE_FULL;
}

QSize PyrowaveVideoDecoder::getDecoderMaxResolution()
{
    return QSize(16384, 16384);
}

bool PyrowaveVideoDecoder::copyDecodeUnit(PDECODE_UNIT du, QByteArray& payload) const
{
    if (du == nullptr || du->bufferList == nullptr || du->fullLength <= 0) {
        return false;
    }

    payload.resize(du->fullLength);
    int offset = 0;
    for (PLENTRY entry = du->bufferList; entry != nullptr; entry = entry->next) {
        if (entry->data == nullptr || entry->length <= 0 || offset < 0 ||
            entry->length > payload.size() - offset) {
            return false;
        }

        memcpy(payload.data() + offset, entry->data, static_cast<size_t>(entry->length));
        offset += entry->length;
    }

    return offset == payload.size();
}

bool PyrowaveVideoDecoder::createFrame(const pyrowave_color_metadata& metadata, AVFrame** frame)
{
    AVFrame* output = av_frame_alloc();
    if (output == nullptr) {
        return false;
    }

    PyrowaveColor::applyToFrame(m_ColorContract, metadata, output);
    output->width = m_Width;
    output->height = m_Height;
    if (av_frame_get_buffer(output, 32) < 0) {
        av_frame_free(&output);
        return false;
    }

    pyrowave_cpu_buffer buffers = {};
    buffers.data[0] = output->data[0];
    buffers.data[1] = output->data[1];
    buffers.data[2] = output->data[2];
    buffers.row_stride_in_bytes[0] = static_cast<size_t>(output->linesize[0]);
    buffers.row_stride_in_bytes[1] = static_cast<size_t>(output->linesize[1]);
    buffers.row_stride_in_bytes[2] = static_cast<size_t>(output->linesize[2]);
    buffers.plane_size_in_bytes[0] = buffers.row_stride_in_bytes[0] * static_cast<size_t>(m_Height);
    buffers.plane_size_in_bytes[1] =
        buffers.row_stride_in_bytes[1] * static_cast<size_t>((m_Height + 1) / 2);
    buffers.plane_size_in_bytes[2] =
        buffers.row_stride_in_bytes[2] * static_cast<size_t>((m_Height + 1) / 2);
    buffers.width = m_Width;
    buffers.height = m_Height;
    buffers.format = m_ColorContract.cpuFormat;

    if (pyrowave_decoder_decode_cpu_buffer_synchronous(m_Decoder, &buffers) != PYROWAVE_SUCCESS) {
        av_frame_free(&output);
        return false;
    }

    SS_HDR_METADATA hdrMetadata;
    if (m_ColorContract.hdrMode != 0 && LiGetHdrMetadata(&hdrMetadata) &&
        !PyrowaveColor::attachHdrMetadata(output, hdrMetadata)) {
        av_frame_free(&output);
        return false;
    }

    *frame = output;
    return true;
}

void PyrowaveVideoDecoder::updatePerformanceOverlay(uint64_t nowUs)
{
    if (m_LastOverlayUpdateUs != 0 && nowUs - m_LastOverlayUpdateUs < 1000000) {
        return;
    }

    VIDEO_STATS stats;
    {
        QMutexLocker locker(&m_VideoStatsMutex);
        stats = m_VideoStats;
    }

    // Establish the bandwidth and frame counter baselines on the first frame.
    const double bandwidthMbps = m_BandwidthMeter.update(LiGetRTPVideoBytesReceived(), nowUs);
    if (m_LastOverlayUpdateUs == 0) {
        m_LastOverlayUpdateUs = nowUs;
        m_LastOverlayStats = stats;
        return;
    }

    const double elapsedSeconds = static_cast<double>(nowUs - m_LastOverlayUpdateUs) / 1000000.0;
    const uint32_t received = stats.receivedFrames - m_LastOverlayStats.receivedFrames;
    const uint32_t decoded = stats.decodedFrames - m_LastOverlayStats.decodedFrames;
    const uint32_t rendered = stats.renderedFrames - m_LastOverlayStats.renderedFrames;
    const uint32_t total = stats.totalFrames - m_LastOverlayStats.totalFrames;
    const uint32_t lost = stats.networkDroppedFrames - m_LastOverlayStats.networkDroppedFrames;
    const uint32_t hostSamples =
        stats.framesWithHostProcessingLatency - m_LastOverlayStats.framesWithHostProcessingLatency;
    const uint64_t decodeTime = stats.totalDecodeTimeUs - m_LastOverlayStats.totalDecodeTimeUs;
    const uint64_t renderTime = stats.totalRenderTimeUs - m_LastOverlayStats.totalRenderTimeUs;
    const uint32_t hostTime =
        stats.totalHostProcessingLatency - m_LastOverlayStats.totalHostProcessingLatency;

    if (Session::get() != nullptr &&
        Session::get()->getOverlayManager().isOverlayEnabled(Overlay::OverlayDebug)) {
        uint32_t rtt = 0, variance = 0;
        char rttString[32];
        char bandwidthString[32];
        char decodeString[32];
        char renderString[32];
        char encodeString[48] = {};
        if (LiGetEstimatedRttInfo(&rtt, &variance)) {
            SDL_snprintf(rttString, sizeof(rttString), "**%u** ± %ums", rtt, variance);
        } else {
            SDL_snprintf(rttString, sizeof(rttString), "N/A");
        }
        if (!std::isnan(bandwidthMbps)) {
            SDL_snprintf(bandwidthString, sizeof(bandwidthString), "**%.2f** {16}Mbps{18}",
                         bandwidthMbps);
        } else {
            SDL_snprintf(bandwidthString, sizeof(bandwidthString), "N/A");
        }
        if (decoded != 0) {
            SDL_snprintf(decodeString, sizeof(decodeString), "**%.2f**ms",
                         static_cast<double>(decodeTime) / decoded / 1000.0);
        } else {
            SDL_snprintf(decodeString, sizeof(decodeString), "N/A");
        }
        if (rendered != 0) {
            SDL_snprintf(renderString, sizeof(renderString), "**%.2f**ms",
                         static_cast<double>(renderTime) / rendered / 1000.0);
        } else {
            SDL_snprintf(renderString, sizeof(renderString), "N/A");
        }
        if (hostSamples != 0) {
            SDL_snprintf(encodeString, sizeof(encodeString), "· Encode **%.1f**ms ",
                         static_cast<double>(hostTime) / 10.0 / hostSamples);
        }
        char text[512];
        SDL_snprintf(text, sizeof(text),
                     " {18}%dx%d@%.0f PyroWave %s "
                     " {18}FPS  %.1f {14}Rx{18} · %.1f {14}De{18} · %.1f {14}Rd{18} \n"
                     " Network %s Loss %.2f%% Bandwidth %s {16}|  {18}Render %s · Decode %s %s",
                     m_Width, m_Height, total / elapsedSeconds, m_ColorContract.label,
                     received / elapsedSeconds, decoded / elapsedSeconds, rendered / elapsedSeconds,
                     rttString, total != 0 ? 100.0 * lost / total : 0.0, bandwidthString,
                     renderString, decodeString, encodeString);
        Session::get()->getOverlayManager().updateOverlayText(Overlay::OverlayDebug, text);
    }

    m_LastOverlayUpdateUs = nowUs;
    m_LastOverlayStats = stats;
}

int PyrowaveVideoDecoder::submitDecodeUnit(PDECODE_UNIT du)
{
    if (du == nullptr || du->frameType != FRAME_TYPE_IDR || m_Pacer == nullptr) {
        return DR_NEED_IDR;
    }

    {
        QMutexLocker locker(&m_VideoStatsMutex);
        if (m_LastFrameNumber != 0 && du->frameNumber > m_LastFrameNumber + 1) {
            m_VideoStats.networkDroppedFrames += du->frameNumber - (m_LastFrameNumber + 1);
            m_VideoStats.totalFrames += du->frameNumber - (m_LastFrameNumber + 1);
        }
        m_LastFrameNumber = du->frameNumber;
        m_VideoStats.receivedFrames++;
        m_VideoStats.totalFrames++;
        if (du->frameHostProcessingLatency != 0) {
            m_VideoStats.framesWithHostProcessingLatency++;
            m_VideoStats.totalHostProcessingLatency += du->frameHostProcessingLatency;
        }
    }

    QByteArray payload;
    if (!copyDecodeUnit(du, payload)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Invalid PyroWave decode unit for frame %d",
                    du->frameNumber);
        updatePerformanceOverlay(LiGetMicroseconds());
        return DR_NEED_IDR;
    }

    // Every transport decode unit is a complete independent frame. Discard
    // any state left behind by a malformed or incomplete previous frame.
    pyrowave_decoder_clear(m_Decoder);
    if (pyrowave_decoder_push_packet(m_Decoder, payload.constData(),
                                     static_cast<size_t>(payload.size())) != PYROWAVE_SUCCESS ||
        !pyrowave_decoder_decode_is_ready(m_Decoder, false)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "PyroWave rejected frame %d", du->frameNumber);
        updatePerformanceOverlay(LiGetMicroseconds());
        return DR_NEED_IDR;
    }

    pyrowave_color_metadata metadata = {};
    if (!pyrowave_decoder_get_color_metadata(m_Decoder, &metadata) ||
        !PyrowaveColor::matches(m_ColorContract, metadata)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "PyroWave frame %d does not match negotiated %s full-range colors "
                    "(primaries=%d transfer=%d matrix=%d range=%d siting=%u)",
                    du->frameNumber, m_ColorContract.label, static_cast<int>(metadata.primaries),
                    static_cast<int>(metadata.transfer), static_cast<int>(metadata.transform),
                    static_cast<int>(metadata.range), metadata.chroma_siting);
        updatePerformanceOverlay(LiGetMicroseconds());
        return DR_NEED_IDR;
    }

    AVFrame* frame = nullptr;
    if (!createFrame(metadata, &frame)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "PyroWave failed to decode frame %d",
                    du->frameNumber);
        updatePerformanceOverlay(LiGetMicroseconds());
        return DR_NEED_IDR;
    }

    frame->pts = static_cast<int64_t>(du->rtpTimestamp);
    frame->pkt_dts = static_cast<int64_t>(LiGetMicroseconds());

    {
        QMutexLocker locker(&m_VideoStatsMutex);
        m_VideoStats.decodedFrames++;
        m_VideoStats.totalDecodeTimeUs += LiGetMicroseconds() - du->enqueueTimeUs;
    }

    m_Pacer->submitFrame(frame);
    updatePerformanceOverlay(LiGetMicroseconds());
    return DR_OK;
}

void PyrowaveVideoDecoder::renderFrameOnMainThread()
{
    if (m_Pacer != nullptr) {
        m_Pacer->renderOnMainThread();
    }
}

void PyrowaveVideoDecoder::setHdrMode(bool enabled)
{
    if (m_Renderer != nullptr) {
        m_Renderer->setHdrMode(enabled);
    }
}

bool PyrowaveVideoDecoder::notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO info)
{
    return m_Renderer != nullptr && m_Renderer->notifyWindowChanged(info);
}
