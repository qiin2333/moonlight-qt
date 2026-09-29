#include "dualsensehaptics.h"
#include "dualsensehapticsstream.h"

#include "SDL_compat.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <QtGlobal>
#include <QString>

#ifdef Q_OS_WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <audioclient.h>
#include <propkey.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <propvarutil.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;
#elif defined(Q_OS_MACOS)
#include "dualsensehapticsmac.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace {
#ifdef HAVE_PHYSICAL_DS5_HAPTICS
constexpr std::size_t MaxQueuedPackets = 32;

// How long to wait before probing for the endpoint again after it failed to
// open or stopped draining.
constexpr auto EndpointProbeBackoff = std::chrono::seconds(2);

// How long a single write may wait for the endpoint to make room before we give
// up on it.
constexpr auto EndpointWriteTimeout = std::chrono::milliseconds(200);

struct Packet
{
    std::uint8_t flags = 0;
    std::uint16_t controllerNumber = 0;
    std::uint16_t frameCount = 0;
    std::uint32_t sequenceNumber = 0;
    std::vector<std::uint8_t> pcm;
};

enum class WriteResult
{
    Ok,
    WouldBlock, // No room right now; retry after the endpoint drains.
    Failed,     // Gone or wedged; the endpoint must be reopened.
};

// A DualShock 4 also calls itself "Wireless Controller", but it exposes a
// two-channel endpoint, so the channel count check rules it out.
bool isDualSenseName(const QString& name)
{
    return name.contains(QLatin1String("dualsense"), Qt::CaseInsensitive) ||
           name.contains(QLatin1String("wireless controller"), Qt::CaseInsensitive) ||
           name.contains(QLatin1String("hidmaestro"), Qt::CaseInsensitive);
}

using dualsense_haptics::EndpointChannelCount;
using dualsense_haptics::spreadToHapticsChannels;

// The packet's PCM as interleaved 16-bit stereo.
const std::int16_t* pcmSamples(const Packet& packet)
{
    return reinterpret_cast<const std::int16_t*>(packet.pcm.data());
}
#endif

#ifndef HAVE_PHYSICAL_DS5_HAPTICS

DualSenseHapticsRenderer::Availability probeHapticsEndpoint()
{
    return DualSenseHapticsRenderer::Availability::NotFound;
}

#elif defined(Q_OS_WIN32)
// Call visit() with an audio client for each active render endpoint named like
// a DualSense, until it accepts one.
template <typename Visit> bool findDualSenseAudioClient(Visit visit)
{
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDeviceCollection> devices;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator))) ||
        FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices))) {
        return false;
    }

    UINT count = 0;
    devices->GetCount(&count);
    for (UINT i = 0; i < count; i++) {
        ComPtr<IMMDevice> device;
        if (FAILED(devices->Item(i, &device)))
            continue;

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
        const QString name = QString::fromStdWString(friendlyName);
        if (!isDualSenseName(name))
            continue;

        ComPtr<IAudioClient> client;
        if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void**>(client.GetAddressOf())))) {
            continue;
        }
        if (visit(client, name)) {
            return true;
        }
    }
    return false;
}

class WasapiHapticsEndpoint
{
public:
    ~WasapiHapticsEndpoint() { close(); }

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

    bool threadInit()
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

    void threadCleanup() { CoUninitialize(); }

    bool open()
    {
        const bool found = findDualSenseAudioClient([this](const ComPtr<IAudioClient>& candidate,
                                                           const QString& name) {
            WAVEFORMATEX* mix = nullptr;
            if (FAILED(candidate->GetMixFormat(&mix)) || mix == nullptr) {
                CoTaskMemFree(mix);
                return false;
            }
            bool candidateFloat = false;
            WORD candidateBits = 0;
            const bool supported = classifyFormat(mix, candidateFloat, candidateBits);
            if (!supported || FAILED(candidate->Initialize(
                    AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_NOPERSIST,
                    500000, 0, mix, nullptr))) {
                CoTaskMemFree(mix);
                return false;
            }
            CoTaskMemFree(mix);

            ComPtr<IAudioRenderClient> candidateRenderer;
            if (FAILED(candidate->GetService(IID_PPV_ARGS(&candidateRenderer))) ||
                FAILED(candidate->GetBufferSize(&m_BufferFrames))) {
                return false;
            }

            m_AudioClient = candidate;
            m_RenderClient = candidateRenderer;
            m_FloatSamples = candidateFloat;
            m_BitsPerSample = candidateBits;
            SDL_LogInfo(SDL_LOG_CATEGORY_AUDIO,
                        "DualSense haptics endpoint ready: %s (48 kHz, 4 ch, %u-bit%s)",
                        qPrintable(name), m_BitsPerSample, m_FloatSamples ? " float" : " PCM");
            return true;
        });

        if (!found) {
            SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO,
                        "No active 48 kHz four-channel DualSense audio endpoint was found");
        }
        return found;
    }

    void close()
    {
        reset();
        m_RenderClient.Reset();
        m_AudioClient.Reset();
        m_BufferFrames = 0;
    }

    bool isOpen() const { return m_AudioClient != nullptr; }
    bool isStarted() const { return m_Started; }

    // How many frames may be written before start() must be called.
    std::uint32_t bufferFrames() const { return m_BufferFrames; }
    std::uint32_t maxQueuedPackets() const { return 0; }

    bool start()
    {
        if (!m_AudioClient || FAILED(m_AudioClient->Start())) {
            return false;
        }
        m_Started = true;
        return true;
    }

    // Stop playback and discard audio that has not been played yet.
    void reset()
    {
        if (m_AudioClient) {
            if (m_Started)
                m_AudioClient->Stop();
            m_AudioClient->Reset();
        }
        m_Started = false;
    }

    WriteResult tryWrite(const Packet& packet)
    {
        if (!m_AudioClient || !m_RenderClient || packet.frameCount == 0)
            return WriteResult::Ok;

        UINT32 padding = 0;
        if (FAILED(m_AudioClient->GetCurrentPadding(&padding)))
            return WriteResult::Failed;
        if (m_BufferFrames - padding < packet.frameCount)
            return WriteResult::WouldBlock;

        BYTE* output = nullptr;
        if (FAILED(m_RenderClient->GetBuffer(packet.frameCount, &output)))
            return WriteResult::Failed;

        if (m_FloatSamples) {
            spreadToHapticsChannels(reinterpret_cast<float*>(output), pcmSamples(packet),
                                    packet.frameCount, [](std::int16_t s) { return s / 32768.0f; });
        } else if (m_BitsPerSample == 16) {
            spreadToHapticsChannels(reinterpret_cast<std::int16_t*>(output), pcmSamples(packet),
                                    packet.frameCount, [](std::int16_t s) { return s; });
        } else {
            spreadToHapticsChannels(
                reinterpret_cast<std::int32_t*>(output), pcmSamples(packet), packet.frameCount,
                [](std::int16_t s) { return static_cast<std::int32_t>(s) * 65536; });
        }
        return SUCCEEDED(m_RenderClient->ReleaseBuffer(packet.frameCount, 0)) ? WriteResult::Ok
                                                                              : WriteResult::Failed;
    }

private:
    ComPtr<IAudioClient> m_AudioClient;
    ComPtr<IAudioRenderClient> m_RenderClient;
    UINT32 m_BufferFrames = 0;
    WORD m_BitsPerSample = 0;
    bool m_FloatSamples = false;
    bool m_Started = false;
};

using HapticsEndpoint = WasapiHapticsEndpoint;

DualSenseHapticsRenderer::Availability probeHapticsEndpoint()
{
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool shouldUninitialize = SUCCEEDED(comResult);
    if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE) {
        return DualSenseHapticsRenderer::Availability::NotFound;
    }

    const bool found =
        findDualSenseAudioClient([](const ComPtr<IAudioClient>& candidate, const QString&) {
            WAVEFORMATEX* mix = nullptr;
            bool supported = false;
            if (SUCCEEDED(candidate->GetMixFormat(&mix)) && mix != nullptr) {
                bool isFloat = false;
                WORD bits = 0;
                supported = WasapiHapticsEndpoint::classifyFormat(mix, isFloat, bits);
            }
            CoTaskMemFree(mix);
            return supported;
        });

    if (shouldUninitialize) {
        CoUninitialize();
    }
    return found ? DualSenseHapticsRenderer::Availability::Available
                 : DualSenseHapticsRenderer::Availability::NotFound;
}

#elif defined(Q_OS_MACOS)

constexpr UInt32 EndpointBytesPerFrame = EndpointChannelCount * sizeof(float);

// Each AudioQueue buffer carries exactly one packet, and submit() never passes
// on a packet longer than 480 frames.
constexpr UInt32 QueueBufferFrames = 480;
constexpr UInt32 QueueBufferCount = 32;
// 50 ms, the same as the WASAPI endpoint asks for. This is what bounds the
// haptics delay a burst of packets can build up.
constexpr std::uint32_t MaxQueuedFrames = 2400;

// Fixed-size scalar property read. The variable-size cases (device list, stream
// configuration) keep their own bodies below.
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
    if (!getDeviceProperty(device, kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal,
                           value) ||
        value == nullptr) {
        return QString();
    }

    const QString name = QString::fromCFString(value);
    CFRelease(value);
    return name;
}

std::uint32_t outputChannelCount(AudioDeviceID device)
{
    AudioObjectPropertyAddress addr{ kAudioDevicePropertyStreamConfiguration,
                                     kAudioDevicePropertyScopeOutput,
                                     kAudioObjectPropertyElementMain };
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
    UInt32 transport = 0;
    return getDeviceProperty(device, kAudioDevicePropertyTransportType,
                             kAudioObjectPropertyScopeGlobal, transport) &&
           transport == kAudioDeviceTransportTypeUSB;
}

bool hasHapticsSampleRate(AudioDeviceID device)
{
    // The controller only ever runs at 48 kHz. Refuse anything else rather than
    // let the HAL resample authored haptics behind our back.
    Float64 rate = 0.0;
    return getDeviceProperty(device, kAudioDevicePropertyNominalSampleRate,
                             kAudioObjectPropertyScopeGlobal, rate) &&
           rate == 48000.0;
}

DualSenseHapticsRenderer::Availability findEndpointDevice(AudioDeviceID* outDevice,
                                                          QString* outName)
{
    AudioObjectPropertyAddress addr{ kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, nullptr, &size) !=
            noErr ||
        size == 0) {
        return DualSenseHapticsRenderer::Availability::NotFound;
    }

    std::vector<AudioDeviceID> devices(size / sizeof(AudioDeviceID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size,
                                   devices.data()) != noErr) {
        return DualSenseHapticsRenderer::Availability::NotFound;
    }

    AudioDeviceID found = kAudioObjectUnknown;
    QString foundName;
    for (AudioDeviceID device : devices) {
        if (!isUsbDevice(device) || outputChannelCount(device) != EndpointChannelCount ||
            !hasHapticsSampleRate(device)) {
            continue;
        }

        const QString name = deviceName(device);
        if (!isDualSenseName(name))
            continue;

        // Nothing ties an audio endpoint back to the controller number the host
        // addressed, so refuse to guess between several pads.
        if (found != kAudioObjectUnknown) {
            return DualSenseHapticsRenderer::Availability::MultipleEndpoints;
        }
        found = device;
        foundName = name;
    }

    if (found == kAudioObjectUnknown) {
        return DualSenseHapticsRenderer::Availability::NotFound;
    }

    if (outDevice != nullptr)
        *outDevice = found;
    if (outName != nullptr)
        *outName = foundName;
    return DualSenseHapticsRenderer::Availability::Available;
}

class CoreAudioHapticsEndpoint
{
public:
    ~CoreAudioHapticsEndpoint() { close(); }

    bool threadInit() { return true; }
    void threadCleanup() {}

    bool open()
    {
        // Drop a queue whose device went away while no stream was playing.
        close();

        AudioDeviceID device = kAudioObjectUnknown;
        QString name;
        switch (findEndpointDevice(&device, &name)) {
        case DualSenseHapticsRenderer::Availability::Available:
            break;
        case DualSenseHapticsRenderer::Availability::NotFound:
            SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO,
                        "No active 48 kHz four-channel DualSense audio endpoint was found");
            return false;
        case DualSenseHapticsRenderer::Availability::MultipleEndpoints:
            SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO,
                        "Multiple DualSense audio endpoints are connected; physical haptics needs "
                        "exactly one to know which pad a stream belongs to");
            return false;
        }

        AudioStreamBasicDescription asbd = {};
        asbd.mSampleRate = 48000.0;
        asbd.mFormatID = kAudioFormatLinearPCM;
        asbd.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        asbd.mChannelsPerFrame = EndpointChannelCount;
        asbd.mBitsPerChannel = 32;
        asbd.mFramesPerPacket = 1;
        asbd.mBytesPerFrame = EndpointBytesPerFrame;
        asbd.mBytesPerPacket = asbd.mBytesPerFrame;

        // With no run loop given, the queue calls bufferDone() on a thread of its
        // own rather than on the HAL's real-time I/O thread.
        if (AudioQueueNewOutput(&asbd, bufferDone, this, nullptr, nullptr, 0, &m_Queue) != noErr) {
            m_Queue = nullptr;
            return false;
        }

        // The queue names its output device by UID rather than by AudioDeviceID.
        CFStringRef uid = nullptr;
        const bool deviceSet = getDeviceProperty(device, kAudioDevicePropertyDeviceUID,
                                                 kAudioObjectPropertyScopeGlobal, uid) &&
                               uid != nullptr &&
                               AudioQueueSetProperty(m_Queue, kAudioQueueProperty_CurrentDevice,
                                                     &uid, sizeof(uid)) == noErr;
        if (uid != nullptr) {
            CFRelease(uid);
        }
        if (!deviceSet) {
            close();
            return false;
        }

        // Reserve up front so bufferDone() never reallocates the free list.
        m_FreeBuffers.reserve(QueueBufferCount);
        for (UInt32 i = 0; i < QueueBufferCount; i++) {
            AudioQueueBufferRef buffer = nullptr;
            if (AudioQueueAllocateBuffer(m_Queue, QueueBufferFrames * EndpointBytesPerFrame,
                                         &buffer) != noErr) {
                close();
                return false;
            }
            m_FreeBuffers.push_back(buffer);
        }

        m_Device = device;
        m_Alive = true;
        addAliveListener();

        // The device's own delay on top of our queue, for measuring the total.
        UInt32 latency = 0;
        UInt32 safetyOffset = 0;
        UInt32 ioBufferFrames = 0;
        getDeviceProperty(device, kAudioDevicePropertyLatency, kAudioDevicePropertyScopeOutput,
                          latency);
        getDeviceProperty(device, kAudioDevicePropertySafetyOffset, kAudioDevicePropertyScopeOutput,
                          safetyOffset);
        getDeviceProperty(device, kAudioDevicePropertyBufferFrameSize,
                          kAudioObjectPropertyScopeGlobal, ioBufferFrames);

        SDL_LogInfo(SDL_LOG_CATEGORY_AUDIO,
                    "DualSense haptics endpoint ready: %s (48 kHz, 4 ch, 32-bit float; "
                    "queue <= %u, device latency %u, safety offset %u, I/O buffer %u frames)",
                    qPrintable(name), MaxQueuedFrames, latency, safetyOffset, ioBufferFrames);
        return true;
    }

    void close()
    {
        removeAliveListener();
        if (m_Queue != nullptr) {
            // Also frees every buffer allocated on the queue.
            AudioQueueDispose(m_Queue, true);
            m_Queue = nullptr;
        }

        m_FreeBuffers.clear();
        m_QueuedFrames = 0;
        m_Started = false;
        m_Device = kAudioObjectUnknown;
    }

    bool isOpen() const { return m_Queue != nullptr && m_Alive.load(std::memory_order_relaxed); }
    bool isStarted() const { return m_Started; }

    // How many frames may be written before start() must be called.
    std::uint32_t bufferFrames() const { return MaxQueuedFrames; }
    // One buffer carries one packet, so a prebuffer of short packets can run
    // out of buffers before it reaches bufferFrames().
    std::uint32_t maxQueuedPackets() const { return QueueBufferCount; }

    bool start()
    {
        if (m_Queue == nullptr || AudioQueueStart(m_Queue, nullptr) != noErr) {
            return false;
        }
        m_Started = true;
        return true;
    }

    // Stop playback and discard audio that has not been played yet. Pause
    // rather than stop: we land here on every lost packet, and a synchronous
    // stop would tear the device I/O down and bring it back up each time.
    void reset()
    {
        if (m_Queue != nullptr) {
            if (m_Started)
                AudioQueuePause(m_Queue);
            // Hands every enqueued buffer back through bufferDone().
            AudioQueueReset(m_Queue);
        }
        m_Started = false;
    }

    WriteResult tryWrite(const Packet& packet)
    {
        if (m_Queue == nullptr || packet.frameCount == 0)
            return WriteResult::Ok;
        if (!m_Alive.load(std::memory_order_relaxed))
            return WriteResult::Failed;
        if (packet.frameCount > QueueBufferFrames)
            return WriteResult::Failed;

        AudioQueueBufferRef buffer = nullptr;
        {
            std::lock_guard lock(m_FreeLock);
            if (!m_FreeBuffers.empty() && m_QueuedFrames + packet.frameCount <= MaxQueuedFrames) {
                buffer = m_FreeBuffers.back();
                m_FreeBuffers.pop_back();
                m_QueuedFrames += packet.frameCount;
            }
        }
        if (buffer == nullptr) {
            // Nothing drains the queue before start(), so waiting would deadlock.
            return m_Started ? WriteResult::WouldBlock : WriteResult::Failed;
        }

        spreadToHapticsChannels(static_cast<float*>(buffer->mAudioData), pcmSamples(packet),
                                packet.frameCount, [](std::int16_t s) { return s / 32768.0f; });
        buffer->mAudioDataByteSize = packet.frameCount * EndpointBytesPerFrame;
        if (AudioQueueEnqueueBuffer(m_Queue, buffer, 0, nullptr) != noErr) {
            bufferDone(this, m_Queue, buffer);
            return WriteResult::Failed;
        }
        return WriteResult::Ok;
    }

private:
    static void bufferDone(void* context, AudioQueueRef, AudioQueueBufferRef buffer)
    {
        auto* self = static_cast<CoreAudioHapticsEndpoint*>(context);
        std::lock_guard lock(self->m_FreeLock);
        self->m_QueuedFrames -= buffer->mAudioDataByteSize / EndpointBytesPerFrame;
        self->m_FreeBuffers.push_back(buffer);
    }

    static AudioObjectPropertyAddress aliveAddress()
    {
        return { kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal,
                 kAudioObjectPropertyElementMain };
    }

    static OSStatus aliveListener(AudioObjectID device, UInt32, const AudioObjectPropertyAddress*,
                                  void* context)
    {
        auto* self = static_cast<CoreAudioHapticsEndpoint*>(context);
        AudioObjectPropertyAddress addr = aliveAddress();
        UInt32 alive = 0;
        UInt32 size = sizeof(alive);
        if (AudioObjectGetPropertyData(device, &addr, 0, nullptr, &size, &alive) != noErr ||
            alive == 0) {
            // Unplugged. The worker reopens the endpoint on the next packet, so
            // replugging the controller recovers.
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
        if (!m_AliveListenerAdded)
            return;
        AudioObjectPropertyAddress addr = aliveAddress();
        AudioObjectRemovePropertyListener(m_Device, &addr, aliveListener, this);
        m_AliveListenerAdded = false;
    }

    AudioQueueRef m_Queue = nullptr;
    AudioDeviceID m_Device = kAudioObjectUnknown;
    // Guards the free list and the queued frame count, which bufferDone()
    // updates from the queue's own thread.
    std::mutex m_FreeLock;
    std::vector<AudioQueueBufferRef> m_FreeBuffers;
    std::uint32_t m_QueuedFrames = 0;
    bool m_Started = false;
    bool m_AliveListenerAdded = false;
    std::atomic_bool m_Alive{ false };
};

using HapticsEndpoint = CoreAudioHapticsEndpoint;

DualSenseHapticsRenderer::Availability probeHapticsEndpoint()
{
    return findEndpointDevice(nullptr, nullptr);
}

#endif
}

struct DualSenseHapticsRenderer::Impl
{
#ifdef HAVE_PHYSICAL_DS5_HAPTICS
    std::mutex mutex;
    std::condition_variable condition;
    std::deque<Packet> queue;
    std::atomic_bool stopping{ false };
    // Set from other threads; the worker owns the endpoint, so it performs the
    // actual teardown.
    bool resetRequested = false;
    // Nothing ties the audio endpoint to a controller number, so only the pad
    // the input handler picked is played, and only while it is the only
    // DualSense connected.
    std::atomic_int controllerTarget{ -1 };

    HapticsEndpoint endpoint;
    dualsense_haptics::PcmStreamTracker streamTracker;
    std::deque<Packet> prebuffer;
    std::uint32_t prebufferedFrames = 0;
    std::chrono::steady_clock::time_point nextEndpointProbe{};
#endif

#ifdef Q_OS_MACOS
    // The analyzed IR path, used in emulated mode.
    std::unique_ptr<MacDualSenseHapticsRenderer> macRenderer;
#endif

#ifdef HAVE_PHYSICAL_DS5_HAPTICS
    // Keep this last: run() may access every member as soon as the thread starts.
    std::thread worker;
#endif

    explicit Impl(Mode mode)
    {
        Q_UNUSED(mode);
#ifdef Q_OS_MACOS
        if (mode == Mode::Emulated) {
            macRenderer = std::make_unique<MacDualSenseHapticsRenderer>();
        }
#endif
#ifdef HAVE_PHYSICAL_DS5_HAPTICS
        if (mode == Mode::Physical) {
            worker = std::thread([this] { run(); });
        }
#endif
    }

#ifdef HAVE_PHYSICAL_DS5_HAPTICS
    ~Impl()
    {
        {
            // Under the mutex, or the notify could land before the worker waits.
            std::lock_guard lock(mutex);
            stopping = true;
        }
        condition.notify_all();
        if (worker.joinable()) {
            worker.join();
        }
    }

    void resetAudioStream()
    {
        endpoint.reset();
        prebuffer.clear();
        prebufferedFrames = 0;
    }

    void resetStream()
    {
        resetAudioStream();
        streamTracker.reset();
    }

    void requestReset()
    {
        {
            std::lock_guard lock(mutex);
            queue.clear();
            resetRequested = true;
        }
        condition.notify_one();
    }

    // The endpoint failed (device unplugged, format renegotiated, ...). Drop it
    // so the next packet probes for it again.
    void failEndpoint()
    {
        resetStream();
        endpoint.close();
    }

    // Hand one packet to the endpoint, waiting while it simply has no room yet.
    bool writePacket(const Packet& packet)
    {
        const auto deadline = std::chrono::steady_clock::now() + EndpointWriteTimeout;
        for (;;) {
            const auto result = endpoint.tryWrite(packet);
            if (result == WriteResult::Ok)
                return true;
            if (result == WriteResult::Failed)
                return false;

            if (stopping)
                return false;
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO,
                            "DualSense haptics endpoint stopped draining; reopening it");
                // A wedged endpoint is likely still enumerable, so don't
                // reopen it on the very next packet.
                nextEndpointProbe = now + EndpointProbeBackoff;
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    bool startPrebufferedStream()
    {
        for (const auto& buffered : prebuffer) {
            if (!writePacket(buffered)) return false;
        }
        prebuffer.clear();
        prebufferedFrames = 0;
        return endpoint.start();
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

        if (!endpoint.isOpen()) {
            const auto now = std::chrono::steady_clock::now();
            if (now < nextEndpointProbe) {
                return;
            }
            // Drop anything prebuffered for a device that went away.
            resetAudioStream();
            if (!endpoint.open()) {
                nextEndpointProbe = now + EndpointProbeBackoff;
                return;
            }
        }

        if (!endpoint.isStarted()) {
            if (packet.frameCount > endpoint.bufferFrames()) {
                SDL_LogError(
                    SDL_LOG_CATEGORY_AUDIO,
                    "DualSense haptics packet (%u frames) exceeds the endpoint buffer (%u frames)",
                    packet.frameCount, endpoint.bufferFrames());
                failEndpoint();
                return;
            }

            // A shared-mode endpoint may expose less than our preferred 15 ms
            // jitter buffer. Never queue more than the endpoint can accept before
            // starting it, or the write would wait for a device that is not running.
            if (dualsense_haptics::mustStartBeforePrebuffering(
                    prebufferedFrames, prebuffer.size(), packet.frameCount, endpoint.bufferFrames(),
                    endpoint.maxQueuedPackets())) {
                if (!startPrebufferedStream() || !writePacket(packet)) {
                    failEndpoint();
                }
                return;
            }

            prebufferedFrames += packet.frameCount;
            prebuffer.emplace_back(std::move(packet));
            if (!dualsense_haptics::isPrebufferFull(prebufferedFrames, endpoint.bufferFrames()))
                return;

            if (!startPrebufferedStream()) {
                failEndpoint();
            }
        } else if (!writePacket(packet)) {
            failEndpoint();
        }
    }

    void run()
    {
        if (!endpoint.threadInit()) {
            return;
        }

        while (!stopping) {
            Packet packet;
            {
                std::unique_lock lock(mutex);
                condition.wait(lock,
                               [this] { return stopping || resetRequested || !queue.empty(); });
                if (stopping) break;
                if (resetRequested) {
                    resetRequested = false;
                    lock.unlock();
                    // Silence the coils now: no stream-end packet is coming to
                    // do it for us. Reopen on the next packet too, since the
                    // connected controllers may have changed.
                    resetStream();
                    endpoint.close();
                    continue;
                }
                packet = std::move(queue.front());
                queue.pop_front();
            }
            process(std::move(packet));
        }

        resetStream();
        endpoint.close();
        endpoint.threadCleanup();
    }
#endif
};

DualSenseHapticsRenderer::DualSenseHapticsRenderer(Mode mode) : m_Impl(std::make_unique<Impl>(mode))
{
}
DualSenseHapticsRenderer::~DualSenseHapticsRenderer() = default;

DualSenseHapticsRenderer::Availability DualSenseHapticsRenderer::availability()
{
    return probeHapticsEndpoint();
}

void DualSenseHapticsRenderer::submit(const LI_DS5_HAPTICS_PCM_FRAME& frame)
{
#ifdef HAVE_PHYSICAL_DS5_HAPTICS
    if (frame.controllerNumber != m_Impl->controllerTarget) {
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
#else
    (void)frame;
#endif
}

void DualSenseHapticsRenderer::setControllerTarget(int controllerNumber)
{
    Q_UNUSED(controllerNumber);
#ifdef HAVE_PHYSICAL_DS5_HAPTICS
    if (m_Impl->controllerTarget.exchange(controllerNumber) != controllerNumber) {
        m_Impl->requestReset();
    }
#endif
#ifdef Q_OS_MACOS
    if (m_Impl->macRenderer != nullptr) {
        m_Impl->macRenderer->setControllerTarget(controllerNumber);
    }
#endif
}

void DualSenseHapticsRenderer::reset()
{
#ifdef Q_OS_MACOS
    if (m_Impl->macRenderer != nullptr) {
        m_Impl->macRenderer->reset();
    }
#endif

#ifdef HAVE_PHYSICAL_DS5_HAPTICS
    // Drop anything the host queued before the connection died.
    m_Impl->requestReset();
#endif
}

bool DualSenseHapticsRenderer::submit(const LI_DS5_HAPTICS_IR_FRAME_V2& frame,
                                      bool& startedNative)
{
#ifdef Q_OS_MACOS
    return m_Impl->macRenderer != nullptr &&
           m_Impl->macRenderer->submit(frame, startedNative);
#else
    (void)frame;
    startedNative = false;
    return false;
#endif
}
