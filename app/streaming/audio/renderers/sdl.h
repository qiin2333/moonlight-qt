#pragma once

#include "renderer.h"
#include "SDL_compat.h"

class SdlAudioRenderer : public IAudioRenderer
{
public:
    SdlAudioRenderer();

    virtual ~SdlAudioRenderer();

    virtual bool prepareForPlayback(const OPUS_MULTISTREAM_CONFIGURATION* opusConfig);

    virtual void* getAudioBuffer(int* size);

    virtual bool submitAudio(int bytesWritten);

    virtual int getCapabilities();

    virtual AudioFormat getAudioBufferFormat();

    virtual const char* getRendererName() { return m_Name; }

private:
    SDL_AudioDeviceID m_AudioDevice;
    void* m_AudioBuffer;
    Uint32 m_FrameSize;
    Uint32 m_FrameDurationMs;
    char m_Name[24];
};
