#pragma once

/*
 * Compatibility include for the provisional name used by early clients.
 * RemoteUsbSessionBinding is the sole QObject state machine; this alias keeps
 * one lifecycle and one callback context instead of maintaining two bindings.
 */
#include "remote_usb_session_binding.h"

namespace RemoteUsb {

using RemoteUsbCoreBinding = RemoteUsbSessionBinding;
using RemoteUsbCoreBindingOptions = RemoteUsbSessionBindingOptions;

} // namespace RemoteUsb
