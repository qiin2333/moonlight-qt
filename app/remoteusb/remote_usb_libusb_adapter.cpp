#include "remote_usb_libusb_adapter.h"

#include <QDebug>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(MOONLIGHT_REMOTE_USB_LIBUSB) || \
    defined(MOONLIGHT_REMOTE_USB_LIBUSB_ENABLED)
#define REMOTE_USB_HAS_LIBUSB 1
/* pkg-config commonly supplies the versioned include directory itself
 * (`.../include/libusb-1.0`), while a manually selected SDK root usually
 * supplies its parent (`.../include`).  Accept both layouts. */
#if defined(__has_include)
#  if __has_include(<libusb-1.0/libusb.h>)
#    include <libusb-1.0/libusb.h>
#  elif __has_include(<libusb.h>)
#    include <libusb.h>
#  else
#    error "libusb headers were not found"
#  endif
#else
#  include <libusb-1.0/libusb.h>
#endif
#else
#define REMOTE_USB_HAS_LIBUSB 0
#endif

namespace RemoteUsb {

namespace {

[[maybe_unused]] constexpr qint32 kStatusInvalidArgument = -22;
constexpr qint32 kStatusNoDevice = -19;
[[maybe_unused]] constexpr qint32 kStatusNoEntry = -2;
[[maybe_unused]] constexpr qint32 kStatusCancelled = -104;
[[maybe_unused]] constexpr qint32 kStatusNotSupported = -95;
[[maybe_unused]] constexpr qint32 kStatusNoMemory = -12;
/* USB/IP carries Linux errno values even when the adapter is built on
 * macOS/Windows.  Do not use the host C library's E* macros here (for
 * example, macOS ETIMEDOUT is 60 while USB/IP expects 110). */
[[maybe_unused]] constexpr qint32 kStatusIo = -5;
[[maybe_unused]] constexpr qint32 kStatusAccess = -13;
[[maybe_unused]] constexpr qint32 kStatusBusy = -16;
[[maybe_unused]] constexpr qint32 kStatusTimeout = -110;
[[maybe_unused]] constexpr qint32 kStatusOverflow = -75;
[[maybe_unused]] constexpr qint32 kStatusInterrupted = -4;
[[maybe_unused]] constexpr qint32 kStatusPipe = -32;

bool fail(QString *error, const QString &message)
{
    if (error != nullptr) {
        *error = message;
    }
    return false;
}

QString unavailableMessage()
{
    return QStringLiteral("Remote USB libusb backend is unavailable");
}

#if REMOTE_USB_HAS_LIBUSB

qint32 mapLibusbError(int error)
{
    switch (error) {
    case 0:
        return 0;
    case LIBUSB_ERROR_INVALID_PARAM:
        return kStatusInvalidArgument;
    case LIBUSB_ERROR_ACCESS:
        return kStatusAccess;
    case LIBUSB_ERROR_NO_DEVICE:
        return kStatusNoDevice;
    case LIBUSB_ERROR_NOT_FOUND:
        return kStatusNoEntry;
    case LIBUSB_ERROR_BUSY:
        return kStatusBusy;
    case LIBUSB_ERROR_TIMEOUT:
        return kStatusTimeout;
    case LIBUSB_ERROR_OVERFLOW:
        return kStatusOverflow;
    case LIBUSB_ERROR_INTERRUPTED:
        return kStatusInterrupted;
    case LIBUSB_ERROR_NO_MEM:
        return kStatusNoMemory;
    case LIBUSB_ERROR_NOT_SUPPORTED:
        return kStatusNotSupported;
    case LIBUSB_ERROR_IO:
        return kStatusIo;
    default:
        return kStatusIo;
    }
}

qint32 mapTransferStatus(enum libusb_transfer_status status)
{
    switch (status) {
    case LIBUSB_TRANSFER_COMPLETED:
        return 0;
    case LIBUSB_TRANSFER_TIMED_OUT:
        return kStatusTimeout;
    case LIBUSB_TRANSFER_CANCELLED:
        return kStatusCancelled;
    case LIBUSB_TRANSFER_STALL:
        return kStatusPipe;
    case LIBUSB_TRANSFER_NO_DEVICE:
        return kStatusNoDevice;
    case LIBUSB_TRANSFER_OVERFLOW:
        return kStatusOverflow;
    case LIBUSB_TRANSFER_ERROR:
    default:
        return kStatusIo;
    }
}

QString libusbErrorMessage(const char *operation, int error)
{
    const char *name = libusb_error_name(error);
    const char *detail = libusb_strerror(error);
    QString message = QStringLiteral("Remote USB %1 failed (%2)")
                          .arg(QString::fromLatin1(operation));
    if (name != nullptr && *name != '\0') {
        message += QStringLiteral(" ") + QString::fromLatin1(name);
    }
    if (detail != nullptr && *detail != '\0') {
        message += QStringLiteral(": ") + QString::fromLatin1(detail);
    }
    return message;
}

struct DeviceListGuard {
    libusb_device **list = nullptr;

    DeviceListGuard() = default;

    ~DeviceListGuard()
    {
        if (list != nullptr) {
            libusb_free_device_list(list, 1);
        }
    }

    DeviceListGuard(const DeviceListGuard &) = delete;
    DeviceListGuard &operator=(const DeviceListGuard &) = delete;
};

bool appendBytes(QByteArray *destination, const void *bytes, qsizetype size)
{
    if (destination == nullptr || size < 0 ||
        destination->size() > static_cast<qsizetype>(kRawDescriptorMaxBytes) - size ||
        (size != 0 && bytes == nullptr)) {
        return false;
    }
    if (size != 0) {
        destination->append(reinterpret_cast<const char *>(bytes), size);
    }
    return true;
}

bool appendDescriptor(QByteArray *destination,
                      const unsigned char *bytes,
                      int size)
{
    if (size < 0 || (size != 0 && bytes == nullptr)) {
        return false;
    }
    if (size == 0) {
        return true;
    }
    /* Unknown descriptor arrays are supplied by libusb as a byte stream. Do
     * not trust a malformed bLength to make us read beyond that stream. */
    int offset = 0;
    while (offset < size) {
        const int remaining = size - offset;
        const unsigned int length = bytes[offset];
        if (length < 2u || length > static_cast<unsigned int>(remaining)) {
            return false;
        }
        if (!appendBytes(destination, bytes + offset,
                         static_cast<qsizetype>(length))) {
            return false;
        }
        offset += static_cast<int>(length);
    }
    return true;
}

QByteArray makeBusId(libusb_device *device)
{
    if (device == nullptr) {
        return {};
    }
    const unsigned int bus = libusb_get_bus_number(device);
    const unsigned int address = libusb_get_device_address(device);
    /* USB/IP clients (including usbip-win2 and the Android reference server)
     * expect the canonical busid form "<bus>-<device address>".  Do not add
     * an adapter prefix here: the value is sent verbatim in IMPORT requests
     * and is parsed by the peer as two decimal numbers. */
    return QByteArray::number(bus) + QByteArrayLiteral("-") +
           QByteArray::number(address);
}

QByteArray makeDeviceId(libusb_device *device)
{
    const QByteArray busId = makeBusId(device);
    if (busId.isEmpty()) {
        return {};
    }
    return QByteArrayLiteral("libusb:") + busId;
}

QString makeDisplayName(const libusb_device_descriptor &descriptor,
                        libusb_device *device)
{
    const QByteArray busId = makeBusId(device);
    return QStringLiteral("USB %1:%2 (%3)")
        .arg(static_cast<unsigned int>(descriptor.idVendor), 4, 16,
             QLatin1Char('0'))
        .arg(static_cast<unsigned int>(descriptor.idProduct), 4, 16,
             QLatin1Char('0'))
        .arg(QString::fromLatin1(busId));
}

bool appendDeviceDescriptor(QByteArray *raw,
                            const libusb_device_descriptor &descriptor)
{
    unsigned char bytes[18] = {
        18u,
        LIBUSB_DT_DEVICE,
        static_cast<unsigned char>(descriptor.bcdUSB & 0xffu),
        static_cast<unsigned char>((descriptor.bcdUSB >> 8u) & 0xffu),
        descriptor.bDeviceClass,
        descriptor.bDeviceSubClass,
        descriptor.bDeviceProtocol,
        descriptor.bMaxPacketSize0,
        static_cast<unsigned char>(descriptor.idVendor & 0xffu),
        static_cast<unsigned char>((descriptor.idVendor >> 8u) & 0xffu),
        static_cast<unsigned char>(descriptor.idProduct & 0xffu),
        static_cast<unsigned char>((descriptor.idProduct >> 8u) & 0xffu),
        static_cast<unsigned char>(descriptor.bcdDevice & 0xffu),
        static_cast<unsigned char>((descriptor.bcdDevice >> 8u) & 0xffu),
        descriptor.iManufacturer,
        descriptor.iProduct,
        descriptor.iSerialNumber,
        descriptor.bNumConfigurations,
    };
    return appendBytes(raw, bytes, sizeof(bytes));
}

bool appendConfigDescriptor(QByteArray *raw,
                            const libusb_config_descriptor &config)
{
    unsigned char bytes[9] = {
        9u,
        LIBUSB_DT_CONFIG,
        static_cast<unsigned char>(config.wTotalLength & 0xffu),
        static_cast<unsigned char>((config.wTotalLength >> 8u) & 0xffu),
        config.bNumInterfaces,
        config.bConfigurationValue,
        config.iConfiguration,
        config.bmAttributes,
        config.MaxPower,
    };
    return appendBytes(raw, bytes, sizeof(bytes));
}

bool appendInterfaceDescriptor(QByteArray *raw,
                               const libusb_interface_descriptor &interface)
{
    const unsigned char bytes[9] = {
        9u,
        LIBUSB_DT_INTERFACE,
        interface.bInterfaceNumber,
        interface.bAlternateSetting,
        interface.bNumEndpoints,
        interface.bInterfaceClass,
        interface.bInterfaceSubClass,
        interface.bInterfaceProtocol,
        interface.iInterface,
    };
    return appendBytes(raw, bytes, sizeof(bytes));
}

bool appendEndpointDescriptor(QByteArray *raw,
                              const libusb_endpoint_descriptor &endpoint)
{
    const unsigned char bytes[7] = {
        7u,
        LIBUSB_DT_ENDPOINT,
        endpoint.bEndpointAddress,
        endpoint.bmAttributes,
        static_cast<unsigned char>(endpoint.wMaxPacketSize & 0xffu),
        static_cast<unsigned char>((endpoint.wMaxPacketSize >> 8u) & 0xffu),
        endpoint.bInterval,
    };
    return appendBytes(raw, bytes, sizeof(bytes));
}

bool appendConfiguration(QByteArray *raw,
                         const libusb_config_descriptor &config,
                         QVector<Endpoint> *endpoints,
                         bool *hasIsochronous)
{
    if (!appendConfigDescriptor(raw, config) ||
        !appendDescriptor(raw, config.extra, config.extra_length)) {
        return false;
    }
    if (config.bNumInterfaces == 0u) {
        return true;
    }
    if (config.interface == nullptr) {
        return false;
    }
    for (int interfaceIndex = 0; interfaceIndex < config.bNumInterfaces;
         ++interfaceIndex) {
        const libusb_interface &interface = config.interface[interfaceIndex];
        if (interface.num_altsetting < 0 ||
            (interface.num_altsetting > 0 && interface.altsetting == nullptr)) {
            return false;
        }
        for (int altIndex = 0; altIndex < interface.num_altsetting; ++altIndex) {
            const libusb_interface_descriptor &alt = interface.altsetting[altIndex];
            if (!appendInterfaceDescriptor(raw, alt) ||
                !appendDescriptor(raw, alt.extra, alt.extra_length)) {
                return false;
            }
            if (alt.endpoint == nullptr && alt.bNumEndpoints != 0u) {
                return false;
            }
            for (int endpointIndex = 0; endpointIndex < alt.bNumEndpoints;
                 ++endpointIndex) {
                const libusb_endpoint_descriptor &endpoint =
                    alt.endpoint[endpointIndex];
                if (!appendEndpointDescriptor(raw, endpoint) ||
                    !appendDescriptor(raw, endpoint.extra,
                                      endpoint.extra_length)) {
                    return false;
                }
                const unsigned char transferType =
                    endpoint.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK;
                if (transferType == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS) {
                    if (hasIsochronous != nullptr) {
                        *hasIsochronous = true;
                    }
                }
                if (endpoints != nullptr &&
                    transferType != LIBUSB_TRANSFER_TYPE_CONTROL) {
                    if (endpoints->size() >=
                        static_cast<qsizetype>(kEndpointMaxCount)) {
                        return false;
                    }
                    Endpoint value;
                    value.interfaceNumber = alt.bInterfaceNumber;
                    value.alternateSetting = alt.bAlternateSetting;
                    value.address = endpoint.bEndpointAddress;
                    value.attributes = endpoint.bmAttributes;
                    value.maxPacketSize = endpoint.wMaxPacketSize;
                    value.interval = endpoint.bInterval;
                    value.reserved = 0;
                    endpoints->append(value);
                }
            }
        }
    }
    return true;
}

bool snapshotForDevice(libusb_device *device,
                       DeviceSnapshot *snapshot,
                       QString *error)
{
    if (device == nullptr || snapshot == nullptr) {
        return fail(error, QStringLiteral("Remote USB device is invalid"));
    }
    libusb_device_descriptor descriptor {};
    int result = libusb_get_device_descriptor(device, &descriptor);
    if (result != 0) {
        return fail(error, libusbErrorMessage("read device descriptor", result));
    }

    DeviceSnapshot value;
    value.deviceId = makeDeviceId(device);
    value.busId = makeBusId(device);
    value.vendorId = descriptor.idVendor;
    value.productId = descriptor.idProduct;
    value.deviceBcd = descriptor.bcdDevice;
    value.deviceClass = descriptor.bDeviceClass;
    value.deviceSubclass = descriptor.bDeviceSubClass;
    value.deviceProtocol = descriptor.bDeviceProtocol;
    value.displayName = makeDisplayName(descriptor, device);
    if (value.deviceId.isEmpty() || value.busId.isEmpty() ||
        value.busId.size() > static_cast<qsizetype>(kBusIdMaxBytes) ||
        value.busId.contains('\0') || !appendDeviceDescriptor(&value.rawDescriptors,
                                                               descriptor)) {
        return fail(error, QStringLiteral("Remote USB device identity is invalid"));
    }

    for (unsigned int index = 0; index < descriptor.bNumConfigurations; ++index) {
        libusb_config_descriptor *config = nullptr;
        result = libusb_get_config_descriptor(device, index, &config);
        if (result != 0 || config == nullptr) {
            /* A device can disappear between the list and descriptor query.
             * Keep the device descriptor and continue with other configs. */
            continue;
        }
        const bool appended = appendConfiguration(&value.rawDescriptors, *config,
                                                  &value.endpoints,
                                                  &value.hasIsochronousEndpoints);
        libusb_free_config_descriptor(config);
        if (!appended) {
            return fail(error, QStringLiteral("Remote USB configuration descriptor is invalid"));
        }
        if (value.rawDescriptors.size() >=
            static_cast<qsizetype>(kRawDescriptorMaxBytes)) {
            return fail(error, QStringLiteral("Remote USB descriptors are too large"));
        }
    }
    *snapshot = std::move(value);
    return true;
}

unsigned char transferFlags(quint32 flags)
{
    unsigned char mapped = 0;
    if ((flags & LIBUSB_TRANSFER_SHORT_NOT_OK) != 0u) {
        mapped |= LIBUSB_TRANSFER_SHORT_NOT_OK;
    }
#ifdef LIBUSB_TRANSFER_ADD_ZERO_PACKET
    if ((flags & LIBUSB_TRANSFER_ADD_ZERO_PACKET) != 0u) {
        mapped |= LIBUSB_TRANSFER_ADD_ZERO_PACKET;
    }
#endif
    return mapped;
}

#endif // REMOTE_USB_HAS_LIBUSB

} // namespace

#if REMOTE_USB_HAS_LIBUSB

struct RemoteUsbLibusbAdapter::Impl {
    /* libusb normally invokes callbacks from the event worker, but a
     * platform shim (and a few test backends) may invoke one synchronously
     * from submit/cancel.  Keep a stack marker instead of a single thread
     * boolean so callbacks which re-enter another adapter remain safe. */
    struct CallbackScope {
        Impl *owner = nullptr;
        CallbackScope *previous = nullptr;

        explicit CallbackScope(Impl *value) noexcept
            : owner(value), previous(top())
        {
            top() = this;
        }

        ~CallbackScope() noexcept
        {
            top() = previous;
        }

        static CallbackScope *&top() noexcept
        {
            static thread_local CallbackScope *current = nullptr;
            return current;
        }

        static bool contains(const Impl *value) noexcept
        {
            for (CallbackScope *scope = top(); scope != nullptr;
                 scope = scope->previous) {
                if (scope->owner == value) {
                    return true;
                }
            }
            return false;
        }
    };

    struct TransferState {
        Impl *owner = nullptr;
        quint64 token = 0;
        TransferRequest request;
        TransferCompletionCallback completionCallback;
        CancelCompletionCallback cancelCompletionCallback;
        libusb_transfer *transfer = nullptr;
        std::vector<unsigned char> storage;
        bool interfaceTracked = false;
        int interfaceNumber = -1;
        bool cancelRequested = false;
        bool terminal = false;
        bool cancelCallbackSent = false;
        qint32 cancelStatus = 0;
        /* libusb owns the transfer object until its callback returns.  Public
         * cancel/release calls can race that callback, so serialize every
         * operation which dereferences or frees the object.  A recursive
         * mutex also permits a synchronous test/backend callback to finish
         * on the submitting thread. */
        std::recursive_mutex operationMutex;
    };

    explicit Impl(const RemoteUsbLibusbAdapterConfig &requested)
        : config(requested)
    {
        if (config.transferTimeoutMs == 0u) {
            config.transferTimeoutMs = 30u * 1000u;
        }
        if (config.eventPollTimeoutMs == 0u) {
            config.eventPollTimeoutMs = 50u;
        }
        config.maxInflight = std::clamp<quint32>(config.maxInflight, 1u, 4096u);

        const int result = libusb_init(&context);
        if (result != 0 || context == nullptr) {
            context = nullptr;
            return;
        }
        initialized = true;
        try {
            eventThread = std::thread([this]() { eventLoop(); });
            eventThreadStarted = true;
        } catch (...) {
            initialized = false;
            libusb_exit(context);
            context = nullptr;
        }
    }

    ~Impl() noexcept
    {
        shutdown();
    }

    Impl(const Impl &) = delete;
    Impl &operator=(const Impl &) = delete;

    RemoteUsbLibusbAdapterConfig config;
    libusb_context *context = nullptr;
    libusb_device_handle *handle = nullptr;
    libusb_device *device = nullptr;
    DeviceSnapshot claimedSnapshot;
    std::vector<int> claimedInterfaces;
    std::map<int, int> activeAlternates;
    std::map<int, std::size_t> interfaceInflight;

    mutable std::mutex mutex;
    std::recursive_mutex ioMutex;
    std::condition_variable condition;
    std::unordered_map<quint64, std::shared_ptr<TransferState>> transfers;
    std::size_t activeCallbacks = 0;
    bool initialized = false;
    bool claimed = false;
    bool claimInProgress = false;
    bool releasePending = false;
    bool shutdownInProgress = false;
    bool stopRequested = false;
    bool eventThreadStarted = false;
    bool eventThreadRunning = false;
    std::thread::id eventThreadId;
    std::thread eventThread;

    static void LIBUSB_CALL transferCallback(libusb_transfer *transfer) noexcept
    {
        if (transfer == nullptr || transfer->user_data == nullptr) {
            return;
        }
        auto *state = static_cast<TransferState *>(transfer->user_data);
        if (state->owner != nullptr) {
            state->owner->handleTransfer(transfer);
        }
    }

    void eventLoop() noexcept
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            eventThreadId = std::this_thread::get_id();
            eventThreadRunning = true;
            condition.notify_all();
        }
        while (true) {
            bool shouldStop = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                /* A stop request is a drain request as well.  In particular,
                 * cancellation callbacks may still be queued when shutdown is
                 * initiated from a completion callback. */
                shouldStop = stopRequested && transfers.empty() &&
                    activeCallbacks == 0u && !releasePending;
            }
            if (shouldStop) {
                break;
            }
            timeval timeout {};
            timeout.tv_sec = static_cast<long>(config.eventPollTimeoutMs / 1000u);
            timeout.tv_usec = static_cast<long>((config.eventPollTimeoutMs % 1000u) * 1000u);
            const int result = libusb_handle_events_timeout_completed(
                context, &timeout, nullptr);
            if (result != 0 && result != LIBUSB_ERROR_INTERRUPTED &&
                result != LIBUSB_ERROR_BUSY) {
                /* A transient event error is reported by individual transfer
                 * callbacks when possible. Avoid calling user code here. */
            }
            finalizeDeferredRelease();
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            eventThreadRunning = false;
            condition.notify_all();
        }
    }

    void requestStop() noexcept
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopRequested = true;
        }
        if (context != nullptr) {
            libusb_interrupt_event_handler(context);
        }
    }

    bool isEventThread() const noexcept
    {
        std::lock_guard<std::mutex> lock(mutex);
        return eventThreadId == std::this_thread::get_id();
    }

    void handleTransfer(libusb_transfer *raw) noexcept
    {
        CallbackScope callbackScope(this);
        std::shared_ptr<TransferState> state;
        TransferCompletion completion;
        CancelCompletionCallback cancelCallback;
        qint32 cancelStatus = 0;
        bool notifyCancel = false;
        bool callbackCounted = false;
        bool transferFreed = false;
        try {
            {
                std::lock_guard<std::mutex> lock(mutex);
                auto *rawState = static_cast<TransferState *>(raw->user_data);
                if (rawState == nullptr) {
                    return;
                }
                const auto iterator = transfers.find(rawState->token);
                if (iterator == transfers.end() || iterator->second.get() != rawState ||
                    rawState->transfer != raw || rawState->terminal) {
                    return;
                }
                state = iterator->second;
                state->terminal = true;
                ++activeCallbacks;
                callbackCounted = true;
                completion.status = mapTransferStatus(raw->status);
                completion.startFrame = std::max<qint32>(0, state->request.startFrame);
                completion.errorCount = 0;
                int actualLength = std::max(0, raw->actual_length);
                const bool controlIn =
                    state->request.endpoint == 0u &&
                    (state->request.setup[0] & 0x80u) != 0u;
                const bool dataIn = state->request.direction == TransferDirection::In;
                const bool inTransfer = state->request.endpoint == 0u ? controlIn : dataIn;
                if (actualLength > state->request.transferBufferLength) {
                    completion.status = kStatusOverflow;
                    actualLength = 0;
                }
                completion.actualLength = actualLength;
                if (inTransfer && actualLength > 0 && raw->buffer != nullptr) {
                    const unsigned char *source = raw->buffer;
                    if (state->request.endpoint == 0u) {
                        source += 8u;
                    }
                    completion.data = QByteArray(
                        reinterpret_cast<const char *>(source), actualLength);
                }
                if (state->cancelRequested) {
                    notifyCancel = !state->cancelCallbackSent;
                    state->cancelCallbackSent = true;
                    cancelCallback = state->cancelCompletionCallback;
                    cancelStatus = state->cancelStatus;
                }
            }

            if (state->completionCallback) {
                try {
                    state->completionCallback(state->token, completion);
                } catch (...) {
                    /* Platform callbacks must never unwind through libusb. */
                }
            }
            if (notifyCancel && cancelCallback) {
                try {
                    cancelCallback(state->token, cancelStatus);
                } catch (...) {
                }
            }

            /* Do not let a racing cancel/release call hand a freed transfer
             * pointer to libusb.  The operation lock is deliberately acquired
             * only after the user callbacks have run, so those callbacks can
             * re-enter the adapter without self-deadlocking. */
            {
                std::unique_lock<std::recursive_mutex> operationLock(
                    state->operationMutex);
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (state->interfaceTracked) {
                        auto count = interfaceInflight.find(state->interfaceNumber);
                        if (count != interfaceInflight.end()) {
                            if (count->second > 0u) {
                                --count->second;
                            }
                            if (count->second == 0u) {
                                interfaceInflight.erase(count);
                            }
                        }
                    }
                    state->transfer = nullptr;
                    transfers.erase(state->token);
                    if (activeCallbacks > 0u) {
                        --activeCallbacks;
                    }
                    callbackCounted = false;
                    condition.notify_all();
                }
                libusb_free_transfer(raw);
                transferFreed = true;
            }
            /* The event loop performs deferred release after callbacks.  Do
             * not close a handle recursively from inside libusb_submit(),
             * where some test backends invoke the callback synchronously. */
        } catch (...) {
            /* Ensure a malformed callback cannot leave release() blocked. */
            try {
                if (state != nullptr) {
                    std::unique_lock<std::recursive_mutex> operationLock(
                        state->operationMutex);
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        if (state->interfaceTracked) {
                            auto count = interfaceInflight.find(state->interfaceNumber);
                            if (count != interfaceInflight.end()) {
                                if (count->second > 0u) {
                                    --count->second;
                                }
                                if (count->second == 0u) {
                                    interfaceInflight.erase(count);
                                }
                            }
                        }
                        state->transfer = nullptr;
                        transfers.erase(state->token);
                        if (callbackCounted && activeCallbacks > 0u) {
                            --activeCallbacks;
                        }
                        callbackCounted = false;
                        condition.notify_all();
                    }
                    if (!transferFreed && raw != nullptr) {
                        libusb_free_transfer(raw);
                        transferFreed = true;
                    }
                } else if (!transferFreed && raw != nullptr) {
                    libusb_free_transfer(raw);
                    transferFreed = true;
                }
            } catch (...) {
            }
            if (!transferFreed && raw != nullptr) {
                libusb_free_transfer(raw);
            }
        }
    }

    void finalizeDeferredRelease() noexcept
    {
        std::unique_lock<std::recursive_mutex> ioLock(ioMutex, std::try_to_lock);
        if (!ioLock.owns_lock()) {
            return;
        }
        libusb_device_handle *oldHandle = nullptr;
        libusb_device *oldDevice = nullptr;
        std::vector<int> oldInterfaces;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!releasePending || !transfers.empty() || activeCallbacks != 0u) {
                return;
            }
            oldHandle = handle;
            oldDevice = device;
            oldInterfaces = claimedInterfaces;
            handle = nullptr;
            device = nullptr;
            claimedInterfaces.clear();
            activeAlternates.clear();
            interfaceInflight.clear();
            claimedSnapshot = DeviceSnapshot {};
            claimed = false;
            releasePending = false;
            condition.notify_all();
        }
        if (oldHandle != nullptr) {
            for (const int interfaceNumber : oldInterfaces) {
                (void)libusb_release_interface(oldHandle, interfaceNumber);
            }
            libusb_close(oldHandle);
        }
        if (oldDevice != nullptr) {
            libusb_unref_device(oldDevice);
        }
    }

    void release() noexcept
    {
        std::unique_lock<std::recursive_mutex> ioLock(ioMutex);
        std::vector<std::shared_ptr<TransferState>> pending;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if ((!claimed && !releasePending) || handle == nullptr) {
                return;
            }
            claimed = false;
            releasePending = true;
            pending.reserve(transfers.size());
            for (const auto &entry : transfers) {
                const auto &state = entry.second;
                if (state == nullptr || state->transfer == nullptr || state->terminal) {
                    continue;
                }
                if (!state->cancelRequested) {
                    state->cancelRequested = true;
                    state->cancelStatus = 0;
                }
                pending.push_back(state);
            }
        }
        /* The state transition above prevents new submissions.  Do not hold
         * the adapter-wide lock while asking libusb to cancel each transfer:
         * callbacks are allowed to re-enter release/cancel. */
        ioLock.unlock();
        for (const std::shared_ptr<TransferState> &state : pending) {
            if (state == nullptr) {
                continue;
            }
            int result = LIBUSB_ERROR_NOT_FOUND;
            {
                std::unique_lock<std::recursive_mutex> operationLock(
                    state->operationMutex);
                libusb_transfer *transfer = nullptr;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (state->transfer == nullptr || state->terminal) {
                        continue;
                    }
                    transfer = state->transfer;
                }
                result = libusb_cancel_transfer(transfer);
            }
            if (result != 0) {
                std::lock_guard<std::mutex> lock(mutex);
                if (!state->cancelCallbackSent) {
                    state->cancelStatus = mapLibusbError(result);
                }
            }
        }
        if (context != nullptr) {
            libusb_interrupt_event_handler(context);
        }
        if (isEventThread() || CallbackScope::contains(this)) {
            /* The callback currently on the event thread will finish the
             * drain and call finalizeDeferredRelease().  The same applies to
             * a synchronous/mock callback: its submit/cancel caller performs
             * the deferred close after libusb returns. */
            return;
        }
        /* No new submit can pass the claimed/releasePending checks. Let
         * callbacks re-enter the adapter while we wait for the drain. */
        std::unique_lock<std::mutex> stateLock(mutex);
        condition.wait(stateLock, [this]() {
            return transfers.empty() && activeCallbacks == 0u;
        });
        stateLock.unlock();
        finalizeDeferredRelease();
    }

    void finishShutdown() noexcept
    {
        if (eventThread.joinable() && !isEventThread()) {
            try {
                eventThread.join();
            } catch (...) {
            }
        }
        /* enumerate/claim/submit use this lock while touching the libusb
         * context.  Join first (the event worker may need the same lock for a
         * deferred release), then wait for any caller that was already inside
         * a libusb operation before destroying the context. */
        std::unique_lock<std::recursive_mutex> ioLock(ioMutex);
        if (context != nullptr) {
            libusb_exit(context);
            context = nullptr;
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            initialized = false;
            eventThreadStarted = false;
            eventThreadRunning = false;
            condition.notify_all();
        }
    }

    void shutdown() noexcept
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            shutdownInProgress = true;
        }
        release();
        requestStop();
        /* A callback can destroy the adapter on the libusb event thread.  It
         * cannot join itself; leave the context and Impl alive until a helper
         * thread has joined the event worker and completed cleanup. */
        if (isEventThread()) {
            return;
        }
        finishShutdown();
    }
};

#endif // REMOTE_USB_HAS_LIBUSB

RemoteUsbLibusbAdapter::RemoteUsbLibusbAdapter(
    const RemoteUsbLibusbAdapterConfig &config)
    : m_impl(std::make_unique<Impl>(config))
{
}

RemoteUsbLibusbAdapter::~RemoteUsbLibusbAdapter()
{
#if REMOTE_USB_HAS_LIBUSB
    if (m_impl != nullptr && m_impl->isEventThread()) {
        Impl *impl = m_impl.release();
        impl->shutdown();
        try {
            std::thread([impl]() {
                impl->finishShutdown();
                delete impl;
            }).detach();
        } catch (...) {
            /* Thread creation failure is exceptionally rare.  Keep the
             * worker/context alive rather than deleting Impl while its event
             * loop still references it; the process can reclaim these
             * resources at exit. */
        }
        return;
    }
#endif
    m_impl.reset();
}

bool RemoteUsbLibusbAdapter::compiledWithLibusb() noexcept
{
#if REMOTE_USB_HAS_LIBUSB
    return true;
#else
    return false;
#endif
}

bool RemoteUsbLibusbAdapter::isAvailable() const noexcept
{
#if REMOTE_USB_HAS_LIBUSB
    return m_impl != nullptr && m_impl->initialized && m_impl->context != nullptr;
#else
    return false;
#endif
}

#if REMOTE_USB_HAS_LIBUSB

QVector<DeviceSnapshot> RemoteUsbLibusbAdapter::enumerate(QString *error)
{
    QVector<DeviceSnapshot> snapshots;
    if (!isAvailable()) {
        fail(error, unavailableMessage());
        return snapshots;
    }
    std::unique_lock<std::recursive_mutex> ioLock(m_impl->ioMutex);
    /* The first availability check can race a concurrent shutdown.  Recheck
     * after taking the same lock used by shutdown/claim so the context cannot
     * disappear while libusb is walking its device list. */
    if (!isAvailable()) {
        fail(error, unavailableMessage());
        return snapshots;
    }
    try {
        DeviceListGuard listGuard;
        const ssize_t count = libusb_get_device_list(m_impl->context,
                                                     &listGuard.list);
        if (count < 0) {
            fail(error, libusbErrorMessage("enumerate devices", static_cast<int>(count)));
            return snapshots;
        }
        for (ssize_t index = 0; index < count; ++index) {
            DeviceSnapshot snapshot;
            QString snapshotError;
            if (!snapshotForDevice(listGuard.list[index], &snapshot, &snapshotError)) {
                continue;
            }
            /* v1 has no isochronous PDU support. Keep such devices visible to
             * callers so the UI can explain why they are unavailable; the
             * session binding will reject their capability deterministically. */
            snapshots.append(std::move(snapshot));
        }
        return snapshots;
    } catch (const std::exception &exception) {
        fail(error, QString::fromUtf8(exception.what()));
    } catch (...) {
        fail(error, QStringLiteral("Remote USB device enumeration failed"));
    }
    return snapshots;
}

bool RemoteUsbLibusbAdapter::claim(const DeviceSnapshot &device,
                                   QString *error)
{
    if (!isAvailable()) {
        return fail(error, unavailableMessage());
    }
    if (device.deviceId.isEmpty() || device.busId.isEmpty() ||
        device.busId.size() > static_cast<qsizetype>(kBusIdMaxBytes) ||
        device.busId.contains('\0')) {
        return fail(error, QStringLiteral("Remote USB device identity is invalid"));
    }
    /* Serialize the check/claim/commit sequence with release() and submit().
     * The lock is recursive so a test backend which completes synchronously
     * can safely re-enter the adapter from its callback. */
    std::unique_lock<std::recursive_mutex> ioLock(m_impl->ioMutex);
    if (!isAvailable()) {
        return fail(error, unavailableMessage());
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (m_impl->shutdownInProgress || m_impl->stopRequested ||
            m_impl->claimed ||
            m_impl->claimInProgress || m_impl->releasePending) {
            if (!m_impl->shutdownInProgress && !m_impl->stopRequested &&
                m_impl->claimed &&
                m_impl->claimedSnapshot.deviceId == device.deviceId) {
                return true;
            }
            return fail(error, QStringLiteral("Remote USB adapter already has a claimed device"));
        }
        m_impl->claimInProgress = true;
    }

    libusb_device *found = nullptr;
    libusb_device_handle *newHandle = nullptr;
    DeviceSnapshot current;
    QVector<int> interfaces;
    QString localError;
    bool success = false;
    DeviceListGuard listGuard;
    try {
        const ssize_t count = libusb_get_device_list(m_impl->context,
                                                     &listGuard.list);
        if (count < 0) {
            localError = libusbErrorMessage("enumerate devices", static_cast<int>(count));
        } else {
            for (ssize_t index = 0; index < count; ++index) {
                DeviceSnapshot candidate;
                if (!snapshotForDevice(listGuard.list[index], &candidate, nullptr)) {
                    continue;
                }
                if (candidate.deviceId == device.deviceId ||
                    candidate.busId == device.busId) {
                    found = libusb_ref_device(listGuard.list[index]);
                    current = std::move(candidate);
                    break;
                }
            }
            if (found == nullptr) {
                localError = QStringLiteral("Remote USB device is no longer present");
            } else {
                int result = libusb_open(found, &newHandle);
                if (result != 0 || newHandle == nullptr) {
                    localError = libusbErrorMessage("open device", result);
                } else {
                    if (m_impl->config.autoDetachKernelDriver) {
                        const int detachResult =
                            libusb_set_auto_detach_kernel_driver(newHandle, 1);
                        if (detachResult != 0 &&
                            detachResult != LIBUSB_ERROR_NOT_SUPPORTED) {
                            localError = libusbErrorMessage("enable kernel-driver detach",
                                                            detachResult);
                        }
                    }
                    libusb_config_descriptor *config = nullptr;
                    int configResult = libusb_get_active_config_descriptor(found, &config);
                    if (configResult != 0 || config == nullptr) {
                        configResult = libusb_get_config_descriptor(found, 0, &config);
                    }
                    if (config != nullptr) {
                        std::set<int> uniqueInterfaces;
                        for (int index = 0; index < config->bNumInterfaces; ++index) {
                            const libusb_interface &iface = config->interface[index];
                            if (iface.altsetting != nullptr && iface.num_altsetting > 0) {
                                uniqueInterfaces.insert(
                                    iface.altsetting[0].bInterfaceNumber);
                            }
                        }
                        interfaces.assign(uniqueInterfaces.begin(), uniqueInterfaces.end());
                        libusb_free_config_descriptor(config);
                    }
                    if (localError.isEmpty()) {
                        std::vector<int> claimedInterfaces;
                        for (const int interfaceNumber : interfaces) {
                            result = libusb_claim_interface(newHandle, interfaceNumber);
                            if (result != 0) {
                                localError = libusbErrorMessage("claim interface", result);
                                break;
                            }
                            claimedInterfaces.push_back(interfaceNumber);
                        }
                        if (!localError.isEmpty()) {
                            for (const int interfaceNumber : claimedInterfaces) {
                                (void)libusb_release_interface(newHandle, interfaceNumber);
                            }
                        } else {
                            std::lock_guard<std::mutex> lock(m_impl->mutex);
                            if (m_impl->shutdownInProgress ||
                                m_impl->stopRequested || m_impl->claimed ||
                                m_impl->releasePending) {
                                localError = QStringLiteral("Remote USB adapter already has a claimed device");
                                for (const int interfaceNumber : interfaces) {
                                    (void)libusb_release_interface(newHandle, interfaceNumber);
                                }
                            } else {
                                m_impl->handle = newHandle;
                                m_impl->device = found;
                                m_impl->claimedSnapshot = std::move(current);
                                m_impl->claimedInterfaces.clear();
                                m_impl->claimedInterfaces.reserve(interfaces.size());
                                for (const int interfaceNumber : interfaces) {
                                    m_impl->claimedInterfaces.push_back(interfaceNumber);
                                }
                                m_impl->activeAlternates.clear();
                                m_impl->interfaceInflight.clear();
                                for (const int interfaceNumber : m_impl->claimedInterfaces) {
                                    m_impl->activeAlternates[interfaceNumber] = 0;
                                }
                                m_impl->claimed = true;
                                newHandle = nullptr;
                                found = nullptr;
                                success = true;
                            }
                        }
                    }
                }
            }
        }
    } catch (const std::exception &exception) {
        localError = QString::fromUtf8(exception.what());
    } catch (...) {
        localError = QStringLiteral("Remote USB device claim failed");
    }
    if (newHandle != nullptr) {
        libusb_close(newHandle);
    }
    if (found != nullptr) {
        libusb_unref_device(found);
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->claimInProgress = false;
        m_impl->condition.notify_all();
    }
    if (!success) {
        return fail(error, localError.isEmpty()
                                ? QStringLiteral("Remote USB device claim failed")
                                : localError);
    }
    return true;
}

void RemoteUsbLibusbAdapter::release() noexcept
{
    if (m_impl != nullptr) {
        try {
            m_impl->release();
        } catch (...) {
        }
    }
}

EndpointResolution RemoteUsbLibusbAdapter::resolveEndpoint(
    const TransferRequest &request,
    Endpoint *endpointOut,
    QString *error) const
{
    if (endpointOut == nullptr) {
        fail(error, QStringLiteral("Remote USB endpoint output is null"));
        return EndpointResolution::Rejected;
    }
    *endpointOut = Endpoint {};
    if (!isAvailable()) {
        fail(error, unavailableMessage());
        return EndpointResolution::Rejected;
    }
    if (request.endpoint > 0x0fu ||
        (request.direction != TransferDirection::In &&
         request.direction != TransferDirection::Out)) {
        fail(error, QStringLiteral("Remote USB endpoint request is invalid"));
        return EndpointResolution::Rejected;
    }
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (!m_impl->claimed || m_impl->handle == nullptr) {
        fail(error, QStringLiteral("Remote USB device is not claimed"));
        return EndpointResolution::Rejected;
    }
    if (request.endpoint == 0u) {
        endpointOut->address = 0;
        endpointOut->attributes = LIBUSB_TRANSFER_TYPE_CONTROL;
        endpointOut->maxPacketSize = 0;
        endpointOut->reserved = 0;
        return EndpointResolution::Found;
    }
    const quint8 expectedAddress = static_cast<quint8>(
        request.endpoint | (request.direction == TransferDirection::In ? 0x80u : 0u));
    const Endpoint *fallback = nullptr;
    for (const Endpoint &endpoint : m_impl->claimedSnapshot.endpoints) {
        if (endpoint.address != expectedAddress || endpoint.reserved != 0u ||
            endpoint.maxPacketSize == 0u) {
            continue;
        }
        const quint8 transferType = endpoint.attributes & 0x03u;
        if (transferType != LIBUSB_TRANSFER_TYPE_BULK &&
            transferType != LIBUSB_TRANSFER_TYPE_INTERRUPT) {
            continue;
        }
        if (endpoint.alternateSetting == 0u) {
            *endpointOut = endpoint;
            return EndpointResolution::Found;
        }
        if (fallback == nullptr) {
            fallback = &endpoint;
        }
    }
    if (fallback != nullptr) {
        *endpointOut = *fallback;
        return EndpointResolution::Found;
    }
    return EndpointResolution::NotFound;
}

SubmitDisposition RemoteUsbLibusbAdapter::submitControl(
    const TransferRequest &request,
    TransferCompletionCallback completionCallback,
    QString *error)
{
    return submitCommon(request, std::move(completionCallback), true, error);
}

SubmitDisposition RemoteUsbLibusbAdapter::submitData(
    const TransferRequest &request,
    TransferCompletionCallback completionCallback,
    QString *error)
{
    return submitCommon(request, std::move(completionCallback), false, error);
}

SubmitDisposition RemoteUsbLibusbAdapter::submitCommon(
    const TransferRequest &request,
    TransferCompletionCallback completionCallback,
    bool control,
    QString *error)
{
    if (!isAvailable()) {
        fail(error, unavailableMessage());
        return SubmitDisposition::Rejected;
    }
    if (!completionCallback || request.requestToken == 0u ||
        request.transferBufferLength < 0 ||
        static_cast<std::size_t>(request.transferBufferLength) > kMaxTransferSize ||
        request.startFrame < 0 || request.interval < 0 ||
        request.numberOfPackets != 0 ||
        (request.endpoint > 0x0fu) ||
        (control != (request.endpoint == 0u))) {
        fail(error, QStringLiteral("Remote USB transfer request is invalid"));
        return SubmitDisposition::Rejected;
    }
    const TransferKind expectedKind = control ? TransferKind::Control
                                              : TransferKind::Data;
    if (request.kind != expectedKind) {
        fail(error, QStringLiteral("Remote USB transfer kind is invalid"));
        return SubmitDisposition::Rejected;
    }
    const qsizetype length = static_cast<qsizetype>(request.transferBufferLength);
    if (request.direction == TransferDirection::Out) {
        if (request.data.size() != length) {
            fail(error, QStringLiteral("Remote USB OUT payload length is invalid"));
            return SubmitDisposition::Rejected;
        }
    } else if (request.direction != TransferDirection::In || !request.data.isEmpty()) {
        fail(error, QStringLiteral("Remote USB transfer direction or payload is invalid"));
        return SubmitDisposition::Rejected;
    }
    if (control) {
        const bool setupIn = (request.setup[0] & 0x80u) != 0u;
        const quint16 setupLength = static_cast<quint16>(request.setup[6]) |
                                    static_cast<quint16>(request.setup[7] << 8u);
        if (setupIn != (request.direction == TransferDirection::In) ||
            setupLength != static_cast<quint16>(request.transferBufferLength) ||
            request.transferBufferLength > static_cast<qint32>(std::numeric_limits<quint16>::max())) {
            fail(error, QStringLiteral("Remote USB control setup is invalid"));
            return SubmitDisposition::Rejected;
        }
        if (request.endpointMetadataValid &&
            (request.endpointMetadata.interfaceNumber != 0u ||
             request.endpointMetadata.alternateSetting != 0u ||
             request.endpointMetadata.address != 0u ||
             request.endpointMetadata.attributes != LIBUSB_TRANSFER_TYPE_CONTROL ||
             request.endpointMetadata.maxPacketSize != 0u ||
             request.endpointMetadata.interval != 0u ||
             request.endpointMetadata.reserved != 0u)) {
            fail(error, QStringLiteral("Remote USB control endpoint metadata is invalid"));
            return SubmitDisposition::Rejected;
        }
    } else {
        for (const quint8 byte : request.setup) {
            if (byte != 0u) {
                fail(error, QStringLiteral("Remote USB data transfer has a setup packet"));
                return SubmitDisposition::Rejected;
            }
        }
    }

    std::unique_lock<std::recursive_mutex> ioLock(m_impl->ioMutex);
    Endpoint endpoint;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (!m_impl->claimed || m_impl->handle == nullptr ||
            m_impl->transfers.size() >= m_impl->config.maxInflight) {
            fail(error, QStringLiteral("Remote USB device is not available for transfer"));
            return SubmitDisposition::Rejected;
        }
        if (!control) {
            const quint8 expectedAddress = static_cast<quint8>(
                request.endpoint | (request.direction == TransferDirection::In ? 0x80u : 0u));
            bool found = false;
            for (const Endpoint &candidate : m_impl->claimedSnapshot.endpoints) {
                if (candidate.address == expectedAddress && candidate.reserved == 0u &&
                    candidate.maxPacketSize != 0u &&
                    ((candidate.attributes & 0x03u) == LIBUSB_TRANSFER_TYPE_BULK ||
                     (candidate.attributes & 0x03u) == LIBUSB_TRANSFER_TYPE_INTERRUPT)) {
                    if (request.endpointMetadataValid &&
                        (candidate.interfaceNumber != request.endpointMetadata.interfaceNumber ||
                         candidate.alternateSetting != request.endpointMetadata.alternateSetting ||
                         candidate.address != request.endpointMetadata.address)) {
                        continue;
                    }
                    endpoint = candidate;
                    found = true;
                    if (candidate.alternateSetting == 0u) {
                        break;
                    }
                }
            }
            if (!found) {
                fail(error, QStringLiteral("Remote USB endpoint is not available"));
                return SubmitDisposition::Rejected;
            }
            if (request.endpointMetadataValid &&
                (request.endpointMetadata.reserved != 0u ||
                 request.endpointMetadata.address != endpoint.address ||
                 request.endpointMetadata.maxPacketSize == 0u ||
                 request.endpointMetadata.maxPacketSize != endpoint.maxPacketSize ||
                 request.endpointMetadata.interfaceNumber != endpoint.interfaceNumber ||
                 request.endpointMetadata.alternateSetting != endpoint.alternateSetting ||
                 request.endpointMetadata.attributes != endpoint.attributes ||
                 request.endpointMetadata.interval != endpoint.interval)) {
                fail(error, QStringLiteral("Remote USB endpoint metadata is invalid"));
                return SubmitDisposition::Rejected;
            }
            const int interfaceNumber = endpoint.interfaceNumber;
            const int alternate = endpoint.alternateSetting;
            const auto active = m_impl->activeAlternates.find(interfaceNumber);
            const auto inFlight = m_impl->interfaceInflight.find(interfaceNumber);
            if (active != m_impl->activeAlternates.end() && active->second != alternate &&
                inFlight != m_impl->interfaceInflight.end() && inFlight->second != 0u) {
                fail(error, QStringLiteral("Remote USB alternate setting is busy"));
                return SubmitDisposition::Rejected;
            }
        }
    }

    if (!control) {
        const int interfaceNumber = endpoint.interfaceNumber;
        const int alternate = endpoint.alternateSetting;
        int activeAlternate = 0;
        bool needSwitch = false;
        {
            std::lock_guard<std::mutex> lock(m_impl->mutex);
            const auto active = m_impl->activeAlternates.find(interfaceNumber);
            activeAlternate = active == m_impl->activeAlternates.end() ? 0 : active->second;
            needSwitch = activeAlternate != alternate;
        }
        if (needSwitch) {
            const int result = libusb_set_interface_alt_setting(
                m_impl->handle, interfaceNumber, alternate);
            if (result != 0) {
                fail(error, libusbErrorMessage("select alternate setting", result));
                return SubmitDisposition::Rejected;
            }
            std::lock_guard<std::mutex> lock(m_impl->mutex);
            m_impl->activeAlternates[interfaceNumber] = alternate;
        }
    }

    std::shared_ptr<Impl::TransferState> state;
    try {
        state = std::make_shared<Impl::TransferState>();
        state->owner = m_impl.get();
        state->token = request.requestToken;
        state->request = request;
        state->completionCallback = std::move(completionCallback);
        const std::size_t requestedLength = static_cast<std::size_t>(request.transferBufferLength);
        const std::size_t storageLength = control
            ? std::max<std::size_t>(8u + requestedLength, 8u)
            : std::max<std::size_t>(requestedLength, 1u);
        state->storage.resize(storageLength);
        if (control) {
            std::memcpy(state->storage.data(), request.setup.data(), 8u);
            if (request.direction == TransferDirection::Out && requestedLength != 0u) {
                std::memcpy(state->storage.data() + 8u, request.data.constData(), requestedLength);
            }
        } else if (request.direction == TransferDirection::Out && requestedLength != 0u) {
            std::memcpy(state->storage.data(), request.data.constData(), requestedLength);
            state->interfaceTracked = true;
            state->interfaceNumber = endpoint.interfaceNumber;
        } else {
            state->interfaceTracked = true;
            state->interfaceNumber = endpoint.interfaceNumber;
        }
        state->transfer = libusb_alloc_transfer(0);
        if (state->transfer == nullptr) {
            fail(error, QStringLiteral("Remote USB transfer allocation failed"));
            return SubmitDisposition::Rejected;
        }
        if (control) {
            libusb_fill_control_transfer(
                state->transfer, m_impl->handle, state->storage.data(),
                &Impl::transferCallback, state.get(), m_impl->config.transferTimeoutMs);
            state->transfer->flags = transferFlags(request.transferFlags);
        } else if ((endpoint.attributes & 0x03u) == LIBUSB_TRANSFER_TYPE_INTERRUPT) {
            libusb_fill_interrupt_transfer(
                state->transfer, m_impl->handle, endpoint.address,
                state->storage.data(), static_cast<int>(requestedLength),
                &Impl::transferCallback, state.get(), m_impl->config.transferTimeoutMs);
            state->transfer->flags = transferFlags(request.transferFlags);
        } else {
            libusb_fill_bulk_transfer(
                state->transfer, m_impl->handle, endpoint.address,
                state->storage.data(), static_cast<int>(requestedLength),
                &Impl::transferCallback, state.get(), m_impl->config.transferTimeoutMs);
            state->transfer->flags = transferFlags(request.transferFlags);
        }
    } catch (const std::exception &exception) {
        if (state != nullptr && state->transfer != nullptr) {
            libusb_free_transfer(state->transfer);
            state->transfer = nullptr;
        }
        fail(error, QString::fromUtf8(exception.what()));
        return SubmitDisposition::Rejected;
    } catch (...) {
        if (state != nullptr && state->transfer != nullptr) {
            libusb_free_transfer(state->transfer);
            state->transfer = nullptr;
        }
        fail(error, QStringLiteral("Remote USB transfer allocation failed"));
        return SubmitDisposition::Rejected;
    }

    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (!m_impl->claimed || m_impl->handle == nullptr ||
            m_impl->transfers.size() >= m_impl->config.maxInflight ||
            m_impl->transfers.find(state->token) != m_impl->transfers.end()) {
            libusb_free_transfer(state->transfer);
            state->transfer = nullptr;
            fail(error, QStringLiteral("Remote USB transfer table is busy"));
            return SubmitDisposition::Rejected;
        }
        m_impl->transfers.emplace(state->token, state);
        if (state->interfaceTracked) {
            ++m_impl->interfaceInflight[state->interfaceNumber];
        }
    }

    int result = LIBUSB_ERROR_INVALID_PARAM;
    {
        /* Keep the transfer object alive until libusb has returned.  A real
         * libusb backend completes asynchronously, but this also makes a
         * synchronous/mock implementation well-defined. */
        std::unique_lock<std::recursive_mutex> operationLock(
            state->operationMutex);
        result = libusb_submit_transfer(state->transfer);
    }
    if (result != 0) {
        std::unique_lock<std::recursive_mutex> operationLock(
            state->operationMutex);
        {
            std::lock_guard<std::mutex> lock(m_impl->mutex);
            m_impl->transfers.erase(state->token);
            if (state->interfaceTracked) {
                auto count = m_impl->interfaceInflight.find(state->interfaceNumber);
                if (count != m_impl->interfaceInflight.end()) {
                    if (count->second > 0u) {
                        --count->second;
                    }
                    if (count->second == 0u) {
                        m_impl->interfaceInflight.erase(count);
                    }
                }
            }
            libusb_transfer *failedTransfer = state->transfer;
            state->transfer = nullptr;
            if (failedTransfer != nullptr) {
                libusb_free_transfer(failedTransfer);
            }
        }
        fail(error, libusbErrorMessage("submit transfer", result));
        return SubmitDisposition::Rejected;
    }
    SubmitDisposition disposition = SubmitDisposition::Pending;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        /* libusb normally invokes callbacks after submit returns. Recognize
         * a synchronous test backend so the disposition remains truthful. */
        if (state->terminal &&
            m_impl->transfers.find(state->token) == m_impl->transfers.end()) {
            disposition = SubmitDisposition::Completed;
        }
    }
    /* A synchronous callback may have re-entered release() and left a
     * deferred close pending.  The handle must only be closed after the
     * submit operation has returned and the adapter-wide lock is released. */
    ioLock.unlock();
    m_impl->finalizeDeferredRelease();
    return disposition;
}

CancelDisposition RemoteUsbLibusbAdapter::cancel(
    const TransferRequest &request,
    CancelCompletionCallback cancelCompletionCallback,
    qint32 *statusOut,
    QString *error)
{
    if (statusOut != nullptr) {
        *statusOut = kStatusNoDevice;
    }
    if (!isAvailable() || request.requestToken == 0u) {
        fail(error, unavailableMessage());
        return CancelDisposition::NotFound;
    }
    std::shared_ptr<Impl::TransferState> state;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        const auto iterator = m_impl->transfers.find(request.requestToken);
        if (!m_impl->claimed || m_impl->handle == nullptr ||
            iterator == m_impl->transfers.end() || iterator->second == nullptr ||
            iterator->second->terminal || iterator->second->transfer == nullptr) {
            if (statusOut != nullptr) {
                *statusOut = kStatusNoEntry;
            }
            return CancelDisposition::NotFound;
        }
        state = iterator->second;
        if (!state->cancelRequested) {
            state->cancelRequested = true;
            state->cancelStatus = 0;
            state->cancelCompletionCallback = std::move(cancelCompletionCallback);
        } else if (!state->cancelCompletionCallback && cancelCompletionCallback) {
            state->cancelCompletionCallback = std::move(cancelCompletionCallback);
        }
    }
    int result = LIBUSB_ERROR_NOT_FOUND;
    {
        /* The completion callback may run concurrently with this call.  Hold
         * the per-transfer lock across the libusb operation so its callback
         * cannot free the object before libusb has consumed the pointer. */
        std::unique_lock<std::recursive_mutex> operationLock(
            state->operationMutex);
        libusb_transfer *transfer = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_impl->mutex);
            if (!m_impl->claimed || m_impl->handle == nullptr ||
                state->terminal || state->transfer == nullptr) {
                if (statusOut != nullptr) {
                    *statusOut = kStatusNoEntry;
                }
                return CancelDisposition::NotFound;
            }
            transfer = state->transfer;
        }
        result = libusb_cancel_transfer(transfer);
    }
    const qint32 mappedStatus = result == 0 ? 0 : mapLibusbError(result);
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (!state->cancelCallbackSent) {
            state->cancelStatus = mappedStatus;
        }
    }
    if (statusOut != nullptr) {
        *statusOut = mappedStatus;
    }
    if (m_impl->context != nullptr) {
        libusb_interrupt_event_handler(m_impl->context);
    }
    /* A mock or unusual backend may invoke the cancellation callback
     * synchronously on this thread.  Give a release requested from that
     * callback a chance to close once libusb_cancel_transfer has returned. */
    m_impl->finalizeDeferredRelease();
    return CancelDisposition::Pending;
}

#else // REMOTE_USB_HAS_LIBUSB

struct RemoteUsbLibusbAdapter::Impl {
    explicit Impl(const RemoteUsbLibusbAdapterConfig &) {}
};

QVector<DeviceSnapshot> RemoteUsbLibusbAdapter::enumerate(QString *error)
{
    fail(error, unavailableMessage());
    return {};
}

bool RemoteUsbLibusbAdapter::claim(const DeviceSnapshot &, QString *error)
{
    return fail(error, unavailableMessage());
}

void RemoteUsbLibusbAdapter::release() noexcept {}

EndpointResolution RemoteUsbLibusbAdapter::resolveEndpoint(
    const TransferRequest &, Endpoint *endpointOut, QString *error) const
{
    if (endpointOut != nullptr) {
        *endpointOut = Endpoint {};
    }
    fail(error, unavailableMessage());
    return EndpointResolution::Rejected;
}

SubmitDisposition RemoteUsbLibusbAdapter::submitControl(
    const TransferRequest &, TransferCompletionCallback, QString *error)
{
    fail(error, unavailableMessage());
    return SubmitDisposition::Rejected;
}

SubmitDisposition RemoteUsbLibusbAdapter::submitData(
    const TransferRequest &, TransferCompletionCallback, QString *error)
{
    fail(error, unavailableMessage());
    return SubmitDisposition::Rejected;
}

CancelDisposition RemoteUsbLibusbAdapter::cancel(
    const TransferRequest &, CancelCompletionCallback, qint32 *statusOut,
    QString *error)
{
    if (statusOut != nullptr) {
        *statusOut = kStatusNoDevice;
    }
    fail(error, unavailableMessage());
    return CancelDisposition::NotFound;
}

#endif // REMOTE_USB_HAS_LIBUSB

} // namespace RemoteUsb
