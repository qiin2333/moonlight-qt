#pragma once

#include <Limelight.h>

#include <AudioUnit/AudioUnit.h>

#include <vector>

// Renders a 5.1, 7.1 or 7.1.4 stream to stereo with Apple's AUSpatialMixer,
// binaurally (with optional head tracking) for headphones.
class CoreAudioSpatialMixer
{
public:
    CoreAudioSpatialMixer();

    ~CoreAudioSpatialMixer();

    // inputLayout holds an AudioChannelLayout describing the stream's channel order
    bool initialize(AUSpatialMixerOutputType outputType, int sampleRate, int channelCount,
                    const std::vector<unsigned char>& inputLayout,
                    UInt32 maxFrames, bool headTracking);

    // Renders interleaved input into the non-interleaved stereo buffers in output.
    // Called on the CoreAudio I/O thread.
    OSStatus render(const float* input,
                    UInt32 frames,
                    AudioUnitRenderActionFlags* ioActionFlags,
                    const AudioTimeStamp* inTimeStamp,
                    AudioBufferList* output);

private:
    static OSStatus inputCallback(void* inRefCon,
                                  AudioUnitRenderActionFlags* ioActionFlags,
                                  const AudioTimeStamp* inTimeStamp,
                                  UInt32 inBusNumber,
                                  UInt32 inNumberFrames,
                                  AudioBufferList* ioData);

    AudioUnit m_Mixer;
    int m_ChannelCount;
    const float* m_Input;
};
