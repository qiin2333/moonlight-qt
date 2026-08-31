#pragma once

#include <QtGlobal>

namespace RemoteUsbAgent {

inline constexpr quint32 kProtocolVersion = 1u;
inline constexpr qsizetype kMaxMessageBytes = 64 * 1024;
inline constexpr qsizetype kMaxTokenBytes = 256;

} // namespace RemoteUsbAgent
