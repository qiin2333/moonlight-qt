#pragma once

#include "decoder.h"
#include "../bandwidth.h"

#include "ffmpeg-renderers/pacer/pacer.h"
#include "ffmpeg-renderers/plvk.h"
#include "pyrowavecolor.h"

#include <QByteArray>
#include <QMutex>

// PyroWave is an optional experimental decoder for full-range YUV420:
// 8-bit BT.709 SDR or 10-bit BT.2020 PQ/HLG. Frames currently use CPU readback.
class PyrowaveVideoDecoder : public IVideoDecoder
{
public:
    explicit PyrowaveVideoDecoder(bool testOnly);
    ~PyrowaveVideoDecoder() override;

    bool initialize(PDECODER_PARAMETERS params) override;
    bool isHardwareAccelerated() override;
    bool isAlwaysFullScreen() override;
    bool isHdrSupported() override;
    int getDecoderCapabilities() override;
    int getDecoderColorspace() override;
    int getDecoderColorRange() override;
    QSize getDecoderMaxResolution() override;
    int submitDecodeUnit(PDECODE_UNIT du) override;
    void renderFrameOnMainThread() override;
    void setHdrMode(bool enabled) override;
    bool notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO info) override;

private:
    static int decoderThreadProc(void* context);
    bool createFrame(const pyrowave_color_metadata& metadata, AVFrame** frame);
    bool copyDecodeUnit(PDECODE_UNIT du, QByteArray& payload) const;
    void updatePerformanceOverlay(uint64_t nowUs);

    bool m_TestOnly;
    int m_Width;
    int m_Height;
    PyrowaveColor::Contract m_ColorContract;
    pyrowave_device m_Device;
    pyrowave_decoder m_Decoder;
    PlVkRenderer* m_Renderer;
    Pacer* m_Pacer;
    bool m_OverlayRegistered;
    SDL_Thread* m_DecoderThread;
    SDL_atomic_t m_DecoderThreadShouldQuit;
    VIDEO_STATS m_VideoStats;
    VIDEO_STATS m_LastOverlayStats;
    QMutex m_VideoStatsMutex;
    BandwidthMeter m_BandwidthMeter;
    int m_LastFrameNumber;
    uint64_t m_LastOverlayUpdateUs;
};
