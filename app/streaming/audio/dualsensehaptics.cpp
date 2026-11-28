#include "dualsensehaptics.h"
#include "dualsensehapticsstream.h"
#include "dualsensehapticscontrol.h"

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
#include <string>
#include <thread>
#include <utility>
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
#include <setupapi.h>
#include <hidsdi.h>
#include <wrl/client.h>

#include "dualsensehapticsbt.h"

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

constexpr auto ControlRetryBackoff = std::chrono::milliseconds(50);

struct Packet
{
    std::uint8_t flags = 0;
    std::uint8_t channelCount = 2;
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

// Lookup and use the SDL handle under SDL's joystick lock, so unplug/close cannot
// invalidate it. No renderer mutex is held while SDL queues the physical report.
bool sendHapticsEffect(int instanceId, const dualsense_haptics::ControlState::Effect& effect,
                       bool takeOwnership = false)
{
#if SDL_VERSION_ATLEAST(2, 0, 16)
    SDL_LockJoysticks();
    auto* controller = SDL_GameControllerFromInstanceID(instanceId);
    bool success = controller != nullptr && SDL_GameControllerGetAttached(controller) &&
                   SDL_GameControllerGetType(controller) == SDL_CONTROLLER_TYPE_PS5;
    if (success) {
        // Cancel SDL's old compatibility-rumble timer/cache before taking over.
        if (takeOwnership)
            SDL_GameControllerRumble(controller, 0, 0, 0);
        success = SDL_GameControllerSendEffect(controller, effect.data(),
                                               static_cast<int>(effect.size())) == 0;
    }
    SDL_UnlockJoysticks();
    return success;
#else
    Q_UNUSED(instanceId);
    Q_UNUSED(effect);
    Q_UNUSED(takeOwnership);
    return false;
#endif
}

void releaseHapticsControl(int instanceId)
{
    dualsense_haptics::ControlState::Effect neutral{};
    neutral[0] = 3 | 4 | 8;
    neutral[1] = 0x62;
    neutral[10] = neutral[21] = 5; // Explicitly release both adaptive triggers.
    sendHapticsEffect(instanceId, neutral);
}

// A DualShock 4 also calls itself "Wireless Controller", but it exposes a
// two-channel endpoint, so the channel count check rules it out.
bool isDualSenseName(const QString& name)
{
    return name.contains(QLatin1String("dualsense"), Qt::CaseInsensitive) ||
           name.contains(QLatin1String("wireless controller"), Qt::CaseInsensitive) ||
           name.contains(QLatin1String("hidmaestro"), Qt::CaseInsensitive);
}

using dualsense_haptics::EndpointChannelCount;
using dualsense_haptics::writePcmChannels;

// Interleaved 16-bit PCM, with either two haptic or four endpoint channels.
const std::int16_t* pcmSamples(const Packet& packet)
{
    return reinterpret_cast<const std::int16_t*>(packet.pcm.data());
}

// Both endpoints only ever run at 48 kHz.
constexpr double framesToMs(std::uint32_t frames)
{
    return frames / 48.0;
}

// A delay for the endpoint-ready log, or "unknown" if the device did not report it.
QString describeDelay(bool known, double ms)
{
    return known ? QStringLiteral("%1 ms").arg(ms, 0, 'f', 1) : QStringLiteral("unknown");
}

#ifdef MOONLIGHT_DS5_TEST_ENDPOINT
using HapticsEndpoint = MOONLIGHT_DS5_TEST_ENDPOINT;
bool probeHapticsEndpoint()
{
    return true;
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
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
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

            // What the engine reports on top of our buffer. The device period is
            // how often the shared-mode engine takes data from it.
            REFERENCE_TIME streamLatency = 0;
            REFERENCE_TIME devicePeriod = 0;
            const bool streamLatencyKnown = SUCCEEDED(candidate->GetStreamLatency(&streamLatency));
            const bool devicePeriodKnown =
                SUCCEEDED(candidate->GetDevicePeriod(&devicePeriod, nullptr));
            const double streamLatencyMs = streamLatency / 10000.0;

            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "DualSense haptics endpoint ready: %s (48 kHz, 4 ch, %u-bit%s; "
                        "latency <= %s: queue <= %.1f ms, stream latency %s, device period %s)",
                        qUtf8Printable(name), m_BitsPerSample, m_FloatSamples ? " float" : " PCM",
                        qPrintable(describeDelay(streamLatencyKnown,
                                                 framesToMs(m_BufferFrames) + streamLatencyMs)),
                        framesToMs(m_BufferFrames),
                        qPrintable(describeDelay(streamLatencyKnown, streamLatencyMs)),
                        qPrintable(describeDelay(devicePeriodKnown, devicePeriod / 10000.0)));
            return true;
        });

        // No warning here: WindowsHapticsEndpoint tries Bluetooth next and
        // reports when neither transport is available.
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

    // The audio device plays everything it was given; nothing is held back.
    bool finishStream() { return true; }

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
            writePcmChannels(reinterpret_cast<float*>(output), pcmSamples(packet),
                             packet.frameCount, packet.channelCount,
                             [](std::int16_t s) { return s / 32768.0f; });
        } else if (m_BitsPerSample == 16) {
            writePcmChannels(reinterpret_cast<std::int16_t*>(output), pcmSamples(packet),
                             packet.frameCount, packet.channelCount,
                             [](std::int16_t s) { return s; });
        } else {
            writePcmChannels(reinterpret_cast<std::int32_t*>(output), pcmSamples(packet),
                             packet.frameCount, packet.channelCount,
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

// A DualSense connected over Bluetooth has no audio endpoint. Its haptics PCM
// goes out as HID output report 0x34 instead (see dualsensehapticsbt.h).
//
// SDL keeps its own handle to the same HID device for input, LEDs and adaptive
// triggers (report 0x31); both handles are opened shared, and the controller
// accepts the two report streams side by side.
//
// Reports are sent as soon as enough PCM has arrived, so the host's audio clock
// paces them. That is how ds5-bridge drives the controller in production.

// Set by the session from the user's preference before a stream starts.
std::atomic<float> g_BluetoothHapticsGain{ dualsense_haptics::bt::DefaultGain };

class BtHidHapticsEndpoint
{
public:
    ~BtHidHapticsEndpoint() { close(); }

    struct Device
    {
        std::wstring path;
        USHORT productId = 0;
        USHORT outputReportLength = 0;
    };

    // Call visit() for each present DualSense HID collection that is connected
    // over Bluetooth, until it accepts one. Bluetooth is told apart from USB by
    // the output report length: USB reports are 64 bytes at most, while the
    // Bluetooth collection carries reports 0x31-0x39 of up to 547 bytes.
    template <typename Visit> static bool findBluetoothDualSense(Visit visit)
    {
        GUID hidGuid;
        HidD_GetHidGuid(&hidGuid);
        HDEVINFO devices =
            SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (devices == INVALID_HANDLE_VALUE)
            return false;

        bool found = false;
        SP_DEVICE_INTERFACE_DATA interfaceData{};
        interfaceData.cbSize = sizeof(interfaceData);
        for (DWORD index = 0; !found && SetupDiEnumDeviceInterfaces(devices, nullptr, &hidGuid,
                                                                    index, &interfaceData);
             index++) {
            DWORD detailSize = 0;
            SetupDiGetDeviceInterfaceDetailW(devices, &interfaceData, nullptr, 0, &detailSize,
                                             nullptr);
            if (detailSize == 0)
                continue;
            std::vector<std::uint8_t> detailBuffer(detailSize);
            auto* detail =
                reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detailBuffer.data());
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            if (!SetupDiGetDeviceInterfaceDetailW(devices, &interfaceData, detail, detailSize,
                                                  nullptr, nullptr)) {
                continue;
            }

            // No access rights are needed to read attributes and capabilities.
            HANDLE handle = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        nullptr, OPEN_EXISTING, 0, nullptr);
            if (handle == INVALID_HANDLE_VALUE)
                continue;

            Device device;
            HIDD_ATTRIBUTES attributes{};
            attributes.Size = sizeof(attributes);
            PHIDP_PREPARSED_DATA preparsed = nullptr;
            HIDP_CAPS caps{};
            const bool isDualSense =
                HidD_GetAttributes(handle, &attributes) && attributes.VendorID == 0x054C &&
                (attributes.ProductID == 0x0CE6 || attributes.ProductID == 0x0DF2);
            if (isDualSense && HidD_GetPreparsedData(handle, &preparsed)) {
                if (HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS &&
                    caps.OutputReportByteLength >= dualsense_haptics::bt::ReportLength) {
                    device.path = detail->DevicePath;
                    device.productId = attributes.ProductID;
                    device.outputReportLength = caps.OutputReportByteLength;
                }
                HidD_FreePreparsedData(preparsed);
            }
            CloseHandle(handle);

            if (!device.path.empty())
                found = visit(device);
        }
        SetupDiDestroyDeviceInfoList(devices);
        return found;
    }

    bool open()
    {
        close();
        const bool found = findBluetoothDualSense([this](const Device& device) {
            HANDLE handle =
                CreateFileW(device.path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            if (handle == INVALID_HANDLE_VALUE) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Unable to open Bluetooth DualSense for haptics: %lu",
                            static_cast<unsigned long>(GetLastError()));
                return false;
            }
            m_Device = handle;
            m_Write = std::make_unique<WriteSlot>();
            m_Write->buffer.assign(device.outputReportLength, 0);
            return true;
        });
        if (!found)
            return false;

        m_Write->event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (m_Write->event == nullptr) {
            close();
            return false;
        }
        const float gain = g_BluetoothHapticsGain.load();
        m_Assembler.reset();
        m_Assembler.setGain(gain);
        m_ConsecutiveFailures = 0;
        SDL_LogInfo(
            SDL_LOG_CATEGORY_APPLICATION,
            "DualSense haptics endpoint ready: Bluetooth HID report 0x34 "
            "(%u-byte output reports, %u frames per report, ~%.0f Hz signed 8-bit, gain %.1f)",
            static_cast<unsigned>(m_Write->buffer.size()),
            static_cast<unsigned>(dualsense_haptics::bt::FramesPerReport),
            dualsense_haptics::bt::InputRate * dualsense_haptics::bt::FramesPerReport /
                dualsense_haptics::bt::InputFramesPerReport,
            gain);
        return true;
    }

    void close()
    {
        m_Started = false;
        if (m_Device != INVALID_HANDLE_VALUE) {
            if (m_Write && m_Write->pending) {
                CancelIoEx(m_Device, &m_Write->overlapped);
                if (!completePendingWrite(CloseWaitMs)) {
                    // The driver ignored the cancel and may still touch the
                    // OVERLAPPED and buffer, so they must never be freed.
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "Bluetooth DualSense haptics write did not finish after "
                                "cancellation; abandoning its buffer");
                    m_Write.release();
                }
            }
            CloseHandle(m_Device);
            m_Device = INVALID_HANDLE_VALUE;
        }
        m_Write.reset();
        m_Assembler.reset();
    }

    bool isOpen() const { return m_Device != INVALID_HANDLE_VALUE; }
    bool isStarted() const { return m_Started; }

    // Reports go out as soon as they are complete, so this only bounds what the
    // worker may prebuffer before start(): 200 ms at 48 kHz.
    std::uint32_t bufferFrames() const { return 9600; }
    std::uint32_t maxQueuedPackets() const { return 0; }

    bool start()
    {
        m_Started = isOpen();
        return m_Started;
    }

    // Whatever already reached the controller plays out; drop the partial report.
    void reset()
    {
        m_Started = false;
        m_Assembler.reset();
    }

    // The host ended the stream: send the partial report instead of holding it
    // back until the next stream discards it.
    bool finishStream()
    {
        return isOpen() && m_Assembler.flush(
                               [this](const std::uint8_t* report) { return writeReport(report); });
    }

    WriteResult tryWrite(const Packet& packet)
    {
        if (!isOpen())
            return WriteResult::Failed;
        if (packet.frameCount == 0)
            return WriteResult::Ok;

        const bool ok =
            m_Assembler.push(pcmSamples(packet), packet.frameCount,
                             [this](const std::uint8_t* report) { return writeReport(report); });
        return ok ? WriteResult::Ok : WriteResult::Failed;
    }

private:
    // A single slow write is tolerated; repeated failures mean the controller
    // went away (powered off, out of range) and the endpoint must be reopened.
    static constexpr int MaxConsecutiveFailures = 3;
    static constexpr DWORD WriteTimeoutMs = 100;
    static constexpr DWORD CancelWaitMs = 20;
    static constexpr DWORD CloseWaitMs = 500;

    // One overlapped write at a time. The OVERLAPPED and buffer belong to the
    // write until the driver completes it, so the worker never waits unbounded:
    // a write that ignores cancellation keeps the slot busy (later reports fail
    // until it completes) and close() abandons the slot rather than free it.
    struct WriteSlot
    {
        OVERLAPPED overlapped{};
        std::vector<std::uint8_t> buffer;
        HANDLE event = nullptr;
        bool pending = false;

        WriteSlot() = default;
        WriteSlot(const WriteSlot&) = delete;
        WriteSlot& operator=(const WriteSlot&) = delete;
        ~WriteSlot()
        {
            if (event != nullptr)
                CloseHandle(event);
        }
    };

    // Wait up to timeoutMs for the write in flight; true once the slot is free.
    bool completePendingWrite(DWORD timeoutMs)
    {
        if (WaitForSingleObject(m_Write->event, timeoutMs) != WAIT_OBJECT_0)
            return false;
        DWORD written = 0;
        GetOverlappedResult(m_Device, &m_Write->overlapped, &written, FALSE);
        m_Write->pending = false;
        return true;
    }

    bool issueWrite(const std::uint8_t* report)
    {
        if (m_Write->pending && !completePendingWrite(0))
            return false;

        std::copy_n(report, dualsense_haptics::bt::ReportLength, m_Write->buffer.begin());
        // Bytes past the report stay zero: Windows wants the collection's
        // longest output report length on every write.

        m_Write->overlapped = OVERLAPPED{};
        m_Write->overlapped.hEvent = m_Write->event;
        ResetEvent(m_Write->event);
        DWORD written = 0;
        if (WriteFile(m_Device, m_Write->buffer.data(), static_cast<DWORD>(m_Write->buffer.size()),
                      nullptr, &m_Write->overlapped))
            return GetOverlappedResult(m_Device, &m_Write->overlapped, &written, FALSE) != FALSE;
        if (GetLastError() != ERROR_IO_PENDING)
            return false;

        m_Write->pending = true;
        if (WaitForSingleObject(m_Write->event, WriteTimeoutMs) == WAIT_OBJECT_0) {
            m_Write->pending = false;
            return GetOverlappedResult(m_Device, &m_Write->overlapped, &written, FALSE) != FALSE;
        }
        CancelIoEx(m_Device, &m_Write->overlapped);
        completePendingWrite(CancelWaitMs);
        return false;
    }

    bool writeReport(const std::uint8_t* report)
    {
        if (issueWrite(report)) {
            m_ConsecutiveFailures = 0;
            return true;
        }
        const DWORD error = GetLastError();
        if (++m_ConsecutiveFailures < MaxConsecutiveFailures)
            return true;
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Bluetooth DualSense stopped accepting haptics reports: %lu",
                    static_cast<unsigned long>(error));
        return false;
    }

    HANDLE m_Device = INVALID_HANDLE_VALUE;
    std::unique_ptr<WriteSlot> m_Write;
    dualsense_haptics::bt::ReportAssembler m_Assembler;
    int m_ConsecutiveFailures = 0;
    bool m_Started = false;
};

// USB first: its audio endpoint plays the authored PCM at the full 48 kHz.
// Bluetooth is the fallback for a controller without one.
class WindowsHapticsEndpoint
{
public:
    bool threadInit() { return m_Usb.threadInit(); }
    void threadCleanup() { m_Usb.threadCleanup(); }

    bool open()
    {
        close();
        if (m_Usb.open()) {
            m_Active = Active::Usb;
        } else if (m_Bt.open()) {
            m_Active = Active::Bluetooth;
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "No DualSense haptics output was found (neither a 48 kHz four-channel "
                        "USB audio endpoint nor a Bluetooth-connected DualSense)");
            return false;
        }
        return true;
    }

    void close()
    {
        m_Usb.close();
        m_Bt.close();
        m_Active = Active::None;
    }

    bool isOpen() const { return m_Active != Active::None; }
    bool isStarted() const
    {
        return m_Active == Active::Usb         ? m_Usb.isStarted()
               : m_Active == Active::Bluetooth ? m_Bt.isStarted()
                                               : false;
    }
    std::uint32_t bufferFrames() const
    {
        return m_Active == Active::Usb         ? m_Usb.bufferFrames()
               : m_Active == Active::Bluetooth ? m_Bt.bufferFrames()
                                               : 0;
    }
    std::uint32_t maxQueuedPackets() const
    {
        return m_Active == Active::Usb         ? m_Usb.maxQueuedPackets()
               : m_Active == Active::Bluetooth ? m_Bt.maxQueuedPackets()
                                               : 0;
    }
    bool start()
    {
        return m_Active == Active::Usb         ? m_Usb.start()
               : m_Active == Active::Bluetooth ? m_Bt.start()
                                               : false;
    }
    void reset()
    {
        if (m_Active == Active::Usb)
            m_Usb.reset();
        else if (m_Active == Active::Bluetooth)
            m_Bt.reset();
    }
    bool finishStream()
    {
        return m_Active == Active::Usb         ? m_Usb.finishStream()
               : m_Active == Active::Bluetooth ? m_Bt.finishStream()
                                               : true;
    }
    WriteResult tryWrite(const Packet& packet)
    {
        return m_Active == Active::Usb         ? m_Usb.tryWrite(packet)
               : m_Active == Active::Bluetooth ? m_Bt.tryWrite(packet)
                                               : WriteResult::Failed;
    }

private:
    enum class Active
    {
        None,
        Usb,
        Bluetooth
    };
    WasapiHapticsEndpoint m_Usb;
    BtHidHapticsEndpoint m_Bt;
    Active m_Active = Active::None;
};

using HapticsEndpoint = WindowsHapticsEndpoint;

bool probeBluetoothHapticsEndpoint()
{
    return BtHidHapticsEndpoint::findBluetoothDualSense(
        [](const BtHidHapticsEndpoint::Device&) { return true; });
}

bool probeHapticsEndpoint()
{
    if (probeBluetoothHapticsEndpoint())
        return true;

    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool shouldUninitialize = SUCCEEDED(comResult);
    if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE) {
        return false;
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
    return found;
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

bool findEndpointDevice(AudioDeviceID* outDevice, QString* outName)
{
    AudioObjectPropertyAddress addr{ kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, nullptr, &size) !=
            noErr ||
        size == 0) {
        return false;
    }

    std::vector<AudioDeviceID> devices(size / sizeof(AudioDeviceID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size,
                                   devices.data()) != noErr) {
        return false;
    }

    for (AudioDeviceID device : devices) {
        if (!isUsbDevice(device) || outputChannelCount(device) != EndpointChannelCount ||
            !hasHapticsSampleRate(device)) {
            continue;
        }

        const QString name = deviceName(device);
        if (!isDualSenseName(name))
            continue;

        if (outDevice != nullptr)
            *outDevice = device;
        if (outName != nullptr)
            *outName = name;
        return true;
    }
    return false;
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
        if (!findEndpointDevice(&device, &name)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "No active 48 kHz four-channel DualSense audio endpoint was found");
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

        // The device's own delay on top of our queue, read the way the CoreAudio
        // audio renderer reads it: the I/O buffer and safety offset on the
        // software side, the device latency on the hardware side.
        UInt32 ioBufferFrames = 0;
        UInt32 safetyOffsetFrames = 0;
        UInt32 latencyFrames = 0;
        const bool ioBufferKnown =
            getDeviceProperty(device, kAudioDevicePropertyBufferFrameSize,
                              kAudioObjectPropertyScopeOutput, ioBufferFrames);
        const bool safetyOffsetKnown =
            getDeviceProperty(device, kAudioDevicePropertySafetyOffset,
                              kAudioObjectPropertyScopeOutput, safetyOffsetFrames);
        const bool latencyKnown = getDeviceProperty(device, kAudioDevicePropertyLatency,
                                                    kAudioObjectPropertyScopeOutput, latencyFrames);
        const std::uint32_t totalFrames =
            MaxQueuedFrames + ioBufferFrames + safetyOffsetFrames + latencyFrames;

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "DualSense haptics endpoint ready: %s (48 kHz, 4 ch, 32-bit float; "
                    "latency <= %s: queue <= %.1f ms, I/O buffer %s, safety offset %s, "
                    "device %s)",
                    qUtf8Printable(name),
                    qPrintable(describeDelay(ioBufferKnown && safetyOffsetKnown && latencyKnown,
                                             framesToMs(totalFrames))),
                    framesToMs(MaxQueuedFrames),
                    qPrintable(describeDelay(ioBufferKnown, framesToMs(ioBufferFrames))),
                    qPrintable(describeDelay(safetyOffsetKnown, framesToMs(safetyOffsetFrames))),
                    qPrintable(describeDelay(latencyKnown, framesToMs(latencyFrames))));
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

    // The queue plays everything it was given; nothing is held back.
    bool finishStream() { return true; }

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

        writePcmChannels(static_cast<float*>(buffer->mAudioData), pcmSamples(packet),
                         packet.frameCount, packet.channelCount,
                         [](std::int16_t s) { return s / 32768.0f; });
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

bool probeHapticsEndpoint()
{
    return findEndpointDevice(nullptr, nullptr);
}

#endif
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
    // One-shot diagnostics, so a silent haptics stream can be told apart from
    // one that is being filtered out on the client.
    std::atomic_bool loggedFirstPcm{ false };
    std::atomic_bool loggedTargetMismatch{ false };
    std::atomic_bool loggedBadFormat{ false };
    int joystickTarget = -1;
    dualsense_haptics::ControlState controls;
    bool outputDirty = false;

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

    void requestReset(bool clearControls = false)
    {
        {
            std::lock_guard lock(mutex);
            queue.clear();
            resetRequested = true;
            if (clearControls)
                controls.reset();
            outputDirty = true;
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
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
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
                                                  packet.sequenceNumber, packet.frameCount);
        if (action == dualsense_haptics::PcmStreamTracker::Action::Ignore) {
            return;
        }
        if (action == dualsense_haptics::PcmStreamTracker::Action::End) {
            // Let the endpoint consume its tail. Reset() would discard both the
            // short prebuffer and samples already queued in the OS audio device.
            if (!prebuffer.empty() && !startPrebufferedStream())
                failEndpoint();
            else
                finishStream();
            return;
        }
        if (action == dualsense_haptics::PcmStreamTracker::Action::ResetAndAccept) {
            resetAudioStream();
        }
        const bool streamEnd = packet.flags & LI_DS5_HAPTICS_PCM_FLAG_STREAM_END;

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
                    SDL_LOG_CATEGORY_APPLICATION,
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
            if (!dualsense_haptics::isPrebufferFull(prebufferedFrames, endpoint.bufferFrames()) &&
                !streamEnd)
                return;

            if (!startPrebufferedStream()) {
                failEndpoint();
                return;
            }
        } else if (!writePacket(packet)) {
            failEndpoint();
            return;
        }
        if (streamEnd)
            finishStream();
    }

    // Send whatever the endpoint still holds back for a stream that just ended.
    void finishStream()
    {
        if (endpoint.isStarted() && !endpoint.finishStream())
            failEndpoint();
    }

    void run()
    {
        if (!endpoint.threadInit()) {
            return;
        }

        int ownedJoystick = -1;
        bool controlApplied = false;
        bool retryControl = false;
        std::chrono::steady_clock::time_point controlRetryDeadline;
        auto prebufferDeadline = std::chrono::steady_clock::now();
        while (!stopping) {
            Packet packet;
            bool hasPacket = false;
            bool reset = false;
            bool flushPrebuffer = false;
            int targetJoystick;
            std::optional<dualsense_haptics::ControlState::Effect> effect;
            {
                std::unique_lock lock(mutex);
                const auto ready = [this] {
                    return stopping || resetRequested || outputDirty || !queue.empty();
                };
                // Coalesce new output while backing off, but let retarget/reset/stop
                // interrupt the retry immediately. PCM stays in the bounded queue.
                if (retryControl) {
                    condition.wait_until(lock, controlRetryDeadline,
                                         [this] { return stopping || resetRequested; });
                } else if (prebuffer.empty())
                    condition.wait(lock, ready);
                else
                    flushPrebuffer = !condition.wait_until(lock, prebufferDeadline, ready);
                if (stopping) break;
                reset = std::exchange(resetRequested, false);
                targetJoystick = joystickTarget;
                if (controls.has(controllerTarget) && (outputDirty || !controlApplied || reset)) {
                    effect = controls.effect(controllerTarget);
                }
                outputDirty = false;
            }
            if (reset) {
                resetStream();
                endpoint.close();
                if (ownedJoystick >= 0)
                    releaseHapticsControl(ownedJoystick);
                ownedJoystick = -1;
                controlApplied = false;
            }
            if (effect) {
                if (!sendHapticsEffect(targetJoystick, *effect, ownedJoystick != targetJoystick)) {
                    controlApplied = false;
                    retryControl = true;
                    controlRetryDeadline = std::chrono::steady_clock::now() + ControlRetryBackoff;
                    continue;
                }
                ownedJoystick = targetJoystick;
                controlApplied = true;
            }
            retryControl = false;
            {
                std::lock_guard lock(mutex);
                // Output or routing may have changed while SDL was sending. Apply
                // that change before taking PCM out of the queue.
                if (stopping || resetRequested || outputDirty)
                    continue;
                if (!queue.empty()) {
                    packet = std::move(queue.front());
                    queue.pop_front();
                    hasPacket = true;
                    prebufferDeadline =
                        std::chrono::steady_clock::now() + std::chrono::milliseconds(8);
                }
            }
            if (hasPacket)
                process(std::move(packet));
            else if (flushPrebuffer && !prebuffer.empty() && !startPrebufferedStream())
                failEndpoint();
        }

        resetStream();
        endpoint.close();
        if (ownedJoystick >= 0)
            releaseHapticsControl(ownedJoystick);
        endpoint.threadCleanup();
    }
#endif
};

DualSenseHapticsRenderer::DualSenseHapticsRenderer(Mode mode) : m_Impl(std::make_unique<Impl>(mode))
{
}
DualSenseHapticsRenderer::~DualSenseHapticsRenderer() = default;

bool DualSenseHapticsRenderer::isAvailable()
{
#ifdef HAVE_PHYSICAL_DS5_HAPTICS
    return probeHapticsEndpoint();
#else
    return false;
#endif
}

void DualSenseHapticsRenderer::submit(const LI_DS5_HAPTICS_PCM_FRAME& frame)
{
#ifdef HAVE_PHYSICAL_DS5_HAPTICS
    if (!m_Impl->loggedFirstPcm.exchange(true)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "First DualSense haptics PCM from host: controller %u (target %d), %u Hz, "
                    "%u ch, %u-bit, %u frames, flags 0x%x",
                    frame.controllerNumber, m_Impl->controllerTarget.load(), frame.sampleRate,
                    frame.channelCount, frame.bitsPerSample, frame.frameCount, frame.flags);
    }

    if (frame.controllerNumber != m_Impl->controllerTarget) {
        if (!m_Impl->loggedTargetMismatch.exchange(true)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Dropping DualSense haptics PCM for controller %u: local target is %d",
                        frame.controllerNumber, m_Impl->controllerTarget.load());
        }
        return;
    }

    if (frame.sampleRate != 48000 || (frame.channelCount != 2 && frame.channelCount != 4) ||
        frame.bitsPerSample != 16 || frame.frameCount > 480 ||
        frame.pcmDataLength != frame.frameCount * frame.channelCount * sizeof(std::int16_t) ||
        (frame.pcmDataLength != 0 && frame.pcmData == nullptr)) {
        if (!m_Impl->loggedBadFormat.exchange(true)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Dropping DualSense haptics PCM with unsupported format: %u Hz, %u ch, "
                        "%u-bit, %u frames, %u bytes",
                        frame.sampleRate, frame.channelCount, frame.bitsPerSample, frame.frameCount,
                        frame.pcmDataLength);
        }
        return;
    }

    Packet packet;
    packet.flags = frame.flags;
    packet.channelCount = frame.channelCount;
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

void DualSenseHapticsRenderer::updateControl(std::uint16_t controllerNumber,
                                             const std::uint8_t* state)
{
#ifdef HAVE_PHYSICAL_DS5_HAPTICS
    {
        std::lock_guard lock(m_Impl->mutex);
        if (m_Impl->stopping || !m_Impl->controls.update(controllerNumber, state))
            return;
        if (controllerNumber != m_Impl->controllerTarget)
            return;
        m_Impl->outputDirty = true;
    }
    m_Impl->condition.notify_one();
#else
    Q_UNUSED(controllerNumber);
    Q_UNUSED(state);
#endif
}

void DualSenseHapticsRenderer::updateOutput(const LI_DS5_OUTPUT_EXTENSION& output)
{
#ifdef HAVE_PHYSICAL_DS5_HAPTICS
    {
        std::lock_guard lock(m_Impl->mutex);
        if (m_Impl->stopping || !m_Impl->controls.updateOutput(output))
            return;
        if (output.controllerNumber != m_Impl->controllerTarget)
            return;
        m_Impl->outputDirty = true;
    }
    m_Impl->condition.notify_one();
#else
    Q_UNUSED(output);
#endif
}

bool DualSenseHapticsRenderer::hasControl(std::uint16_t controllerNumber) const
{
#ifdef HAVE_PHYSICAL_DS5_HAPTICS
    std::lock_guard lock(m_Impl->mutex);
    return !m_Impl->stopping && controllerNumber == m_Impl->controllerTarget &&
           m_Impl->controls.has(controllerNumber);
#else
    Q_UNUSED(controllerNumber);
    return false;
#endif
}

bool DualSenseHapticsRenderer::submitEffect(std::uint16_t controllerNumber, const void* effect,
                                            std::size_t size)
{
#ifdef HAVE_PHYSICAL_DS5_HAPTICS
    if (effect == nullptr || size != dualsense_haptics::ControlState::Effect{}.size())
        return false;
    dualsense_haptics::ControlState::Effect snapshot;
    std::memcpy(snapshot.data(), effect, size);
    {
        std::lock_guard lock(m_Impl->mutex);
        if (m_Impl->stopping || controllerNumber != m_Impl->controllerTarget ||
            !m_Impl->controls.mergeEffect(controllerNumber, snapshot))
            return false;
        m_Impl->outputDirty = true;
    }
    m_Impl->condition.notify_one();
    return true;
#else
    Q_UNUSED(controllerNumber);
    Q_UNUSED(effect);
    Q_UNUSED(size);
    return false;
#endif
}

void DualSenseHapticsRenderer::setBluetoothGain(float gain)
{
#if defined(HAVE_PHYSICAL_DS5_HAPTICS) && defined(Q_OS_WIN32) &&                                   \
    !defined(MOONLIGHT_DS5_TEST_ENDPOINT)
    g_BluetoothHapticsGain = std::clamp(gain, 0.5f, 4.0f);
#else
    Q_UNUSED(gain);
#endif
}

void DualSenseHapticsRenderer::setControllerTarget(int controllerNumber, int joystickInstanceId)
{
    Q_UNUSED(controllerNumber);
    Q_UNUSED(joystickInstanceId);
#ifdef HAVE_PHYSICAL_DS5_HAPTICS
    {
        std::lock_guard lock(m_Impl->mutex);
        if (m_Impl->controllerTarget == controllerNumber &&
            m_Impl->joystickTarget == joystickInstanceId)
            return;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "DualSense haptics target: controller %d (joystick %d)", controllerNumber,
                    joystickInstanceId);
        m_Impl->controllerTarget = controllerNumber;
        m_Impl->joystickTarget = joystickInstanceId;
        m_Impl->queue.clear();
        m_Impl->resetRequested = true;
        m_Impl->outputDirty = true;
    }
    m_Impl->condition.notify_one();
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
    m_Impl->requestReset(true);
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
