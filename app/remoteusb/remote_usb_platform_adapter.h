#pragma once

/*
 * Qt-side boundary for the platform half of Remote USB.
 *
 * This header intentionally contains only value objects and callbacks.  The
 * shared C protocol core is adapted to these types by the session binding;
 * Qt, Android, file-descriptor, and MoonBridge objects must never cross
 * that boundary.
 */

#include <QtGlobal>

#include <QByteArray>
#include <QString>
#include <QVector>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace RemoteUsb {

inline constexpr std::uint32_t kPlatformAdapterAbiVersion = 1u;
inline constexpr std::uint32_t kWireProtocolVersion = 1u;
/* These limits mirror the version-1 C wire/PDU headers. Keep the values in
 * one place in each binding and add compile-time checks when the C headers are
 * available; never size a stream accumulator from peer-provided values. */
inline constexpr std::size_t kBrokerHelloSize = 84u;
inline constexpr std::size_t kWireHeaderSize = 32u;
inline constexpr std::size_t kWireMaxPayload = 128u * 1024u;
inline constexpr std::size_t kWireMaxFrameSize =
    kWireHeaderSize + kWireMaxPayload;
inline constexpr std::size_t kMaxReassemblySize = 1024u * 1024u;
inline constexpr std::size_t kMaxFragments = 4096u;
inline constexpr std::size_t kPduHeaderSize = 48u;
inline constexpr std::size_t kMaxTransferSize =
    (1024u * 1024u) - kPduHeaderSize;
inline constexpr std::size_t kBusIdMaxBytes = 31u;
inline constexpr std::size_t kRawDescriptorMaxBytes = 64u * 1024u;
inline constexpr std::size_t kEndpointMaxCount = 256u;
inline constexpr std::size_t kSetupPacketSize = 8u;

/* These records are copied into the shared C ABI at the adapter boundary. */
struct Endpoint {
    quint8 interfaceNumber = 0;
    quint8 alternateSetting = 0;
    quint8 address = 0;
    quint8 attributes = 0;
    quint16 maxPacketSize = 0;
    quint8 interval = 0;
    quint8 reserved = 0;
};

/* Keep this value type byte-for-byte compatible with the C endpoint record.
 * The fields are intentionally listed in wire order; do not add members. */
static_assert(sizeof(Endpoint) == 8u,
              "Remote USB endpoint layout must remain eight bytes");
static_assert(offsetof(Endpoint, maxPacketSize) == 4u,
              "Remote USB endpoint max packet offset changed");

/*
 * A snapshot is safe to retain after enumerate() returns.  deviceId is an
 * adapter-local opaque key; busId is the broker-visible opaque USB/IP name and
 * must not contain an Android device node, file descriptor, or other handle.
 * A binding must reject an empty busId, embedded NUL bytes, or a busId longer
 * than kBusIdMaxBytes before passing it to the shared wire codec.
 */
struct DeviceSnapshot {
    QByteArray deviceId;
    QByteArray busId;
    QString displayName;
    quint16 vendorId = 0;
    quint16 productId = 0;
    quint16 deviceBcd = 0;
    quint8 deviceClass = 0;
    quint8 deviceSubclass = 0;
    quint8 deviceProtocol = 0;
    QByteArray rawDescriptors;
    QVector<Endpoint> endpoints;
    bool hasIsochronousEndpoints = false;
};

enum class TransferDirection : quint32 {
    Out = 0u,
    In = 1u,
};

enum class TransferKind : quint8 {
    Control = 1u,
    Data = 2u,
};

static_assert(sizeof(TransferDirection) == sizeof(quint32),
              "TransferDirection ABI changed");
static_assert(sizeof(TransferKind) == sizeof(quint8),
              "TransferKind ABI changed");
static_assert(static_cast<quint32>(TransferDirection::Out) == 0u &&
                  static_cast<quint32>(TransferDirection::In) == 1u,
              "TransferDirection values changed");
static_assert(static_cast<quint8>(TransferKind::Control) == 1u &&
                  static_cast<quint8>(TransferKind::Data) == 2u,
              "TransferKind values changed");

/*
 * A transfer is a value copy of the request view supplied by the shared
 * executor. `deviceId` is the USB/IP wire `devid`; it is unrelated to the
 * adapter-local QByteArray deviceId in DeviceSnapshot. `endpoint` is the low
 * four-bit endpoint number, while Endpoint.address includes the USB direction
 * bit (0x80) when metadata is returned.
 *
 * For IN requests, data is empty and transferBufferLength is the requested
 * size. For OUT requests, data.size() must equal transferBufferLength. v1
 * accepts only numberOfPackets == 0 (wire -1 is normalized by the C decoder),
 * and all lengths/frame/interval values must be non-negative. Control (ep0)
 * requests must have setup[0].bit7 matching direction and the little-endian
 * setup wLength (setup[6..7]) equal to transferBufferLength; non-control
 * requests must have an all-zero setup packet. `kind` is derived by the C
 * executor (Control exactly when endpoint == 0, Data otherwise); a binding
 * must validate or overwrite it rather than treating it as caller-selectable.
 */
struct TransferRequest {
    quint64 requestToken = 0;
    quint32 sequence = 0;
    quint32 deviceId = 0;
    TransferDirection direction = TransferDirection::Out;
    quint32 endpoint = 0;
    quint32 transferFlags = 0;
    qint32 transferBufferLength = 0;
    qint32 startFrame = 0;
    qint32 numberOfPackets = 0;
    qint32 interval = 0;
    std::array<quint8, kSetupPacketSize> setup {};
    QByteArray data;
    TransferKind kind = TransferKind::Data;
    Endpoint endpointMetadata {};
    bool endpointMetadataValid = false;
};

/*
 * A completion is also a value object. status may be zero or a negative
 * USB/IP errno; actualLength, startFrame, and errorCount must be non-negative,
 * and actualLength must not exceed the submitted buffer length. `data` is only
 * valid for IN transfers and must be exactly actualLength bytes (or empty when
 * actualLength is zero); OUT completions must leave it empty.
 */
struct TransferCompletion {
    qint32 status = 0;
    qint32 actualLength = 0;
    qint32 startFrame = 0;
    qint32 errorCount = 0;
    QByteArray data;
};

enum class SubmitDisposition : quint8 {
    /* completionCallback must be called exactly once at a later time. */
    Pending = 0u,
    /* completionCallback must be called exactly once before submit returns. */
    Completed = 1u,
    /* No callback is made; error describes why the request was rejected. */
    Rejected = 2u,
};

/* Values intentionally match ml_remote_usb_executor_submit_result.  The
 * names differ only because a platform adapter reports a rejected operation
 * while the C ABI calls it a failed submit. */
static_assert(static_cast<quint8>(SubmitDisposition::Pending) == 0u,
              "SubmitDisposition::Pending ABI value changed");
static_assert(static_cast<quint8>(SubmitDisposition::Completed) == 1u,
              "SubmitDisposition::Completed ABI value changed");
static_assert(static_cast<quint8>(SubmitDisposition::Rejected) == 2u,
              "SubmitDisposition::Rejected ABI value changed");

enum class CancelDisposition : quint8 {
    /* cancelCompletionCallback will be called exactly once later. */
    Pending = 0u,
    /* statusOut is the terminal RET_UNLINK status. */
    Completed = 1u,
    /* The target was not present; the core still emits a terminal submit. */
    NotFound = 2u,
    /* statusOut is used as the synthetic submit status when negative. */
    Failed = 3u,
};

static_assert(static_cast<quint8>(CancelDisposition::Pending) == 0u,
              "CancelDisposition::Pending ABI value changed");
static_assert(static_cast<quint8>(CancelDisposition::Completed) == 1u,
              "CancelDisposition::Completed ABI value changed");
static_assert(static_cast<quint8>(CancelDisposition::NotFound) == 2u,
              "CancelDisposition::NotFound ABI value changed");
static_assert(static_cast<quint8>(CancelDisposition::Failed) == 3u,
              "CancelDisposition::Failed ABI value changed");

enum class EndpointResolution : quint8 {
    Found = 0u,
    NotFound = 1u,
    Rejected = 2u,
};

static_assert(static_cast<quint8>(EndpointResolution::Found) == 0u &&
                  static_cast<quint8>(EndpointResolution::NotFound) == 1u &&
                  static_cast<quint8>(EndpointResolution::Rejected) == 2u,
              "EndpointResolution values changed");

using TransferCompletionCallback =
    std::function<void(quint64 requestToken, TransferCompletion completion)>;
using CancelCompletionCallback =
    std::function<void(quint64 requestToken, qint32 status)>;

/*
 * Implements local USB ownership and operations for one selected device.
 * Implementations may use libusb, native OS APIs, or a platform service, but
 * expose no native handle through this interface.
 *
 * Callback rules are deliberately strict so the C executor can safely retain
 * an in-flight request: Pending means exactly one later completion, while
 * Completed means a synchronous completion before the method returns.
 * The TransferRequest reference is borrowed for the duration of the method
 * call only.  An implementation that returns Pending must deep-copy every
 * field it needs (including setup and QByteArray data) before returning; it
 * must never capture the reference or a QByteArray backing pointer in an
 * asynchronous operation.  The same rule applies to a Pending cancel.
 * resolveEndpoint() is a synchronous lookup used only while dispatching a
 * submit.  It intentionally receives the normalized request view and does
 * not expose the original PDU command, unlink fields, or wire buffer.
 * When it returns Found, endpointOut must be populated with value metadata:
 * reserved == 0, address == 0 for endpoint zero, otherwise
 * (endpoint number | (direction == In ? 0x80 : 0)), and maxPacketSize must be
 * non-zero for every non-zero endpoint.  A null endpointOut is a rejection.
 * Calls into the shared session/executor must be serialized by one owner event
 * loop. Callbacks may arrive on any thread, must not directly touch Qt UI
 * state, and should enqueue completion work onto that owner loop before
 * calling the C core. Implementations and callbacks must catch C++ exceptions;
 * no exception may cross this interface or the C shared-core callback
 * boundary.
 */
class RemoteUsbPlatformAdapter
{
public:
    virtual ~RemoteUsbPlatformAdapter() = default;

    virtual QVector<DeviceSnapshot> enumerate(QString *error = nullptr) = 0;

    /* Only one snapshot is claimed by an adapter instance at a time. */
    virtual bool claim(const DeviceSnapshot &device,
                       QString *error = nullptr) = 0;
    /* release() must cancel/drain callbacks before it returns. */
    virtual void release() noexcept = 0;

    virtual EndpointResolution resolveEndpoint(
        const TransferRequest &request,
        Endpoint *endpointOut,
        QString *error = nullptr) const = 0;

    virtual SubmitDisposition submitControl(
        const TransferRequest &request,
        TransferCompletionCallback completionCallback,
        QString *error = nullptr) = 0;

    virtual SubmitDisposition submitData(
        const TransferRequest &request,
        TransferCompletionCallback completionCallback,
        QString *error = nullptr) = 0;

    /* statusOut is non-null and must be set for Completed/Failed results.
     * A Pending callback may use only the original requestToken and must fire
     * exactly once, even when cancellation races completion. */
    virtual CancelDisposition cancel(
        const TransferRequest &request,
        CancelCompletionCallback cancelCompletionCallback,
        qint32 *statusOut,
        QString *error = nullptr) = 0;
};

struct ChannelCapabilities {
    quint32 protocolVersion = 0;
    bool authenticated = false;
    bool independent = false;

    bool usable() const noexcept
    {
        return protocolVersion == kWireProtocolVersion &&
               authenticated && independent;
    }
};

/*
 * Full-duplex byte channel used by the caller-driven shared transport pump.
 * It is intentionally not a QIODevice: a platform implementation may wrap a
 * QSslSocket, native socket, or service IPC without exposing that type here.
 * BytesCallback receives arbitrary byte-stream chunks: one callback may hold a
 * partial frame or several complete frames. The binding owns a bounded
 * accumulator, consumes exactly the first kBrokerHelloSize bytes as the
 * unframed broker HELLO, and then validates kWireHeaderSize-byte RUSB headers
 * before forwarding complete frames to the shared transport. It must consume
 * a chunk incrementally (HELLO remainder, header, then payload) instead of
 * appending an arbitrarily large callback chunk to the accumulator; at no
 * point may parser storage exceed kWireMaxFrameSize plus fixed parser state.
 * An oversized/unacceptable chunk is a protocol failure or is back-pressured,
 * never an unbounded allocation. All callbacks must be posted to the same
 * owner event loop used for C transport calls.
 */
class RemoteUsbByteChannel
{
public:
    using BytesCallback = std::function<void(QByteArray bytes)>;
    using ErrorCallback = std::function<void(QString message)>;
    using ClosedCallback = std::function<void()>;

    /* The owner must keep the object alive until close() has delivered
     * closedCallback and every in-flight callback has returned. */
    virtual ~RemoteUsbByteChannel() = default;

    virtual ChannelCapabilities capabilities() const noexcept = 0;

    /* Must be called before start() on the owner event loop. Callbacks may run
     * on an I/O thread, but they must only enqueue work onto that loop. */
    virtual void setCallbacks(BytesCallback bytesCallback,
                              ErrorCallback errorCallback,
                              ClosedCallback closedCallback) = 0;

    /*
     * start() returns only after the underlying channel is authenticated and
     * usable. The Remote USB broker HELLO is still owned by the shared core;
     * its send_hello callback must be the first bytes written on this channel,
     * before any RUSB frame is queued.
     */
    /* Must not race setCallbacks(), send(), or close(). */
    virtual bool start(QString *error = nullptr) = 0;

    /* A successful send copies/enqueues every byte into a bounded queue before
     * returning. It is an all-or-nothing operation: false means closed or
     * unable to accept the complete byte array and the owner should tear down
     * the session. Must be serialized with start()/close(). */
    virtual bool send(const QByteArray &bytes,
                      QString *error = nullptr) = 0;

    /* Idempotent; must unblock pending I/O and eventually invoke closedCallback.
     * The caller must wait for that callback and callback quiescence (or an
     * equivalent implementation guarantee) before destroying the shared
     * transport or this channel. Invoke it on the owner event loop and do not
     * race it with start()/send(); an I/O callback should enqueue close rather
     * than call it concurrently. */
    virtual void close() noexcept = 0;

    virtual bool isOpen() const noexcept = 0;
};

} // namespace RemoteUsb
