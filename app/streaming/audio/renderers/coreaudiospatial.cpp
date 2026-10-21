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

template <typename T>
bool setProperty(AudioUnit unit, AudioUnitPropertyID property, AudioUnitScope scope, const T& value)
{
    OSStatus status = AudioUnitSetProperty(unit, property, scope, 0, &value, sizeof(value));
    if (status != noErr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Unable to set spatial mixer property %u: %d",
                     (unsigned int)property,
                     (int)status);
        return false;
    }
    return true;
}

}

CoreAudioSpatialMixer::CoreAudioSpatialMixer()
    : m_Mixer(nullptr),
      m_ChannelCount(0),
      m_Input(nullptr)
{
    for (int i = 0; i < AUDIO_CONFIGURATION_MAX_CHANNEL_COUNT; i++) {
        m_ChannelMap[i] = i;
    }
}

CoreAudioSpatialMixer::~CoreAudioSpatialMixer()
{
    if (m_Mixer != nullptr) {
        AudioUnitUninitialize(m_Mixer);
        AudioComponentInstanceDispose(m_Mixer);
    }
}

bool CoreAudioSpatialMixer::initialize(AUSpatialMixerOutputType outputType, int sampleRate, int channelCount,
                                       UInt32 maxFrames, bool headTracking)
{
    // The host sends WAVE channel order: FL FR FC LFE BL BR SL SR (TFL TFR TBL TBR).
    // WAVE_5_1_B and WAVE_7_1 match it, but Atmos_7_1_4 puts the side pair
    // before the back pair, so 7.1.4 needs those two pairs swapped.
    AudioChannelLayoutTag layoutTag;
    switch (channelCount) {
    case 6:
        layoutTag = kAudioChannelLayoutTag_WAVE_5_1_B;
        break;
    case 8:
        layoutTag = kAudioChannelLayoutTag_WAVE_7_1;
        break;
    case 12:
        layoutTag = kAudioChannelLayoutTag_Atmos_7_1_4;
        m_ChannelMap[4] = 6;
        m_ChannelMap[5] = 7;
        m_ChannelMap[6] = 4;
        m_ChannelMap[7] = 5;
        break;
    default:
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

    AudioChannelLayout inputLayout = {};
    inputLayout.mChannelLayoutTag = layoutTag;
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
            !setProperty(m_Mixer, kAudioUnitProperty_AudioChannelLayout, kAudioUnitScope_Input, inputLayout) ||
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
        const float* in = me->m_Input + me->m_ChannelMap[channel];
        for (UInt32 frame = 0; frame < inNumberFrames; frame++) {
            out[frame] = in[frame * me->m_ChannelCount];
        }
    }

    return noErr;
}
