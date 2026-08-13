#include "dualsensehaptics.h"

#include "SDL_compat.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <cwctype>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include <QtGlobal>

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

#include <QString>

using Microsoft::WRL::ComPtr;
#endif

namespace {
constexpr std::size_t MaxQueuedPackets = 32;
constexpr std::uint32_t PrebufferFrames = 720; // 15 ms at 48 kHz
constexpr auto StreamStarvationTimeout = std::chrono::milliseconds(20);

struct Packet
{
    std::uint8_t flags = 0;
    std::uint16_t controllerNumber = 0;
    std::uint16_t frameCount = 0;
    std::uint32_t sequenceNumber = 0;
    std::vector<std::uint8_t> pcm;
};
}

struct DualSenseHapticsRenderer::Impl
{
    std::mutex mutex;
    std::condition_variable condition;
    std::deque<Packet> queue;
    std::thread worker;
    std::atomic_bool stopping{false};

#ifdef Q_OS_WIN32
    ComPtr<IAudioClient> audioClient;
    ComPtr<IAudioRenderClient> renderClient;
    UINT32 bufferFrames = 0;
    WORD bitsPerSample = 0;
    bool floatSamples = false;
    bool streamStarted = false;
    bool sequenceValid = false;
    std::uint32_t expectedSequence = 0;
    std::uint16_t activeController = 0;
    std::deque<Packet> prebuffer;
    std::uint32_t prebufferedFrames = 0;
    std::chrono::steady_clock::time_point nextEndpointProbe{};

    Impl() : worker([this] { run(); }) {}

    ~Impl()
    {
        stopping = true;
        condition.notify_all();
        if (worker.joinable()) {
            worker.join();
        }
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
        if (format->nSamplesPerSec != 48000 || format->nChannels != 4) {
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

    bool openEndpoint()
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
                FAILED(candidate->GetBufferSize(&bufferFrames))) {
                continue;
            }

            audioClient = candidate;
            renderClient = candidateRenderer;
            floatSamples = candidateFloat;
            bitsPerSample = candidateBits;
            const QByteArray name = QString::fromStdWString(friendlyName).toUtf8();
            SDL_LogInfo(SDL_LOG_CATEGORY_AUDIO,
                        "DualSense haptics endpoint ready: %s (48 kHz, 4 ch, %u-bit%s)",
                        name.constData(), bitsPerSample, floatSamples ? " float" : " PCM");
            return true;
        }

        SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO,
                    "No active 48 kHz four-channel DualSense audio endpoint was found");
        return false;
    }

    void resetStream()
    {
        if (audioClient) {
            if (streamStarted) audioClient->Stop();
            audioClient->Reset();
        }
        streamStarted = false;
        sequenceValid = false;
        prebuffer.clear();
        prebufferedFrames = 0;
    }

    bool writePacket(const Packet& packet)
    {
        if (!audioClient || !renderClient || packet.frameCount == 0) return true;
        while (!stopping) {
            UINT32 padding = 0;
            if (FAILED(audioClient->GetCurrentPadding(&padding))) return false;
            if (bufferFrames - padding >= packet.frameCount) break;
            Sleep(1);
        }
        if (stopping) return false;

        BYTE* output = nullptr;
        if (FAILED(renderClient->GetBuffer(packet.frameCount, &output))) return false;
        const auto* input = reinterpret_cast<const std::int16_t*>(packet.pcm.data());
        if (floatSamples) {
            auto* samples = reinterpret_cast<float*>(output);
            std::fill_n(samples, static_cast<std::size_t>(packet.frameCount) * 4, 0.0f);
            for (std::uint16_t i = 0; i < packet.frameCount; i++) {
                samples[i * 4 + 2] = input[i * 2] / 32768.0f;
                samples[i * 4 + 3] = input[i * 2 + 1] / 32768.0f;
            }
        }
        else if (bitsPerSample == 16) {
            auto* samples = reinterpret_cast<std::int16_t*>(output);
            std::fill_n(samples, static_cast<std::size_t>(packet.frameCount) * 4, 0);
            for (std::uint16_t i = 0; i < packet.frameCount; i++) {
                samples[i * 4 + 2] = input[i * 2];
                samples[i * 4 + 3] = input[i * 2 + 1];
            }
        }
        else {
            auto* samples = reinterpret_cast<std::int32_t*>(output);
            std::fill_n(samples, static_cast<std::size_t>(packet.frameCount) * 4, 0);
            for (std::uint16_t i = 0; i < packet.frameCount; i++) {
                samples[i * 4 + 2] = static_cast<std::int32_t>(input[i * 2]) * 65536;
                samples[i * 4 + 3] = static_cast<std::int32_t>(input[i * 2 + 1]) * 65536;
            }
        }
        return SUCCEEDED(renderClient->ReleaseBuffer(packet.frameCount, 0));
    }

    bool startPrebufferedStream()
    {
        for (const auto& buffered : prebuffer) {
            if (!writePacket(buffered)) return false;
        }
        prebuffer.clear();
        prebufferedFrames = 0;
        if (FAILED(audioClient->Start())) return false;
        streamStarted = true;
        return true;
    }

    void process(Packet packet)
    {
        if (packet.flags & LI_DS5_HAPTICS_PCM_FLAG_STREAM_END) {
            resetStream();
            return;
        }

        const bool discontinuity =
            (packet.flags & (LI_DS5_HAPTICS_PCM_FLAG_STREAM_START |
                             LI_DS5_HAPTICS_PCM_FLAG_DISCONTINUITY)) ||
            (sequenceValid && packet.sequenceNumber != expectedSequence) ||
            (sequenceValid && packet.controllerNumber != activeController);
        if (discontinuity) {
            resetStream();
        }

        activeController = packet.controllerNumber;
        expectedSequence = packet.sequenceNumber + 1;
        sequenceValid = true;

        if (!audioClient) {
            const auto now = std::chrono::steady_clock::now();
            if (now < nextEndpointProbe || !openEndpoint()) {
                nextEndpointProbe = now + std::chrono::seconds(2);
                return;
            }
        }
        if (!streamStarted) {
            if (packet.frameCount > bufferFrames) {
                SDL_LogError(SDL_LOG_CATEGORY_AUDIO,
                             "DualSense haptics packet (%u frames) exceeds the endpoint buffer (%u frames)",
                             packet.frameCount, bufferFrames);
                audioClient.Reset();
                renderClient.Reset();
                resetStream();
                return;
            }

            // A shared-mode endpoint may expose less than our preferred 15 ms
            // jitter buffer. Never queue more than the endpoint can accept before
            // Start(), or writePacket() would wait for a device that is not running.
            if (!prebuffer.empty() && prebufferedFrames + packet.frameCount > bufferFrames) {
                if (!startPrebufferedStream()) {
                    audioClient.Reset();
                    renderClient.Reset();
                    resetStream();
                    return;
                }
                if (!writePacket(packet)) {
                    audioClient.Reset();
                    renderClient.Reset();
                    resetStream();
                }
                return;
            }

            prebufferedFrames += packet.frameCount;
            prebuffer.emplace_back(std::move(packet));
            const auto targetFrames = std::min(PrebufferFrames, bufferFrames);
            if (prebufferedFrames < targetFrames) return;

            if (!startPrebufferedStream()) {
                audioClient.Reset();
                renderClient.Reset();
                resetStream();
                return;
            }
        }
        else if (!writePacket(packet)) {
            audioClient.Reset();
            renderClient.Reset();
            resetStream();
        }
    }

    void run()
    {
        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(comResult)) {
            SDL_LogError(SDL_LOG_CATEGORY_AUDIO,
                         "Unable to initialize COM for DualSense haptics: 0x%08lx",
                         static_cast<unsigned long>(comResult));
            return;
        }

        while (!stopping) {
            Packet packet;
            {
                std::unique_lock lock(mutex);
                if (!condition.wait_for(lock, StreamStarvationTimeout,
                                        [this] { return stopping || !queue.empty(); })) {
                    if (streamStarted || !prebuffer.empty()) {
                        SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO,
                                    "DualSense haptics stream starved; resetting jitter buffer");
                        resetStream();
                    }
                    continue;
                }
                if (stopping) break;
                packet = std::move(queue.front());
                queue.pop_front();
            }
            process(std::move(packet));
        }

        resetStream();
        renderClient.Reset();
        audioClient.Reset();
        CoUninitialize();
    }
#else
    Impl() = default;
    ~Impl() = default;
#endif
};

DualSenseHapticsRenderer::DualSenseHapticsRenderer() : m_Impl(std::make_unique<Impl>()) {}
DualSenseHapticsRenderer::~DualSenseHapticsRenderer() = default;

void DualSenseHapticsRenderer::submit(const LI_DS5_HAPTICS_PCM_FRAME& frame)
{
#ifdef Q_OS_WIN32
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
