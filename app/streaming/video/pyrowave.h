#pragma once

#include "decoder.h"
#include "../bandwidth.h"

#include "ffmpeg-renderers/pacer/pacer.h"
#include "ffmpeg-renderers/plvk.h"

#include <QByteArray>
#include <QMutex>

extern "C" {
#include <libavutil/frame.h>
}

#include <pyrowave.h>

// PyroWave is an optional experimental decoder.  The current Moonlight
// integration deliberately starts with the stable 8-bit YUV420 SDR contract;
// HDR and 4:4:4 remain on the negotiated legacy codec paths until their
// application-side color contract is wired through this class.
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
    bool createFrame(AVFrame** frame);
    bool copyDecodeUnit(PDECODE_UNIT du, QByteArray& payload) const;
    void updatePerformanceOverlay(uint64_t nowUs);

    bool m_TestOnly;
    int m_Width;
    int m_Height;
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
