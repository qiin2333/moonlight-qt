#include "coreaudiospatial.h"

#include "SDL_compat.h"

#include <AudioToolbox/AudioToolbox.h>

namespace {

AudioStreamBasicDescription nonInterleavedFloatFormat(int sampleRate, int channelCount)
{
    AudioStreamBasicDescription asbd = {};
    asbd.mSampleRate = sampleRate;
    asbd.mFormatID = kAudioFormatLinearPCM;
    asbd.mFormatFlags = kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved;
    asbd.mChannelsPerFrame = channelCount;
    asbd.mBitsPerChannel = 32;
    asbd.mFramesPerPacket = 1;
    asbd.mBytesPerFrame = sizeof(float);
    asbd.mBytesPerPacket = sizeof(float);
    return asbd;
}

bool setProperty(AudioUnit unit, AudioUnitPropertyID property, AudioUnitScope scope, const void* data, UInt32 size)
{
    OSStatus status = AudioUnitSetProperty(unit, property, scope, 0, data, size);
    if (status != noErr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Unable to set spatial mixer property %u: %d",
                     (unsigned int)property,
                     (int)status);
        return false;
    }
    return true;
}

template <typename T>
bool setProperty(AudioUnit unit, AudioUnitPropertyID property, AudioUnitScope scope, const T& value)
{
    return setProperty(unit, property, scope, &value, sizeof(value));
}

}

CoreAudioSpatialMixer::CoreAudioSpatialMixer()
    : m_Mixer(nullptr),
      m_ChannelCount(0),
      m_Input(nullptr)
{
}

CoreAudioSpatialMixer::~CoreAudioSpatialMixer()
{
    if (m_Mixer != nullptr) {
        AudioUnitUninitialize(m_Mixer);
        AudioComponentInstanceDispose(m_Mixer);
    }
}

bool CoreAudioSpatialMixer::initialize(AUSpatialMixerOutputType outputType, int sampleRate, int channelCount,
                                       const std::vector<unsigned char>& inputLayout,
                                       UInt32 maxFrames, bool headTracking)
{
    if (inputLayout.empty()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Spatial audio does not support %d channels",
                     channelCount);
        return false;
    }

    m_ChannelCount = channelCount;

    AudioComponentDescription desc = {};
    desc.componentType = kAudioUnitType_Mixer;
    desc.componentSubType = kAudioUnitSubType_SpatialMixer;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;

    AudioComponent component = AudioComponentFindNext(nullptr, &desc);
    if (component == nullptr || AudioComponentInstanceNew(component, &m_Mixer) != noErr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Unable to create the spatial mixer audio unit");
        m_Mixer = nullptr;
        return false;
    }

    AudioChannelLayout outputLayout = {};
    outputLayout.mChannelLayoutTag = kAudioChannelLayoutTag_Stereo;

    AURenderCallbackStruct callback = {};
    callback.inputProc = inputCallback;
    callback.inputProcRefCon = this;

    // UseOutputType picks the best rendering for the output type, and
    // AmbienceBed places each input channel around the listener like a speaker.
    if (!setProperty(m_Mixer, kAudioUnitProperty_ElementCount, kAudioUnitScope_Input, (UInt32)1) ||
            !setProperty(m_Mixer, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input,
                         nonInterleavedFloatFormat(sampleRate, channelCount)) ||
            !setProperty(m_Mixer, kAudioUnitProperty_AudioChannelLayout, kAudioUnitScope_Input,
                         inputLayout.data(), (UInt32)inputLayout.size()) ||
            !setProperty(m_Mixer, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output,
                         nonInterleavedFloatFormat(sampleRate, 2)) ||
            !setProperty(m_Mixer, kAudioUnitProperty_AudioChannelLayout, kAudioUnitScope_Output, outputLayout) ||
            !setProperty(m_Mixer, kAudioUnitProperty_SpatializationAlgorithm, kAudioUnitScope_Input,
                         (UInt32)kSpatializationAlgorithm_UseOutputType) ||
            !setProperty(m_Mixer, kAudioUnitProperty_SpatialMixerSourceMode, kAudioUnitScope_Input,
                         (UInt32)kSpatialMixerSourceMode_AmbienceBed) ||
            !setProperty(m_Mixer, kAudioUnitProperty_SpatialMixerOutputType, kAudioUnitScope_Global,
                         (UInt32)outputType) ||
            !setProperty(m_Mixer, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, maxFrames) ||
            !setProperty(m_Mixer, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, callback)) {
        return false;
    }

    // Both only take effect when rendering for headphones. Neither is fatal:
    // the mixer falls back to the generic HRTF without head tracking.
    if (outputType == kSpatialMixerOutputType_Headphones) {
        setProperty(m_Mixer, kAudioUnitProperty_SpatialMixerPersonalizedHRTFMode, kAudioUnitScope_Global,
                    (UInt32)kSpatialMixerPersonalizedHRTFMode_Auto);
        if (headTracking &&
                !setProperty(m_Mixer, kAudioUnitProperty_SpatialMixerEnableHeadTracking, kAudioUnitScope_Global,
                             (UInt32)1)) {
            headTracking = false;
        }
    }
    else {
        headTracking = false;
    }

    OSStatus status = AudioUnitInitialize(m_Mixer);
    if (status != noErr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Unable to initialize the spatial mixer: %d",
                     (int)status);
        return false;
    }

    // Personalized HRTF is opportunistic, so ask whether we actually got it
    UInt32 personalizedHrtf = 0;
    if (__builtin_available(macOS 14.0, *)) {
        UInt32 size = sizeof(personalizedHrtf);
        AudioUnitGetProperty(m_Mixer, kAudioUnitProperty_SpatialMixerAnyInputIsUsingPersonalizedHRTF,
                             kAudioUnitScope_Global, 0, &personalizedHrtf, &size);
    }

    Float64 latency = 0.0;
    UInt32 size = sizeof(latency);
    AudioUnitGetProperty(m_Mixer, kAudioUnitProperty_Latency, kAudioUnitScope_Global, 0, &latency, &size);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Spatial mixer ready: %d channels, output type %u, personalized HRTF %s, head tracking %s, latency %.1f ms",
                channelCount,
                (unsigned int)outputType,
                personalizedHrtf ? "on" : "off",
                headTracking ? "on" : "off",
                latency * 1000.0);
    return true;
}

OSStatus CoreAudioSpatialMixer::render(const float* input,
                                       UInt32 frames,
                                       AudioUnitRenderActionFlags* ioActionFlags,
                                       const AudioTimeStamp* inTimeStamp,
                                       AudioBufferList* output)
{
    // AudioUnitRender() calls inputCallback() synchronously, so this is all
    // it needs to find the input
    m_Input = input;
    return AudioUnitRender(m_Mixer, ioActionFlags, inTimeStamp, 0, frames, output);
}

OSStatus CoreAudioSpatialMixer::inputCallback(void* inRefCon,
                                              AudioUnitRenderActionFlags*,
                                              const AudioTimeStamp*,
                                              UInt32,
                                              UInt32 inNumberFrames,
                                              AudioBufferList* ioData)
{
    auto me = static_cast<CoreAudioSpatialMixer*>(inRefCon);

    if (ioData->mNumberBuffers != (UInt32)me->m_ChannelCount) {
        return kAudioUnitErr_FormatNotSupported;
    }

    for (int channel = 0; channel < me->m_ChannelCount; channel++) {
        float* out = static_cast<float*>(ioData->mBuffers[channel].mData);
        const float* in = me->m_Input + channel;
        for (UInt32 frame = 0; frame < inNumberFrames; frame++) {
            out[frame] = in[frame * me->m_ChannelCount];
        }
    }

    return noErr;
}
