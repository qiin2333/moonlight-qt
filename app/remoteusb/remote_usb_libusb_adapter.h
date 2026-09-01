#pragma once

/*
 * Optional libusb implementation of the Qt Remote USB platform boundary.
 *
 * The public header deliberately does not include libusb headers and does not
 * expose a libusb context, device, transfer, or native handle.  Builds that
 * do not opt in to libusb still get a linkable stub which reports the backend
 * as unavailable; this keeps the normal Moonlight binary independent from a
 * USB development package.
 */

#include "remote_usb_platform_adapter.h"

#include <QtGlobal>

#include <cstdint>
#include <memory>

namespace RemoteUsb {

struct RemoteUsbLibusbAdapterConfig {
    /* Timeout passed to libusb asynchronous transfers.  Zero selects 30 s. */
    quint32 transferTimeoutMs = 30u * 1000u;
    /* Maximum time the event worker waits between libusb event polls. */
    quint32 eventPollTimeoutMs = 50u;
    /* Bound the number of transfers retained by the adapter. */
    quint32 maxInflight = 256u;
    /* Request libusb to detach a kernel driver before claiming interfaces. */
    bool autoDetachKernelDriver = false;
};

class RemoteUsbLibusbAdapter final : public RemoteUsbPlatformAdapter
{
public:
    explicit RemoteUsbLibusbAdapter(
        const RemoteUsbLibusbAdapterConfig &config = {});
    ~RemoteUsbLibusbAdapter() override;

    RemoteUsbLibusbAdapter(const RemoteUsbLibusbAdapter &) = delete;
    RemoteUsbLibusbAdapter &operator=(const RemoteUsbLibusbAdapter &) = delete;

    /* True when this binary was compiled with libusb support. */
    static bool compiledWithLibusb() noexcept;
    /* True when libusb was initialized successfully for this instance. */
    bool isAvailable() const noexcept;

    QVector<DeviceSnapshot> enumerate(QString *error = nullptr) override;

    bool claim(const DeviceSnapshot &device,
               QString *error = nullptr) override;
    void release() noexcept override;

    EndpointResolution resolveEndpoint(const TransferRequest &request,
                                       Endpoint *endpointOut,
                                       QString *error = nullptr) const override;

    SubmitDisposition submitControl(
        const TransferRequest &request,
        TransferCompletionCallback completionCallback,
        QString *error = nullptr) override;

    SubmitDisposition submitData(
        const TransferRequest &request,
        TransferCompletionCallback completionCallback,
        QString *error = nullptr) override;

    CancelDisposition cancel(
        const TransferRequest &request,
        CancelCompletionCallback cancelCompletionCallback,
        qint32 *statusOut,
        QString *error = nullptr) override;

private:
    SubmitDisposition submitCommon(
        const TransferRequest &request,
        TransferCompletionCallback completionCallback,
        bool control,
        QString *error);

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace RemoteUsb
