# Qt Remote USB Adapter Boundary

The Qt client carries an opt-in adapter boundary and session coordinator for
the shared Rust Remote USB core. The coordinator is connected to `Session` but is
disabled by default at runtime; a normal qmake build does not compile or link
any Remote USB code. When enabled, it uses a dedicated worker thread and a
separate broker/TLS channel, never the normal streaming/control channel.

This is the first in-process bridge-agent slice. The public boundary is kept
platform-neutral so the same core and adapter can later move into a standalone
`moonlight-usb-agent` process behind local IPC without changing the wire
protocol.

## Enable the boundary

The feature is enabled explicitly with `CONFIG+=remote_usb`. By default qmake
builds the Rust core at the commit pinned by the submodule:

```sh
git submodule update --init moonlight-remote-usb-core
qmake6 moonlight-qt.pro CONFIG+=remote_usb
```

The boundary smoke can be built independently of the full application:

```sh
qmake6 tests/remote_usb_qt_boundary/remote_usb_qt_boundary.pro \
  -o "$TMPDIR/remote-usb-qt-smoke/Makefile"
make -C "$TMPDIR/remote-usb-qt-smoke" -j2
"$TMPDIR/remote-usb-qt-smoke/remote_usb_qt_boundary"
```

Expected output is `remote_usb_qt_boundary=passed`.  The smoke deliberately
uses an in-process byte channel and platform adapter, so it does not require a
USB device, a broker, or a network connection.

For a packaged build, pass the include directory and complete prebuilt library
path instead:

```sh
qmake6 moonlight-qt.pro CONFIG+=remote_usb \
  MOONLIGHT_REMOTE_USB_CORE_INCLUDE_DIR=/opt/moonlight/include/moonlight/remoteusb \
  MOONLIGHT_REMOTE_USB_CORE_LIBRARY=/opt/moonlight/lib/libmoonlight-remote-usb-core.a
```

The same values may be provided as environment variables. The public header
and static library must come from the same core revision. The binding checks
both `RUSB_CORE_ABI_VERSION` and the independent RUSB protocol version before
starting a session.

The wire contract is normative in
`moonlight-remote-usb-core/contract/protocol-v1.md`. Cross-language
implementations must also pass the byte-exact vectors in
`moonlight-remote-usb-core/contract/vectors-v1.json`; native Rust structure
layout and Qt value objects are never wire formats.

## Boundary rules

`app/remoteusb/remote_usb_platform_adapter.h` contains Qt-side value records:

- `DeviceSnapshot` owns copied descriptors and opaque `deviceId`/`busId` values;
- `TransferRequest` and `TransferCompletion` own transfer bytes and metadata;
- `RemoteUsbPlatformAdapter` supplies claim/release, endpoint lookup, submit,
  and cancel callbacks;
- `RemoteUsbByteChannel` supplies an authenticated, independent, full-duplex
  byte stream and an idempotent `close()`.

The `TransferRequest` references in `resolveEndpoint()`, `submit*()`, and
`cancel()` are borrowed for the duration of the call. A backend returning
`Pending` must copy all required scalars, setup bytes, and `QByteArray` data
before returning; it must not retain the reference or its backing storage.
`resolveEndpoint()` is only the synchronous endpoint lookup used for a submit;
the reduced request view intentionally does not carry PDU command/unlink
fields. For `Found`, the endpoint record must have `reserved == 0`, address `0`
for endpoint zero or `endpoint | (direction == In ? 0x80 : 0)` otherwise, and a
non-zero `maxPacketSize` for every non-zero endpoint. A null output record is a
rejection.

`TransferRequest.deviceId` is the 32-bit USB/IP wire `devid`, not the opaque
`DeviceSnapshot.deviceId`. `endpoint` is a number in the low four bits;
resolved `Endpoint.address` carries the USB direction bit. Before invoking the
Rust core, the binding must validate non-negative lengths/frame/interval values,
`numberOfPackets == 0`, control setup direction and little-endian `wLength`,
zero setup bytes for non-control requests, and exact OUT payload length. For an
IN completion, `data.size()` must equal `actualLength` (and be empty at zero);
OUT completions must not return data. `actualLength`, `startFrame`, and
`errorCount` are non-negative and bounded by the submitted buffer, while
`status` remains a signed USB/IP status so negative errno values are preserved.
`cancel()` must initialize `statusOut` for immediate dispositions; a delayed
cancel callback may only use the original request token and still fires once
when completion and cancellation race.

The binding copies these values into the versioned `remoteusb.h` C ABI. The
channel's protocol version refers to the Remote USB wire protocol; it is
independent from the core ABI version. `start()` authenticates the underlying
byte channel, while the Rust core still performs the broker HELLO exchange.
The broker's negotiated reassembly size, fragment count, transfer size, and
byte/PDU windows are also passed into the session config and enforced inside
the core before allocation or USB dispatch.
Callbacks may run on an I/O thread, and a `Pending` operation must issue exactly
one terminal callback. The binding maps each `SubmitDisposition` explicitly
instead of passing a C++ enum through the ABI. A rejected submit carries
human-readable detail through `QString`; the binding maps it to a deterministic
USB/IP error status.

`BytesCallback` receives arbitrary TCP stream chunks, not complete RUSB frames;
a chunk can split a header or contain multiple frames. The binding must consume
each chunk incrementally (first the remaining HELLO bytes, then a header, then
its payload) instead of appending the whole chunk to an unbounded accumulator.
At most `kWireMaxFrameSize` bytes for one frame plus fixed parser state may be
retained; a chunk larger than that is sliced and processed, or rejected with
backpressure/protocol failure if it cannot be accepted. The parser consumes
exactly the first `kBrokerHelloSize` bytes as the unframed broker HELLO, then
parses the `kWireHeaderSize`-byte header, enforces `kWireMaxPayload`, and calls
`accept_frame()` only for a complete frame. The shared-core limits are: 1 MiB
reassembly, 4096 fragments, 48-byte USB/IP PDU header, and
`kMaxTransferSize` transfer bytes. A successful channel `send()` is
all-or-nothing and has copied or queued every byte into a bounded queue before
returning; `false` means the owner must tear down the session. `start()`,
`send()`, and `close()` are serialized on the owner loop and must not race one
another. Closing the channel must unblock both directions, and the caller must
wait for `closedCallback` and callback quiescence (or an equivalent drain
guarantee) before destroying the Rust session or channel. Adapter callbacks
must catch C++ exceptions; none may cross the C ABI.

All calls into the session, transport, and executor, including asynchronous
completion and cancel delivery, are serialized on one owner event loop. An I/O
thread may only enqueue a byte chunk or completion notification; it must not
call `accept_frame()`, `complete()`, or `cancel_complete()` directly while
another owner-loop operation is active.

The standard `USBIPServerForAndroid` project speaks the legacy USB/IP protocol
directly (`OP_REQ_DEVLIST`/`IMPORT` and raw 48-byte USB/IP PDUs on TCP/3240). It is
useful as an Android USB-device/backend reference, but it is not a drop-in
implementation of this authenticated RUSB channel: it has no 84-byte broker
HELLO or 32-byte RUSB framing. A client using the shared core must place a
small authenticated RUSB adapter in front of it, or explicitly implement a
separate legacy USB/IP mode with its own transport and security boundary.

The shared-core v1 executor accepts control, bulk, and interrupt transfers. It
normalizes non-isochronous `number_of_packets` values to zero and rejects
isochronous requests; a device exposing only isochronous endpoints must remain
hidden or be reported as unsupported until a negotiated v2 path exists.

The following objects must stay private to a platform implementation and never
appear in this header or in shared-core callbacks: Android `UsbDevice` and
`UsbDeviceConnection`, file descriptors, `QSslSocket`/`QIODevice`, native USB
handles, `MoonBridge` control or RTSP sockets, private keys, and UI `QObject`
instances. The byte channel must advertise protocol version 1, authentication,
and independence; there is no fallback to the ordinary Moonlight stream.

## Current implementation slice

1. The Qt broker-capability/client, TLS channel, Rust C-ABI binding, and libusb
   backend are implemented behind `CONFIG+=remote_usb remote_usb_libusb`; the
   core is pinned as a repository submodule.
2. `Session` exposes asynchronous enumerate/start/stop calls, performs cleanup
   on reconnect and teardown, and presents per-session consent in the streaming
   overlay. The legacy in-process coordinator remains opt-in with
   `MOONLIGHT_REMOTE_USB_ENABLE=1`.
3. The production `moonlight-usb-agent` lives in `usb-agent/`. It owns the
   authenticated `QLocalServer`, real libusb enumeration/claim path, broker
   capability request, pinned TLS tunnel, shared-core session, and teardown.
   `CONFIG+=remote_usb_agent` builds and packages it with Moonlight; the client
   discovers it without environment configuration.
