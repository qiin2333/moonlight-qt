#include "coreaudio.h"

#include "settings/streamingpreferences.h"

#include "SDL_compat.h"

#include <IOKit/audio/IOAudioTypes.h>
#include <QString>

namespace {

// The same 30 ms the SDL renderer allows to build up in Moonlight's own queue
constexpr int RingBufferDurationMs = 30;

constexpr AudioObjectPropertyAddress DefaultDeviceAddress = {
    kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
};

// The data source changes when headphones are plugged into the built-in jack,
// which changes how spatial audio has to be rendered
constexpr AudioObjectPropertyAddress DeviceAddresses[] = {
    { kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
    { kAudioDevicePropertyDataSource, kAudioDevicePropertyScopeOutput, kAudioObjectPropertyElementMain },
};

template <typename T>
bool getDeviceProperty(AudioObjectID object, AudioObjectPropertySelector selector,
                       AudioObjectPropertyScope scope, T& out)
{
    AudioObjectPropertyAddress addr{ selector, scope, kAudioObjectPropertyElementMain };
    UInt32 size = sizeof(out);
    return AudioObjectGetPropertyData(object, &addr, 0, nullptr, &size, &out) == noErr;
}

QString deviceName(AudioDeviceID device)
{
    CFStringRef value = nullptr;
    if (!getDeviceProperty(device, kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, value) ||
            value == nullptr) {
        return QString();
    }

    QString name = QString::fromCFString(value);
    CFRelease(value);
    return name;
}

UInt32 outputChannelCount(AudioDeviceID device)
{
    AudioObjectPropertyAddress addr{ kAudioDevicePropertyStreamConfiguration,
                                     kAudioDevicePropertyScopeOutput,
                                     kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(device, &addr, 0, nullptr, &size) != noErr || size == 0) {
        return 0;
    }

    std::vector<unsigned char> storage(size);
    auto list = reinterpret_cast<AudioBufferList*>(storage.data());
    if (AudioObjectGetPropertyData(device, &addr, 0, nullptr, &size, list) != noErr) {
        return 0;
    }

    UInt32 channels = 0;
    for (UInt32 i = 0; i < list->mNumberBuffers; i++) {
        channels += list->mBuffers[i].mNumberChannels;
    }
    return channels;
}

}

CoreAudioRenderer::CoreAudioRenderer()
    : m_OutputUnit(nullptr),
      m_OutputDevice(kAudioObjectUnknown),
      m_ListenersAdded(false),
      m_NeedsReinit(false),
      m_FrameSize(0),
      m_SpatialMixer(nullptr)
{
}

CoreAudioRenderer::~CoreAudioRenderer()
{
    // Stop the I/O thread before tearing down anything it reads
    if (m_OutputUnit != nullptr) {
        AudioOutputUnitStop(m_OutputUnit);
        AudioUnitUninitialize(m_OutputUnit);
        AudioComponentInstanceDispose(m_OutputUnit);
    }

    removeListeners();
    delete m_SpatialMixer;
}

bool CoreAudioRenderer::prepareForPlayback(const OPUS_MULTISTREAM_CONFIGURATION* opusConfig)
{
    StreamingPreferences* prefs = StreamingPreferences::get();

    if (!getDeviceProperty(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice,
                           kAudioObjectPropertyScopeGlobal, m_OutputDevice) ||
            m_OutputDevice == kAudioObjectUnknown) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "No default audio output device");
        return false;
    }

    // Listen before reading the device, so a change during setup isn't missed
    addListeners();

    UInt32 deviceChannels = outputChannelCount(m_OutputDevice);
    AUSpatialMixerOutputType outputType = getSpatialMixerOutputType();

    // Surround is spatialized unless it can go straight to real surround speakers.
    // A 2-channel USB DAC claims to be external speakers, so the channel count
    // decides too.
    bool spatial = opusConfig->channelCount > 2 &&
                   (outputType != kSpatialMixerOutputType_ExternalSpeakers ||
                    deviceChannels < (UInt32)opusConfig->channelCount);

    if (spatial && prefs->spatialAudioConfig == StreamingPreferences::SAC_DISABLED) {
        if (deviceChannels < (UInt32)opusConfig->channelCount) {
            // AUHAL drops channels the device doesn't have instead of downmixing
            // them, so let the SDL renderer handle this as it did before
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Spatial audio is disabled and the output device has only %u channels",
                        deviceChannels);
            return false;
        }
        spatial = false;
    }

    m_FrameSize = opusConfig->channelCount * sizeof(float);

    if (!openOutputUnit()) {
        return false;
    }

    // Ask for a device buffer of one packet to keep latency down. The device
    // may not allow it, which only costs latency.
    Float64 deviceRate = 0.0;
    AudioValueRange range = {};
    if (getDeviceProperty(m_OutputDevice, kAudioDevicePropertyNominalSampleRate,
                          kAudioObjectPropertyScopeGlobal, deviceRate) &&
            getDeviceProperty(m_OutputDevice, kAudioDevicePropertyBufferFrameSizeRange,
                              kAudioObjectPropertyScopeOutput, range)) {
        Float64 packetFrames = opusConfig->samplesPerFrame * deviceRate / opusConfig->sampleRate;
        UInt32 bufferFrames = (UInt32)qBound(range.mMinimum, packetFrames, range.mMaximum);
        AudioObjectPropertyAddress addr{ kAudioDevicePropertyBufferFrameSize,
                                         kAudioObjectPropertyScopeOutput,
                                         kAudioObjectPropertyElementMain };
        if (AudioObjectSetPropertyData(m_OutputDevice, &addr, 0, nullptr,
                                       sizeof(bufferFrames), &bufferFrames) != noErr) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Unable to set the audio device buffer to %u frames",
                        bufferFrames);
        }
    }

    // The spatial mixer outputs non-interleaved stereo, otherwise we pass the
    // decoder's interleaved output straight through
    AudioStreamBasicDescription asbd = {};
    asbd.mSampleRate = opusConfig->sampleRate;
    asbd.mFormatID = kAudioFormatLinearPCM;
    asbd.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;
    asbd.mBitsPerChannel = 32;
    asbd.mFramesPerPacket = 1;
    if (spatial) {
        asbd.mFormatFlags |= kAudioFormatFlagIsNonInterleaved;
        asbd.mChannelsPerFrame = 2;
        asbd.mBytesPerFrame = sizeof(float);
    }
    else {
        asbd.mChannelsPerFrame = opusConfig->channelCount;
        asbd.mBytesPerFrame = m_FrameSize;
    }
    asbd.mBytesPerPacket = asbd.mBytesPerFrame;

    AURenderCallbackStruct callback = {};
    callback.inputProc = renderCallback;
    callback.inputProcRefCon = this;

    OSStatus status;
    if ((status = AudioUnitSetProperty(m_OutputUnit, kAudioUnitProperty_StreamFormat,
                                       kAudioUnitScope_Input, 0, &asbd, sizeof(asbd))) != noErr ||
            (status = AudioUnitSetProperty(m_OutputUnit, kAudioUnitProperty_SetRenderCallback,
                                           kAudioUnitScope_Input, 0, &callback, sizeof(callback))) != noErr ||
            (status = AudioUnitInitialize(m_OutputUnit)) != noErr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Unable to configure the CoreAudio output unit: %d",
                     (int)status);
        return false;
    }

    UInt32 maxFrames = 0;
    UInt32 size = sizeof(maxFrames);
    if (AudioUnitGetProperty(m_OutputUnit, kAudioUnitProperty_MaximumFramesPerSlice,
                             kAudioUnitScope_Global, 0, &maxFrames, &size) != noErr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Unable to get the CoreAudio output unit's maximum frames per slice");
        return false;
    }

    if (spatial) {
        m_SpatialMixer = new CoreAudioSpatialMixer();
        if (!m_SpatialMixer->initialize(outputType, opusConfig->sampleRate, opusConfig->channelCount,
                                        maxFrames, prefs->spatialHeadTracking)) {
            return false;
        }

        m_RenderBuffer.resize(maxFrames * m_FrameSize);
    }

    m_PacketBuffer.resize(opusConfig->samplesPerFrame * m_FrameSize);
    m_RingBuffer.init(qMax(opusConfig->sampleRate / 1000 * RingBufferDurationMs,
                           opusConfig->samplesPerFrame * 2) * m_FrameSize);

    status = AudioOutputUnitStart(m_OutputUnit);
    if (status != noErr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Unable to start the CoreAudio output unit: %d",
                     (int)status);
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "CoreAudio output: %s (%u channels, output type %u), %s for %d-channel stream",
                qUtf8Printable(deviceName(m_OutputDevice)),
                deviceChannels,
                (unsigned int)outputType,
                spatial ? "spatial audio" : "passthrough",
                opusConfig->channelCount);
    return true;
}

bool CoreAudioRenderer::openOutputUnit()
{
    AudioComponentDescription desc = {};
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_HALOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;

    AudioComponent component = AudioComponentFindNext(nullptr, &desc);
    if (component == nullptr || AudioComponentInstanceNew(component, &m_OutputUnit) != noErr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Unable to create the CoreAudio output unit");
        m_OutputUnit = nullptr;
        return false;
    }

    // Pin the unit to the device we inspected, rather than letting it follow
    // the default device on its own. We get recreated when the default changes.
    OSStatus status = AudioUnitSetProperty(m_OutputUnit, kAudioOutputUnitProperty_CurrentDevice,
                                           kAudioUnitScope_Global, 0, &m_OutputDevice, sizeof(m_OutputDevice));
    if (status != noErr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Unable to select the CoreAudio output device: %d",
                     (int)status);
        return false;
    }

    return true;
}

AUSpatialMixerOutputType CoreAudioRenderer::getSpatialMixerOutputType()
{
    UInt32 transport = 0;
    getDeviceProperty(m_OutputDevice, kAudioDevicePropertyTransportType,
                      kAudioObjectPropertyScopeGlobal, transport);

    switch (transport) {
    case kAudioDeviceTransportTypeBluetooth:
    case kAudioDeviceTransportTypeBluetoothLE:
    case kAudioDeviceTransportTypeUSB:
        // Nearly always a headset
        return kSpatialMixerOutputType_Headphones;

    case kAudioDeviceTransportTypeBuiltIn: {
        UInt32 dataSource = 0;
        if (getDeviceProperty(m_OutputDevice, kAudioDevicePropertyDataSource,
                              kAudioDevicePropertyScopeOutput, dataSource)) {
            if (dataSource == kIOAudioOutputPortSubTypeInternalSpeaker) {
                return kSpatialMixerOutputType_BuiltInSpeakers;
            }
            else if (dataSource == kIOAudioOutputPortSubTypeHeadphones) {
                return kSpatialMixerOutputType_Headphones;
            }
        }
        return kSpatialMixerOutputType_ExternalSpeakers;
    }

    default:
        return kSpatialMixerOutputType_ExternalSpeakers;
    }
}

void CoreAudioRenderer::addListeners()
{
    // The data source isn't present on every device, so failures are ignored
    AudioObjectAddPropertyListener(kAudioObjectSystemObject, &DefaultDeviceAddress, deviceChanged, this);
    for (const AudioObjectPropertyAddress& addr : DeviceAddresses) {
        AudioObjectAddPropertyListener(m_OutputDevice, &addr, deviceChanged, this);
    }
    m_ListenersAdded = true;
}

void CoreAudioRenderer::removeListeners()
{
    if (!m_ListenersAdded) {
        return;
    }

    AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &DefaultDeviceAddress, deviceChanged, this);
    for (const AudioObjectPropertyAddress& addr : DeviceAddresses) {
        AudioObjectRemovePropertyListener(m_OutputDevice, &addr, deviceChanged, this);
    }
    m_ListenersAdded = false;
}

OSStatus CoreAudioRenderer::deviceChanged(AudioObjectID,
                                          UInt32,
                                          const AudioObjectPropertyAddress*,
                                          void* inClientData)
{
    auto me = static_cast<CoreAudioRenderer*>(inClientData);
    me->m_NeedsReinit = true;
    return noErr;
}

OSStatus CoreAudioRenderer::renderCallback(void* inRefCon,
                                           AudioUnitRenderActionFlags* ioActionFlags,
                                           const AudioTimeStamp* inTimeStamp,
                                           UInt32,
                                           UInt32 inNumberFrames,
                                           AudioBufferList* ioData)
{
    auto me = static_cast<CoreAudioRenderer*>(inRefCon);
    size_t bytes = inNumberFrames * me->m_FrameSize;

    if (me->m_SpatialMixer != nullptr) {
        if (bytes <= me->m_RenderBuffer.size()) {
            // Keep the mixer running through an underrun so its output stays
            // continuous when audio resumes
            if (!me->m_RingBuffer.read(me->m_RenderBuffer.data(), bytes)) {
                memset(me->m_RenderBuffer.data(), 0, bytes);
            }
            return me->m_SpatialMixer->render(reinterpret_cast<const float*>(me->m_RenderBuffer.data()),
                                              inNumberFrames, ioActionFlags, inTimeStamp, ioData);
        }
    }
    else if (ioData->mNumberBuffers == 1 && ioData->mBuffers[0].mDataByteSize == bytes &&
             me->m_RingBuffer.read(ioData->mBuffers[0].mData, bytes)) {
        return noErr;
    }

    // Not enough audio has arrived yet
    for (UInt32 i = 0; i < ioData->mNumberBuffers; i++) {
        memset(ioData->mBuffers[i].mData, 0, ioData->mBuffers[i].mDataByteSize);
    }
    *ioActionFlags |= kAudioUnitRenderAction_OutputIsSilence;
    return noErr;
}

void* CoreAudioRenderer::getAudioBuffer(int*)
{
    return m_PacketBuffer.data();
}

bool CoreAudioRenderer::submitAudio(int bytesWritten)
{
    // We're recreated after any change to the output device or its route
    if (m_NeedsReinit) {
        return false;
    }

    if (bytesWritten == 0) {
        // Nothing to do
        return true;
    }

    // Don't queue if there's already more than 30 ms of audio data waiting
    // in Moonlight's audio queue.
    if (LiGetPendingAudioDuration() > 30) {
        return true;
    }

    // A full ring means the device has fallen behind, so drop the packet
    // rather than build up latency
    m_RingBuffer.write(m_PacketBuffer.data(), bytesWritten);
    return true;
}

IAudioRenderer::AudioFormat CoreAudioRenderer::getAudioBufferFormat()
{
    return AudioFormat::Float32NE;
}
