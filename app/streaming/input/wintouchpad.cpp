#include "input.h"
#include "wintouchpad.h"

#ifdef HAVE_WINDOWS_RAW_TOUCHPAD

#include <Windows.h>
#include <hidsdi.h>

#include <SDL_syswm.h>
#include <SDL_system.h>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

#ifndef HID_USAGE_DIGITIZER_CONTACT_IDENTIFIER
#define HID_USAGE_DIGITIZER_CONTACT_IDENTIFIER ((USAGE) 0x51)
#endif
#ifndef HID_USAGE_DIGITIZER_CONTACT_COUNT
#define HID_USAGE_DIGITIZER_CONTACT_COUNT ((USAGE) 0x54)
#endif
#ifndef HID_USAGE_DIGITIZER_SCAN_TIME
#define HID_USAGE_DIGITIZER_SCAN_TIME ((USAGE) 0x56)
#endif
#ifndef HID_USAGE_DIGITIZER_CONFIDENCE
#define HID_USAGE_DIGITIZER_CONFIDENCE ((USAGE) 0x47)
#endif
#ifndef HID_USAGE_BUTTON_1
#define HID_USAGE_BUTTON_1 ((USAGE) 0x01)
#endif

namespace {

struct RawContact
{
    uint32_t pointerId;
    float x;
    float y;
    float pressure;
    bool touching;
};

struct PartialContact
{
    bool hasTipSwitch = false;
    bool tipSwitch = false;
    bool hasConfidence = false;
    bool confidence = false;
    bool hasPointerId = false;
    bool hasX = false;
    bool hasY = false;
    bool hasPressure = false;
    uint32_t pointerId = 0;
    ULONG x = 0;
    ULONG y = 0;
    LONG xMin = 0;
    LONG xMax = 0;
    LONG yMin = 0;
    LONG yMax = 0;
    ULONG pressure = 0;
    LONG pressureMin = 0;
    LONG pressureMax = 0;
};

struct ParsedReport
{
    bool hasContactCount = false;
    uint32_t contactCount = 0;
    bool hasScanTime = false;
    uint32_t scanTime = 0;
    bool hasButtonState = false;
    bool buttonDown = false;
    std::vector<RawContact> contacts;
};

template <typename T>
static bool capContainsUsage(const T& cap, USAGE usage)
{
    if (cap.IsRange) {
        return usage >= cap.Range.UsageMin && usage <= cap.Range.UsageMax;
    }

    return cap.NotRange.Usage == usage;
}

static float normalizeAxis(ULONG value, LONG minimum, LONG maximum)
{
    if (maximum <= minimum) {
        return 0.0f;
    }

    double normalized = (static_cast<double>(value) - minimum) /
                        (static_cast<double>(maximum) - minimum);
    return static_cast<float>(std::clamp(normalized, 0.0, 1.0));
}

}

class WindowsTouchpadInput::Impl
{
public:
    struct DeviceState
    {
        HANDLE handle = nullptr;
        std::vector<BYTE> preparsedData;
        std::vector<HIDP_BUTTON_CAPS> buttonCaps;
        std::vector<HIDP_VALUE_CAPS> valueCaps;
        uint32_t pendingContactCount = 0;
        bool pendingFrame = false;
        uint32_t pendingScanTime = 0;
        bool pendingHasScanTime = false;
        uint32_t completedScanTime = 0;
        bool completedHasScanTime = false;
        bool buttonDown = false;
        std::map<uint32_t, RawContact> pendingContacts;

        PHIDP_PREPARSED_DATA preparsed() const
        {
            return reinterpret_cast<PHIDP_PREPARSED_DATA>(
                    const_cast<BYTE*>(preparsedData.data()));
        }
    };

    explicit Impl(SdlInputHandler* owner)
        : inputHandler(owner),
          hwnd(nullptr)
    {
    }

    bool initialize(SDL_Window* window)
    {
        if (hwnd != nullptr) {
            return true;
        }

#if SDL_VERSION_ATLEAST(2, 0, 10)
        for (int i = 0; i < SDL_GetNumTouchDevices(); i++) {
            if (SDL_GetTouchDeviceType(SDL_GetTouchDevice(i)) ==
                    SDL_TOUCH_DEVICE_INDIRECT_ABSOLUTE) {
                return false;
            }
        }
#endif

        SDL_SysWMinfo windowInfo;
        SDL_VERSION(&windowInfo.version);
        if (!SDL_GetWindowWMInfo(window, &windowInfo) ||
                windowInfo.subsystem != SDL_SYSWM_WINDOWS) {
            SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                        "Windows touchpad Raw Input unavailable: SDL window has no HWND");
            return false;
        }

        hwnd = windowInfo.info.win.window;
        RAWINPUTDEVICE device = {};
        device.usUsagePage = HID_USAGE_PAGE_DIGITIZER;
        device.usUsage = HID_USAGE_DIGITIZER_TOUCH_PAD;
        device.dwFlags = RIDEV_DEVNOTIFY;
        device.hwndTarget = hwnd;
        if (!RegisterRawInputDevices(&device, 1, sizeof(device))) {
            SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                        "Windows touchpad Raw Input registration failed: error=%lu",
                        GetLastError());
            hwnd = nullptr;
            return false;
        }

        SDL_SetWindowsMessageHook(WindowsTouchpadInput::messageHook, this);
        return true;
    }

    void shutdown()
    {
        if (hwnd == nullptr) {
            return;
        }

        SDL_SetWindowsMessageHook(nullptr, nullptr);

        RAWINPUTDEVICE device = {};
        device.usUsagePage = HID_USAGE_PAGE_DIGITIZER;
        device.usUsage = HID_USAGE_DIGITIZER_TOUCH_PAD;
        device.dwFlags = RIDEV_REMOVE;
        device.hwndTarget = nullptr;
        if (!RegisterRawInputDevices(&device, 1, sizeof(device))) {
            SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                        "Windows touchpad Raw Input removal failed: error=%lu",
                        GetLastError());
        }

        devices.clear();
        hwnd = nullptr;
    }

    void handleMessage(void* messageHwnd, unsigned int message,
                       Uint64 wParam, Sint64 lParam)
    {
        if (hwnd == nullptr || messageHwnd != hwnd) {
            return;
        }

        if (message == WM_INPUT) {
            handleRawInput(reinterpret_cast<HRAWINPUT>(lParam));
        }
        else if (message == WM_INPUT_DEVICE_CHANGE) {
            HANDLE device = reinterpret_cast<HANDLE>(lParam);
            uintptr_t key = reinterpret_cast<uintptr_t>(device);
            if (wParam == GIDC_REMOVAL) {
                devices.erase(key);
                inputHandler->cancelWindowsTouchpadContacts(
                        reinterpret_cast<uint64_t>(device));
            }
            else if (wParam == GIDC_ARRIVAL) {
                // A handle may be reused after removal. Discard any cached
                // failure so the new device is parsed on its first report.
                devices.erase(key);
            }
        }
    }

private:
    std::unique_ptr<DeviceState> createDeviceState(HANDLE device)
    {
        UINT preparsedSize = 0;
        UINT queried = GetRawInputDeviceInfoW(device, RIDI_PREPARSEDDATA,
                                              nullptr, &preparsedSize);
        if (queried == static_cast<UINT>(-1) || preparsedSize == 0) {
            return nullptr;
        }

        auto state = std::make_unique<DeviceState>();
        state->handle = device;
        state->preparsedData.resize(preparsedSize);
        UINT copiedSize = preparsedSize;
        UINT copied = GetRawInputDeviceInfoW(device, RIDI_PREPARSEDDATA,
                                             state->preparsedData.data(), &copiedSize);
        if (copied == static_cast<UINT>(-1) || copied == 0 ||
                copied != copiedSize || copied > state->preparsedData.size()) {
            return nullptr;
        }
        state->preparsedData.resize(copied);

        HIDP_CAPS caps = {};
        if (HidP_GetCaps(state->preparsed(), &caps) != HIDP_STATUS_SUCCESS ||
                caps.UsagePage != HID_USAGE_PAGE_DIGITIZER ||
                caps.Usage != HID_USAGE_DIGITIZER_TOUCH_PAD) {
            return nullptr;
        }

        if (caps.NumberInputButtonCaps > 0) {
            USHORT buttonCapsLength = caps.NumberInputButtonCaps;
            state->buttonCaps.resize(buttonCapsLength);
            if (HidP_GetButtonCaps(HidP_Input, state->buttonCaps.data(),
                                   &buttonCapsLength, state->preparsed()) != HIDP_STATUS_SUCCESS) {
                return nullptr;
            }
            state->buttonCaps.resize(buttonCapsLength);
        }

        USHORT valueCapsLength = caps.NumberInputValueCaps;
        if (valueCapsLength == 0) {
            return nullptr;
        }
        state->valueCaps.resize(valueCapsLength);
        if (HidP_GetValueCaps(HidP_Input, state->valueCaps.data(),
                              &valueCapsLength, state->preparsed()) != HIDP_STATUS_SUCCESS) {
            return nullptr;
        }
        state->valueCaps.resize(valueCapsLength);

        return state;
    }

    DeviceState* getDeviceState(HANDLE device)
    {
        uintptr_t key = reinterpret_cast<uintptr_t>(device);
        auto existing = devices.find(key);
        if (existing != devices.end()) {
            return existing->second.get();
        }

        std::unique_ptr<DeviceState> state = createDeviceState(device);
        DeviceState* result = state.get();
        // Cache failures too. Otherwise an unsupported device would repeat the
        // full HID capability query for every raw input report.
        devices.emplace(key, std::move(state));
        return result;
    }

    bool parseReport(DeviceState& device, const BYTE* report,
                     ULONG reportLength, ParsedReport& parsed)
    {
        std::map<USHORT, PartialContact> partialContacts;
        bool anyValue = false;

        for (const HIDP_BUTTON_CAPS& cap : device.buttonCaps) {
            if (cap.ReportID != 0 &&
                    (reportLength == 0 || report[0] != cap.ReportID)) {
                continue;
            }

            const bool isTouchpadButton = cap.UsagePage == HID_USAGE_PAGE_BUTTON &&
                    capContainsUsage(cap, HID_USAGE_BUTTON_1);
            const bool isTipSwitch = cap.UsagePage == HID_USAGE_PAGE_DIGITIZER &&
                    cap.LinkCollection != 0 &&
                    capContainsUsage(cap, HID_USAGE_DIGITIZER_TIP_SWITCH);
            const bool isConfidence = cap.UsagePage == HID_USAGE_PAGE_DIGITIZER &&
                    cap.LinkCollection != 0 &&
                    capContainsUsage(cap, HID_USAGE_DIGITIZER_CONFIDENCE);
            if (!isTouchpadButton && !isTipSwitch && !isConfidence) {
                continue;
            }

            USAGE usages[16];
            ULONG usageCount = static_cast<ULONG>(std::size(usages));
            NTSTATUS status = HidP_GetUsages(
                    HidP_Input, cap.UsagePage, cap.LinkCollection,
                    usages, &usageCount, device.preparsed(),
                    reinterpret_cast<PCHAR>(const_cast<BYTE*>(report)), reportLength);
            if (status != HIDP_STATUS_SUCCESS) {
                continue;
            }

            auto isPressed = [&](USAGE usage) {
                return std::find(usages, usages + usageCount, usage) !=
                        usages + usageCount;
            };
            anyValue = true;
            if (isTouchpadButton) {
                parsed.hasButtonState = true;
                parsed.buttonDown = isPressed(HID_USAGE_BUTTON_1);
            }
            if (isTipSwitch) {
                PartialContact& partial = partialContacts[cap.LinkCollection];
                partial.hasTipSwitch = true;
                partial.tipSwitch = isPressed(HID_USAGE_DIGITIZER_TIP_SWITCH);
            }
            if (isConfidence) {
                PartialContact& partial = partialContacts[cap.LinkCollection];
                partial.hasConfidence = true;
                partial.confidence = isPressed(HID_USAGE_DIGITIZER_CONFIDENCE);
            }
        }

        for (const HIDP_VALUE_CAPS& cap : device.valueCaps) {
            if (cap.ReportID != 0 &&
                    (reportLength == 0 || report[0] != cap.ReportID)) {
                continue;
            }

            auto parseUsage = [&](USAGE usage) {
                if (!capContainsUsage(cap, usage)) {
                    return;
                }

                ULONG value = 0;
                if (HidP_GetUsageValue(HidP_Input, cap.UsagePage,
                                       cap.LinkCollection, usage, &value,
                                       device.preparsed(),
                                       reinterpret_cast<PCHAR>(const_cast<BYTE*>(report)),
                                       reportLength) != HIDP_STATUS_SUCCESS) {
                    return;
                }

                anyValue = true;
                if (cap.UsagePage == HID_USAGE_PAGE_DIGITIZER &&
                        usage == HID_USAGE_DIGITIZER_CONTACT_COUNT) {
                    parsed.hasContactCount = true;
                    parsed.contactCount = value;
                }
                else if (cap.UsagePage == HID_USAGE_PAGE_DIGITIZER &&
                         usage == HID_USAGE_DIGITIZER_SCAN_TIME) {
                    parsed.hasScanTime = true;
                    parsed.scanTime = value;
                }
                else if (cap.LinkCollection != 0) {
                    PartialContact& partial = partialContacts[cap.LinkCollection];
                    if (cap.UsagePage == HID_USAGE_PAGE_DIGITIZER &&
                            usage == HID_USAGE_DIGITIZER_CONTACT_IDENTIFIER) {
                        partial.hasPointerId = true;
                        partial.pointerId = value;
                    }
                    else if (cap.UsagePage == HID_USAGE_PAGE_DIGITIZER &&
                             usage == HID_USAGE_DIGITIZER_TIP_PRESSURE) {
                        partial.hasPressure = true;
                        partial.pressure = value;
                        partial.pressureMin = cap.LogicalMin;
                        partial.pressureMax = cap.LogicalMax;
                    }
                    else if (cap.UsagePage == HID_USAGE_PAGE_GENERIC &&
                             usage == HID_USAGE_GENERIC_X) {
                        partial.hasX = true;
                        partial.x = value;
                        partial.xMin = cap.LogicalMin;
                        partial.xMax = cap.LogicalMax;
                    }
                    else if (cap.UsagePage == HID_USAGE_PAGE_GENERIC &&
                             usage == HID_USAGE_GENERIC_Y) {
                        partial.hasY = true;
                        partial.y = value;
                        partial.yMin = cap.LogicalMin;
                        partial.yMax = cap.LogicalMax;
                    }
                }
            };

            if (cap.UsagePage == HID_USAGE_PAGE_DIGITIZER) {
                if (cap.LinkCollection == 0) {
                    parseUsage(HID_USAGE_DIGITIZER_CONTACT_COUNT);
                    parseUsage(HID_USAGE_DIGITIZER_SCAN_TIME);
                }
                else {
                    parseUsage(HID_USAGE_DIGITIZER_CONTACT_IDENTIFIER);
                    parseUsage(HID_USAGE_DIGITIZER_TIP_PRESSURE);
                }
            }
            else if (cap.UsagePage == HID_USAGE_PAGE_GENERIC &&
                     cap.LinkCollection != 0) {
                parseUsage(HID_USAGE_GENERIC_X);
                parseUsage(HID_USAGE_GENERIC_Y);
            }
        }

        for (const auto& entry : partialContacts) {
            const PartialContact& partial = entry.second;
            if (!partial.hasPointerId || !partial.hasX || !partial.hasY) {
                continue;
            }

            // Contacts rejected by Tip Switch or Confidence still participate
            // in Contact Count and frame completion so an Up event is emitted.
            parsed.contacts.push_back({
                partial.pointerId,
                normalizeAxis(partial.x, partial.xMin, partial.xMax),
                normalizeAxis(partial.y, partial.yMin, partial.yMax),
                partial.hasPressure ?
                    normalizeAxis(partial.pressure, partial.pressureMin, partial.pressureMax) :
                    0.0f,
                (!partial.hasTipSwitch || partial.tipSwitch) &&
                    (!partial.hasConfidence || partial.confidence),
            });
        }

        return anyValue;
    }

    void consumeReport(DeviceState& device, const BYTE* report, ULONG reportLength)
    {
        ParsedReport parsed;
        if (!parseReport(device, report, reportLength, parsed)) {
            return;
        }

        if (parsed.hasButtonState) {
            device.buttonDown = parsed.buttonDown;
        }

        auto forwardButtonState = [&]() {
            inputHandler->handleWindowsTouchpadFrame(
                    reinterpret_cast<uint64_t>(device.handle),
                    nullptr, nullptr, nullptr, nullptr, nullptr, 0,
                    false, device.buttonDown);
        };

        if (!parsed.hasContactCount && !device.pendingFrame) {
            if (parsed.hasButtonState) {
                forwardButtonState();
            }
            return;
        }

        const bool samePendingScan = device.pendingFrame &&
                (!parsed.hasScanTime || !device.pendingHasScanTime ||
                 parsed.scanTime == device.pendingScanTime);
        const bool sameCompletedScan = !device.pendingFrame &&
                parsed.hasScanTime && device.completedHasScanTime &&
                parsed.scanTime == device.completedScanTime;
        const bool zeroCountContinuation = parsed.hasContactCount &&
                parsed.contactCount == 0 && samePendingScan;

        // Ignore continuation reports that arrive after the local five-contact
        // limit completed this scan. A real all-up frame has a new scan time.
        if (parsed.hasContactCount && parsed.contactCount == 0 && sameCompletedScan) {
            if (parsed.hasButtonState) {
                forwardButtonState();
            }
            return;
        }

        bool startsNewFrame = !device.pendingFrame && parsed.hasContactCount;
        if (parsed.hasContactCount && parsed.hasScanTime && device.pendingHasScanTime &&
                parsed.scanTime != device.pendingScanTime) {
            startsNewFrame = true;
        }
        if (startsNewFrame) {
            device.pendingContacts.clear();
            device.pendingContactCount = parsed.contactCount;
            device.pendingFrame = true;
            device.pendingHasScanTime = parsed.hasScanTime;
            device.pendingScanTime = parsed.scanTime;
        }
        else if (parsed.hasContactCount && !zeroCountContinuation) {
            device.pendingContactCount = parsed.contactCount;
        }

        const size_t expectedContacts = std::min<size_t>(
                device.pendingContactCount, MAX_TOUCHPAD_FRAME_CONTACTS);
        for (const RawContact& contact : parsed.contacts) {
            // Parallel reports leave the logical collections after Contact
            // Count invalid. Hybrid reports use all collections until the
            // total declared by the first report has been accumulated.
            if (device.pendingContacts.size() >= expectedContacts) {
                break;
            }

            // Contact IDs are unique within a hardware frame. Keeping the
            // first occurrence prevents an unused parallel-report slot (often
            // zero-filled with Contact ID 0 and Tip clear) from overwriting the
            // real first contact, which also commonly uses ID 0.
            device.pendingContacts.emplace(contact.pointerId, contact);
        }

        if (expectedContacts == 0 ||
                device.pendingContacts.size() >= expectedContacts) {
            std::vector<uint32_t> pointerIds;
            std::vector<float> x;
            std::vector<float> y;
            std::vector<float> pressure;
            std::vector<uint8_t> touching;
            size_t count = expectedContacts;
            pointerIds.reserve(count);
            x.reserve(count);
            y.reserve(count);
            pressure.reserve(count);
            touching.reserve(count);

            for (const auto& entry : device.pendingContacts) {
                if (pointerIds.size() >= count) {
                    break;
                }
                pointerIds.push_back(entry.second.pointerId);
                x.push_back(entry.second.x);
                y.push_back(entry.second.y);
                pressure.push_back(entry.second.pressure);
                touching.push_back(entry.second.touching ? 1 : 0);
            }

            inputHandler->handleWindowsTouchpadFrame(
                    reinterpret_cast<uint64_t>(device.handle),
                    pointerIds.data(), x.data(), y.data(), pressure.data(),
                    touching.data(),
                    static_cast<int>(pointerIds.size()), true,
                    device.buttonDown);
            device.completedHasScanTime = device.pendingHasScanTime;
            device.completedScanTime = device.pendingScanTime;
            device.pendingContacts.clear();
            device.pendingContactCount = 0;
            device.pendingFrame = false;
            device.pendingHasScanTime = false;
        }
    }

    void handleRawInput(HRAWINPUT rawInputHandle)
    {
        RAWINPUTHEADER header = {};
        UINT headerSize = sizeof(header);
        UINT headerBytes = GetRawInputData(rawInputHandle, RID_HEADER, &header,
                                           &headerSize, sizeof(header));
        if (headerBytes != sizeof(header) || header.dwType != RIM_TYPEHID) {
            return;
        }

        DeviceState* device = getDeviceState(header.hDevice);
        if (!device) {
            return;
        }

        UINT rawInputSize = 0;
        if (GetRawInputData(rawInputHandle, RID_INPUT, nullptr, &rawInputSize,
                            sizeof(RAWINPUTHEADER)) != 0 || rawInputSize == 0) {
            return;
        }

        std::vector<BYTE> rawInputData(rawInputSize);
        UINT copiedSize = rawInputSize;
        UINT copied = GetRawInputData(rawInputHandle, RID_INPUT, rawInputData.data(),
                                      &copiedSize, sizeof(RAWINPUTHEADER));
        if (copied == static_cast<UINT>(-1) || copied != copiedSize) {
            return;
        }

        constexpr size_t rawHidDataOffset =
                offsetof(RAWINPUT, data) + offsetof(RAWHID, bRawData);
        if (copiedSize < rawHidDataOffset) {
            return;
        }

        RAWINPUT* rawInput = reinterpret_cast<RAWINPUT*>(rawInputData.data());
        if (rawInput->header.dwType != RIM_TYPEHID ||
                rawInput->data.hid.dwSizeHid == 0 ||
                rawInput->data.hid.dwCount == 0) {
            return;
        }

        const size_t availableReportBytes = copiedSize - rawHidDataOffset;
        if (rawInput->data.hid.dwCount >
                availableReportBytes / rawInput->data.hid.dwSizeHid) {
            return;
        }

        const BYTE* report = rawInput->data.hid.bRawData;
        for (DWORD i = 0; i < rawInput->data.hid.dwCount; i++) {
            consumeReport(*device, report, rawInput->data.hid.dwSizeHid);
            report += rawInput->data.hid.dwSizeHid;
        }
    }

    SdlInputHandler* inputHandler;
    HWND hwnd;
    std::unordered_map<uintptr_t, std::unique_ptr<DeviceState>> devices;
};

WindowsTouchpadInput::WindowsTouchpadInput(SdlInputHandler* inputHandler)
    : m_Impl(std::make_unique<Impl>(inputHandler))
{
}

WindowsTouchpadInput::~WindowsTouchpadInput()
{
    m_Impl->shutdown();
}

bool WindowsTouchpadInput::initialize(SDL_Window* window)
{
    return m_Impl->initialize(window);
}

void SDLCALL WindowsTouchpadInput::messageHook(void* userdata, void* hwnd,
                                               unsigned int message,
                                               Uint64 wParam, Sint64 lParam)
{
    static_cast<Impl*>(userdata)->handleMessage(hwnd, message, wParam, lParam);
}

#endif
