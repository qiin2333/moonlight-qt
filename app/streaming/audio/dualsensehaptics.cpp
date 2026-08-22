#include "dualsensehaptics.h"
#include "dualsensehapticsstream.h"

#include "SDL_compat.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <QtGlobal>

#ifdef Q_OS_WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <audioclient.h>
#include <cwctype>
#include <propkey.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <QString>

using Microsoft::WRL::ComPtr;
#elif defined(Q_OS_MACOS)
#include "renderers/coreaudio/TPCircularBuffer.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace {
constexpr std::size_t MaxQueuedPackets = 32;
constexpr std::uint32_t PrebufferFrames = 720; // 15 ms at 48 kHz

// The DualSense exposes a single four-channel USB audio endpoint: channels 1
// and 2 drive the headset jack, channels 3 and 4 drive the two haptic voice
// coils. We render silence to the headset pair and the authored PCM to the
// haptics pair.
constexpr std::uint32_t EndpointChannelCount = 4;
constexpr std::uint32_t HapticsChannelOffset = 2;

struct Packet
{
    std::uint8_t flags = 0;
    std::uint16_t controllerNumber = 0;
    std::uint16_t frameCount = 0;
    std::uint32_t sequenceNumber = 0;
    std::vector<std::uint8_t> pcm;
};

// Platform sink for haptics PCM. The queueing, stream tracking and prebuffer
// state machine around it are shared; only endpoint discovery and the actual
// hand-off to the OS audio stack differ per platform.
class HapticsEndpoint
{
public:
    virtual ~HapticsEndpoint() = default;

    // Called on the worker thread before and after any other method.
    virtual bool threadInit() { return true; }
    virtual void threadCleanup() {}

    // Acquire (or release) the DualSense endpoint. open() also publishes
    // bufferFrames().
    virtual bool open() = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;

    // How many frames may be written before start() must be called.
    virtual std::uint32_t bufferFrames() const = 0;

    // Blocks until the endpoint has room. Returns false once the endpoint has
    // failed and must be reopened.
    virtual bool write(const Packet& packet, const std::atomic_bool& stopping) = 0;

    // Begin playback of whatever has been written so far.
    virtual bool start() = 0;

    // Stop playback and discard audio that has not been played yet. The
    // endpoint stays open.
    virtual void reset() = 0;
};

#ifdef Q_OS_WIN32
class WasapiHapticsEndpoint final : public HapticsEndpoint
{
public:
    ~WasapiHapticsEndpoint() override
    {
        close();
    }

    static bool isDualSenseName(const std::wstring& name)
    {
        auto lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), towlower);
        return lower.find(L"dualsense") != std::wstring::npos ||
               lower.find(L"wireless controller") != std::wstring::npos ||
               lower.find(L"hidmaestro") != std::wstring::npos;
    }

    static bool classifyFormat(const WAVEFORMATEX* format, bool& isFloat, WORD& bits)
    {
        if (format->nSamplesPerSec != 48000 || format->nChannels != EndpointChannelCount) {
            return false;
        }

        GUID subtype = GUID_NULL;
        if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
            format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
            subtype = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format)->SubFormat;
        }
        else if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
            subtype = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        }
        else if (format->wFormatTag == WAVE_FORMAT_PCM) {
            subtype = KSDATAFORMAT_SUBTYPE_PCM;
        }

        bits = format->wBitsPerSample;
        isFloat = subtype == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT && bits == 32;
        return isFloat || (subtype == KSDATAFORMAT_SUBTYPE_PCM && (bits == 16 || bits == 32));
    }

    bool threadInit() override
    {
        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(comResult)) {
            SDL_LogError(SDL_LOG_CATEGORY_AUDIO,
                         "Unable to initialize COM for DualSense haptics: 0x%08lx",
                         static_cast<unsigned long>(comResult));
            return false;
        }
        return true;
    }

    void threadCleanup() override
    {
        CoUninitialize();
    }

    bool open() override
    {
        ComPtr<IMMDeviceEnumerator> enumerator;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    IID_PPV_ARGS(&enumerator)))) {
            return false;
        }

        ComPtr<IMMDeviceCollection> devices;
        if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices))) {
            return false;
        }

        UINT count = 0;
        devices->GetCount(&count);
        for (UINT i = 0; i < count; i++) {
            ComPtr<IMMDevice> device;
            if (FAILED(devices->Item(i, &device))) continue;

            std::wstring friendlyName;
            ComPtr<IPropertyStore> properties;
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties))) {
                PROPVARIANT value;
                PropVariantInit(&value);
                if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &value)) &&
                    value.vt == VT_LPWSTR && value.pwszVal != nullptr) {
                    friendlyName = value.pwszVal;
                }
                PropVariantClear(&value);
            }
            if (!isDualSenseName(friendlyName)) continue;

            ComPtr<IAudioClient> candidate;
            if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                        reinterpret_cast<void**>(candidate.GetAddressOf())))) {
                continue;
            }

            WAVEFORMATEX* mix = nullptr;
            if (FAILED(candidate->GetMixFormat(&mix)) || mix == nullptr) {
                CoTaskMemFree(mix);
                continue;
            }
            bool candidateFloat = false;
            WORD candidateBits = 0;
            const bool supported = classifyFormat(mix, candidateFloat, candidateBits);
            if (!supported || FAILED(candidate->Initialize(
                    AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_NOPERSIST,
                    500000, 0, mix, nullptr))) {
                CoTaskMemFree(mix);
                continue;
            }
            CoTaskMemFree(mix);

            ComPtr<IAudioRenderClient> candidateRenderer;
            if (FAILED(candidate->GetService(IID_PPV_ARGS(&candidateRenderer))) ||
                FAILED(candidate->GetBufferSize(&m_BufferFrames))) {
                continue;
            }

            m_AudioClient = candidate;
            m_RenderClient = candidateRenderer;
            m_FloatSamples = candidateFloat;
            m_BitsPerSample = candidateBits;
            const QByteArray name = QString::fromStdWString(friendlyName).toUtf8();
            SDL_LogInfo(SDL_LOG_CATEGORY_AUDIO,
                        "DualSense haptics endpoint ready: %s (48 kHz, 4 ch, %u-bit%s)",
                        name.constData(), m_BitsPerSample, m_FloatSamples ? " float" : " PCM");
            return true;
        }

        SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO,
                    "No active 48 kHz four-channel DualSense audio endpoint was found");
        return false;
    }

    void close() override
    {
        reset();
        m_RenderClient.Reset();
        m_AudioClient.Reset();
        m_BufferFrames = 0;
    }

    bool isOpen() const override
    {
        return m_AudioClient != nullptr;
    }

    std::uint32_t bufferFrames() const override
    {
        return m_BufferFrames;
    }

    bool start() override
    {
        if (!m_AudioClient || FAILED(m_AudioClient->Start())) {
            return false;
        }
        m_Started = true;
        return true;
    }

    void reset() override
    {
        if (m_AudioClient) {
            if (m_Started) m_AudioClient->Stop();
            m_AudioClient->Reset();
        }
        m_Started = false;
    }

    bool write(const Packet& packet, const std::atomic_bool& stopping) override
    {
        if (!m_AudioClient || !m_RenderClient || packet.frameCount == 0) return true;
        while (!stopping) {
            UINT32 padding = 0;
            if (FAILED(m_AudioClient->GetCurrentPadding(&padding))) return false;
            if (m_BufferFrames - padding >= packet.frameCount) break;
            Sleep(1);
        }
        if (stopping) return false;

        BYTE* output = nullptr;
        if (FAILED(m_RenderClient->GetBuffer(packet.frameCount, &output))) return false;
        const auto* input = reinterpret_cast<const std::int16_t*>(packet.pcm.data());
        if (m_FloatSamples) {
            auto* samples = reinterpret_cast<float*>(output);
            std::fill_n(samples, static_cast<std::size_t>(packet.frameCount) * EndpointChannelCount, 0.0f);
            for (std::uint16_t i = 0; i < packet.frameCount; i++) {
                samples[i * EndpointChannelCount + HapticsChannelOffset] = input[i * 2] / 32768.0f;
                samples[i * EndpointChannelCount + HapticsChannelOffset + 1] = input[i * 2 + 1] / 32768.0f;
            }
        }
        else if (m_BitsPerSample == 16) {
            auto* samples = reinterpret_cast<std::int16_t*>(output);
            std::fill_n(samples, static_cast<std::size_t>(packet.frameCount) * EndpointChannelCount, 0);
            for (std::uint16_t i = 0; i < packet.frameCount; i++) {
                samples[i * EndpointChannelCount + HapticsChannelOffset] = input[i * 2];
                samples[i * EndpointChannelCount + HapticsChannelOffset + 1] = input[i * 2 + 1];
            }
        }
        else {
            auto* samples = reinterpret_cast<std::int32_t*>(output);
            std::fill_n(samples, static_cast<std::size_t>(packet.frameCount) * EndpointChannelCount, 0);
            for (std::uint16_t i = 0; i < packet.frameCount; i++) {
                samples[i * EndpointChannelCount + HapticsChannelOffset] = static_cast<std::int32_t>(input[i * 2]) * 65536;
                samples[i * EndpointChannelCount + HapticsChannelOffset + 1] = static_cast<std::int32_t>(input[i * 2 + 1]) * 65536;
            }
        }
        return SUCCEEDED(m_RenderClient->ReleaseBuffer(packet.frameCount, 0));
    }

private:
    ComPtr<IAudioClient> m_AudioClient;
    ComPtr<IAudioRenderClient> m_RenderClient;
    UINT32 m_BufferFrames = 0;
    WORD m_BitsPerSample = 0;
    bool m_FloatSamples = false;
    bool m_Started = false;
};

bool probeHapticsEndpoint()
{
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool shouldUninitialize = SUCCEEDED(comResult);
    if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE) {
        return false;
    }

    bool found = false;
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDeviceCollection> devices;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   IID_PPV_ARGS(&enumerator))) &&
        SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices))) {
        UINT count = 0;
        devices->GetCount(&count);
        for (UINT i = 0; i < count && !found; i++) {
            ComPtr<IMMDevice> device;
            if (FAILED(devices->Item(i, &device))) continue;

            std::wstring friendlyName;
            ComPtr<IPropertyStore> properties;
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties))) {
                PROPVARIANT value;
                PropVariantInit(&value);
                if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &value)) &&
                    value.vt == VT_LPWSTR && value.pwszVal != nullptr) {
                    friendlyName = value.pwszVal;
                }
                PropVariantClear(&value);
            }
            if (!WasapiHapticsEndpoint::isDualSenseName(friendlyName)) continue;

            ComPtr<IAudioClient> candidate;
            if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                        reinterpret_cast<void**>(candidate.GetAddressOf())))) {
                continue;
            }
            WAVEFORMATEX* mix = nullptr;
            if (SUCCEEDED(candidate->GetMixFormat(&mix)) && mix != nullptr) {
                bool isFloat = false;
                WORD bits = 0;
                found = WasapiHapticsEndpoint::classifyFormat(mix, isFloat, bits);
            }
            CoTaskMemFree(mix);
        }
    }

    if (shouldUninitialize) {
        CoUninitialize();
    }
    return found;
}

#elif defined(Q_OS_MACOS)

// 100 ms of four-channel float audio. Large enough that the worker thread never
// blocks in practice, small enough that a stalled stream cannot accumulate a
// haptics delay the player would notice.
constexpr std::uint32_t RingFrames = 4800;
constexpr AudioUnitElement OutputElement = 0;
constexpr AudioUnitElement InputElement = 1;

std::string lowercased(const std::string& value)
{
    std::string lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower;
}

bool isDualSenseName(const std::string& name)
{
    const auto lower = lowercased(name);
    // A DualShock 4 also calls itself "Wireless Controller", but it exposes a
    // two-channel endpoint, so the channel count check below rules it out.
    return lower.find("dualsense") != std::string::npos ||
           lower.find("wireless controller") != std::string::npos;
}

bool copyDeviceName(AudioDeviceID device, std::string& out)
{
    AudioObjectPropertyAddress addr{kAudioObjectPropertyName,
                                    kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    CFStringRef value = nullptr;
    UInt32 size = sizeof(value);
    if (AudioObjectGetPropertyData(device, &addr, 0, nullptr, &size, &value) != noErr ||
        value == nullptr) {
        return false;
    }

    const CFIndex capacity =
        CFStringGetMaximumSizeForEncoding(CFStringGetLength(value), kCFStringEncodingUTF8) + 1;
    std::vector<char> buffer(static_cast<std::size_t>(capacity));
    const bool converted = CFStringGetCString(value, buffer.data(), capacity, kCFStringEncodingUTF8);
    CFRelease(value);
    if (!converted) {
        return false;
    }

    out.assign(buffer.data());
    return true;
}

std::uint32_t outputChannelCount(AudioDeviceID device)
{
    AudioObjectPropertyAddress addr{kAudioDevicePropertyStreamConfiguration,
                                    kAudioDevicePropertyScopeOutput,
                                    kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(device, &addr, 0, nullptr, &size) != noErr || size == 0) {
        return 0;
    }

    std::vector<std::uint8_t> storage(size);
    auto* list = reinterpret_cast<AudioBufferList*>(storage.data());
    if (AudioObjectGetPropertyData(device, &addr, 0, nullptr, &size, list) != noErr) {
        return 0;
    }

    std::uint32_t channels = 0;
    for (UInt32 i = 0; i < list->mNumberBuffers; i++) {
        channels += list->mBuffers[i].mNumberChannels;
    }
    return channels;
}

bool isUsbDevice(AudioDeviceID device)
{
    AudioObjectPropertyAddress addr{kAudioDevicePropertyTransportType,
                                    kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    UInt32 transport = 0;
    UInt32 size = sizeof(transport);
    if (AudioObjectGetPropertyData(device, &addr, 0, nullptr, &size, &transport) != noErr) {
        return false;
    }
    return transport == kAudioDeviceTransportTypeUSB;
}

bool hasHapticsSampleRate(AudioDeviceID device)
{
    AudioObjectPropertyAddress addr{kAudioDevicePropertyNominalSampleRate,
                                    kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    Float64 rate = 0.0;
    UInt32 size = sizeof(rate);
    if (AudioObjectGetPropertyData(device, &addr, 0, nullptr, &size, &rate) != noErr) {
        return false;
    }
    // The controller only ever runs at 48 kHz. Refuse anything else rather than
    // let the HAL resample authored haptics behind our back.
    return rate == 48000.0;
}

bool findEndpointDevice(AudioDeviceID* outDevice, std::string* outName)
{
    AudioObjectPropertyAddress addr{kAudioHardwarePropertyDevices,
                                    kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, nullptr, &size) != noErr ||
        size == 0) {
        return false;
    }

    std::vector<AudioDeviceID> devices(size / sizeof(AudioDeviceID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size,
                                   devices.data()) != noErr) {
        return false;
    }

    for (AudioDeviceID device : devices) {
        std::string name;
        if (!copyDeviceName(device, name) || !isDualSenseName(name)) continue;
        if (outputChannelCount(device) != EndpointChannelCount) continue;
        if (!isUsbDevice(device)) continue;
        if (!hasHapticsSampleRate(device)) continue;

        if (outDevice != nullptr) *outDevice = device;
        if (outName != nullptr) *outName = name;
        return true;
    }

    return false;
}

class CoreAudioHapticsEndpoint final : public HapticsEndpoint
{
public:
    ~CoreAudioHapticsEndpoint() override
    {
        close();
    }

    bool open() override
    {
        AudioDeviceID device = kAudioObjectUnknown;
        std::string name;
        if (!findEndpointDevice(&device, &name)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO,
                        "No active 48 kHz four-channel DualSense audio endpoint was found");
            return false;
        }

        if (!TPCircularBufferInit(&m_Ring, RingFrames * EndpointChannelCount * sizeof(float))) {
            SDL_LogError(SDL_LOG_CATEGORY_AUDIO,
                         "Unable to allocate the DualSense haptics ring buffer");
            return false;
        }
        m_RingValid = true;
        // TPCircularBuffer rounds the capacity up to a page multiple, so ask it
        // what we actually got rather than assuming we got what we requested.
        m_BufferFrames = m_Ring.length / (EndpointChannelCount * sizeof(float));

        AudioComponentDescription desc = {};
        desc.componentType = kAudioUnitType_Output;
        desc.componentSubType = kAudioUnitSubType_HALOutput;
        desc.componentManufacturer = kAudioUnitManufacturer_Apple;

        AudioComponent component = AudioComponentFindNext(nullptr, &desc);
        if (component == nullptr || AudioComponentInstanceNew(component, &m_Unit) != noErr) {
            m_Unit = nullptr;
            close();
            return false;
        }

        // Output only. Leaving the input element enabled would make macOS treat
        // us as a recording client and prompt the user for microphone access.
        UInt32 enable = 1;
        UInt32 disable = 0;
        if (AudioUnitSetProperty(m_Unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output,
                                 OutputElement, &enable, sizeof(enable)) != noErr ||
            AudioUnitSetProperty(m_Unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input,
                                 InputElement, &disable, sizeof(disable)) != noErr ||
            AudioUnitSetProperty(m_Unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global,
                                 OutputElement, &device, sizeof(device)) != noErr) {
            close();
            return false;
        }

        AudioStreamBasicDescription asbd = {};
        asbd.mSampleRate = 48000.0;
        asbd.mFormatID = kAudioFormatLinearPCM;
        asbd.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        asbd.mChannelsPerFrame = EndpointChannelCount;
        asbd.mBitsPerChannel = 32;
        asbd.mFramesPerPacket = 1;
        asbd.mBytesPerFrame = EndpointChannelCount * sizeof(float);
        asbd.mBytesPerPacket = asbd.mBytesPerFrame;
        if (AudioUnitSetProperty(m_Unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input,
                                 OutputElement, &asbd, sizeof(asbd)) != noErr) {
            close();
            return false;
        }

        AURenderCallbackStruct callback = {};
        callback.inputProc = renderCallback;
        callback.inputProcRefCon = this;
        if (AudioUnitSetProperty(m_Unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input,
                                 OutputElement, &callback, sizeof(callback)) != noErr ||
            AudioUnitInitialize(m_Unit) != noErr) {
            close();
            return false;
        }

        m_Device = device;
        m_Alive = true;
        addAliveListener();

        SDL_LogInfo(SDL_LOG_CATEGORY_AUDIO,
                    "DualSense haptics endpoint ready: %s (48 kHz, 4 ch, 32-bit float)",
                    name.c_str());
        return true;
    }

    void close() override
    {
        if (m_Unit != nullptr) {
            removeAliveListener();
            AudioOutputUnitStop(m_Unit);
            AudioUnitUninitialize(m_Unit);
            AudioComponentInstanceDispose(m_Unit);
            m_Unit = nullptr;
        }

        m_Started = false;
        m_Device = kAudioObjectUnknown;
        m_BufferFrames = 0;
        if (m_RingValid) {
            TPCircularBufferCleanup(&m_Ring);
            m_RingValid = false;
        }
    }

    bool isOpen() const override
    {
        return m_Unit != nullptr;
    }

    std::uint32_t bufferFrames() const override
    {
        return m_BufferFrames;
    }

    bool start() override
    {
        if (m_Unit == nullptr || AudioOutputUnitStart(m_Unit) != noErr) {
            return false;
        }
        m_Started = true;
        return true;
    }

    void reset() override
    {
        // Stop first: AudioOutputUnitStop() waits for an in-flight render
        // callback, so afterwards we are the only one touching the ring.
        if (m_Unit != nullptr && m_Started) {
            AudioOutputUnitStop(m_Unit);
        }
        m_Started = false;
        if (m_RingValid) {
            TPCircularBufferClear(&m_Ring);
        }
    }

    bool write(const Packet& packet, const std::atomic_bool& stopping) override
    {
        if (m_Unit == nullptr || !m_RingValid || packet.frameCount == 0) return true;

        m_Scratch.assign(static_cast<std::size_t>(packet.frameCount) * EndpointChannelCount, 0.0f);
        const auto* input = reinterpret_cast<const std::int16_t*>(packet.pcm.data());
        for (std::uint16_t i = 0; i < packet.frameCount; i++) {
            m_Scratch[i * EndpointChannelCount + HapticsChannelOffset] = input[i * 2] / 32768.0f;
            m_Scratch[i * EndpointChannelCount + HapticsChannelOffset + 1] = input[i * 2 + 1] / 32768.0f;
        }

        const auto bytes = static_cast<std::uint32_t>(m_Scratch.size() * sizeof(float));
        while (!stopping) {
            if (!m_Alive.load(std::memory_order_relaxed)) return false;
            if (TPCircularBufferProduceBytes(&m_Ring, m_Scratch.data(), bytes)) return true;
            if (!m_Started) {
                // Nothing is draining the ring yet, so waiting would deadlock.
                // The caller is not supposed to queue more than bufferFrames()
                // before starting the stream.
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    }

private:
    static OSStatus renderCallback(void* context, AudioUnitRenderActionFlags* actionFlags,
                                   const AudioTimeStamp*, UInt32, UInt32 frameCount,
                                   AudioBufferList* data)
    {
        return static_cast<CoreAudioHapticsEndpoint*>(context)->render(actionFlags, frameCount, data);
    }

    OSStatus render(AudioUnitRenderActionFlags* actionFlags, UInt32 frameCount, AudioBufferList* data)
    {
        if (data == nullptr || data->mNumberBuffers == 0) return noErr;

        auto& buffer = data->mBuffers[0];
        auto* output = static_cast<std::uint8_t*>(buffer.mData);
        if (output == nullptr) return noErr;

        const auto requested = static_cast<std::uint32_t>(frameCount) * EndpointChannelCount *
                               static_cast<std::uint32_t>(sizeof(float));
        const std::uint32_t wanted = std::min(buffer.mDataByteSize, requested);

        std::uint32_t available = 0;
        void* tail = TPCircularBufferTail(&m_Ring, &available);
        const std::uint32_t copied = std::min(available, wanted);
        if (copied != 0) {
            std::memcpy(output, tail, copied);
            TPCircularBufferConsume(&m_Ring, copied);
        }
        if (copied < wanted) {
            std::memset(output + copied, 0, wanted - copied);
            if (copied == 0 && actionFlags != nullptr) {
                *actionFlags |= kAudioUnitRenderAction_OutputIsSilence;
            }
        }
        return noErr;
    }

    static AudioObjectPropertyAddress aliveAddress()
    {
        return {kAudioDevicePropertyDeviceIsAlive,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain};
    }

    static OSStatus aliveListener(AudioObjectID device, UInt32, const AudioObjectPropertyAddress*,
                                  void* context)
    {
        auto* self = static_cast<CoreAudioHapticsEndpoint*>(context);
        AudioObjectPropertyAddress addr = aliveAddress();
        UInt32 alive = 0;
        UInt32 size = sizeof(alive);
        if (AudioObjectGetPropertyData(device, &addr, 0, nullptr, &size, &alive) != noErr || alive == 0) {
            // Unplugged. write() gives up, the worker closes us and starts
            // probing again, so replugging the controller mid-stream recovers.
            self->m_Alive.store(false, std::memory_order_relaxed);
        }
        return noErr;
    }

    void addAliveListener()
    {
        AudioObjectPropertyAddress addr = aliveAddress();
        m_AliveListenerAdded =
            AudioObjectAddPropertyListener(m_Device, &addr, aliveListener, this) == noErr;
    }

    void removeAliveListener()
    {
        if (!m_AliveListenerAdded) return;
        AudioObjectPropertyAddress addr = aliveAddress();
        AudioObjectRemovePropertyListener(m_Device, &addr, aliveListener, this);
        m_AliveListenerAdded = false;
    }

    AudioUnit m_Unit = nullptr;
    AudioDeviceID m_Device = kAudioObjectUnknown;
    TPCircularBuffer m_Ring = {};
    bool m_RingValid = false;
    bool m_AliveListenerAdded = false;
    bool m_Started = false;
    std::uint32_t m_BufferFrames = 0;
    std::atomic_bool m_Alive{false};
    std::vector<float> m_Scratch;
};

bool probeHapticsEndpoint()
{
    return findEndpointDevice(nullptr, nullptr);
}

#else

bool probeHapticsEndpoint()
{
    return false;
}

#endif

std::unique_ptr<HapticsEndpoint> createHapticsEndpoint()
{
#ifdef Q_OS_WIN32
    return std::unique_ptr<HapticsEndpoint>(new WasapiHapticsEndpoint());
#elif defined(Q_OS_MACOS)
    return std::unique_ptr<HapticsEndpoint>(new CoreAudioHapticsEndpoint());
#else
    return nullptr;
#endif
}
}

struct DualSenseHapticsRenderer::Impl
{
    std::mutex mutex;
    std::condition_variable condition;
    std::deque<Packet> queue;
    std::atomic_bool stopping{false};

    std::unique_ptr<HapticsEndpoint> endpoint;
    dualsense_haptics::PcmStreamTracker streamTracker;
    std::deque<Packet> prebuffer;
    std::uint32_t prebufferedFrames = 0;
    bool streamStarted = false;
    std::chrono::steady_clock::time_point nextEndpointProbe{};

    // Keep this last: run() may access every member as soon as the thread starts.
    std::thread worker;

    Impl() : endpoint(createHapticsEndpoint())
    {
        if (endpoint != nullptr) {
            worker = std::thread([this] { run(); });
        }
    }

    ~Impl()
    {
        stopping = true;
        condition.notify_all();
        if (worker.joinable()) {
            worker.join();
        }
    }

    void resetAudioStream()
    {
        endpoint->reset();
        streamStarted = false;
        prebuffer.clear();
        prebufferedFrames = 0;
    }

    void resetStream()
    {
        resetAudioStream();
        streamTracker.reset();
    }

    // The endpoint failed mid-stream (device unplugged, format renegotiated,
    // ...). Drop it so the next packet probes for it again.
    void failEndpoint()
    {
        endpoint->close();
        resetStream();
    }

    bool startPrebufferedStream()
    {
        for (const auto& buffered : prebuffer) {
            if (!endpoint->write(buffered, stopping)) return false;
        }
        prebuffer.clear();
        prebufferedFrames = 0;
        if (!endpoint->start()) return false;
        streamStarted = true;
        return true;
    }

    void process(Packet packet)
    {
        const auto action = streamTracker.observe(packet.flags, packet.controllerNumber,
                                                  packet.sequenceNumber);
        if (action == dualsense_haptics::PcmStreamTracker::Action::Ignore) {
            return;
        }
        if (action == dualsense_haptics::PcmStreamTracker::Action::End) {
            resetAudioStream();
            return;
        }
        if (action == dualsense_haptics::PcmStreamTracker::Action::ResetAndAccept) {
            resetAudioStream();
        }

        if (!endpoint->isOpen()) {
            const auto now = std::chrono::steady_clock::now();
            if (now < nextEndpointProbe) {
                return;
            }
            if (!endpoint->open()) {
                nextEndpointProbe = now + std::chrono::seconds(2);
                return;
            }
        }
        if (!streamStarted) {
            if (packet.frameCount > endpoint->bufferFrames()) {
                SDL_LogError(SDL_LOG_CATEGORY_AUDIO,
                             "DualSense haptics packet (%u frames) exceeds the endpoint buffer (%u frames)",
                             packet.frameCount, endpoint->bufferFrames());
                failEndpoint();
                return;
            }

            // A shared-mode endpoint may expose less than our preferred 15 ms
            // jitter buffer. Never queue more than the endpoint can accept before
            // starting it, or write() would wait for a device that is not running.
            if (!prebuffer.empty() && prebufferedFrames + packet.frameCount > endpoint->bufferFrames()) {
                if (!startPrebufferedStream()) {
                    failEndpoint();
                    return;
                }
                if (!endpoint->write(packet, stopping)) {
                    failEndpoint();
                }
                return;
            }

            prebufferedFrames += packet.frameCount;
            prebuffer.emplace_back(std::move(packet));
            const auto targetFrames = std::min(PrebufferFrames, endpoint->bufferFrames());
            if (prebufferedFrames < targetFrames) return;

            if (!startPrebufferedStream()) {
                failEndpoint();
                return;
            }
        }
        else if (!endpoint->write(packet, stopping)) {
            failEndpoint();
        }
    }

    void run()
    {
        if (!endpoint->threadInit()) {
            return;
        }

        while (!stopping) {
            Packet packet;
            {
                std::unique_lock lock(mutex);
                condition.wait(lock, [this] { return stopping || !queue.empty(); });
                if (stopping) break;
                packet = std::move(queue.front());
                queue.pop_front();
            }
            process(std::move(packet));
        }

        resetStream();
        endpoint->close();
        endpoint->threadCleanup();
    }
};

DualSenseHapticsRenderer::DualSenseHapticsRenderer() : m_Impl(std::make_unique<Impl>()) {}
DualSenseHapticsRenderer::~DualSenseHapticsRenderer() = default;

bool DualSenseHapticsRenderer::isAvailable()
{
    return probeHapticsEndpoint();
}

void DualSenseHapticsRenderer::submit(const LI_DS5_HAPTICS_PCM_FRAME& frame)
{
    if (m_Impl->endpoint == nullptr) {
        return;
    }

    if (frame.sampleRate != 48000 || frame.channelCount != 2 || frame.bitsPerSample != 16 ||
        frame.frameCount > 480 || frame.pcmDataLength != frame.frameCount * 4 ||
        (frame.pcmDataLength != 0 && frame.pcmData == nullptr)) {
        return;
    }

    Packet packet;
    packet.flags = frame.flags;
    packet.controllerNumber = frame.controllerNumber;
    packet.frameCount = frame.frameCount;
    packet.sequenceNumber = frame.sequenceNumber;
    if (frame.pcmDataLength != 0) {
        packet.pcm.assign(frame.pcmData, frame.pcmData + frame.pcmDataLength);
    }
    {
        std::lock_guard lock(m_Impl->mutex);
        if (m_Impl->queue.size() == MaxQueuedPackets) {
            m_Impl->queue.pop_front();
            packet.flags |= LI_DS5_HAPTICS_PCM_FLAG_DISCONTINUITY;
        }
        m_Impl->queue.emplace_back(std::move(packet));
    }
    m_Impl->condition.notify_one();
}
