#pragma once

#include <QString>

namespace FileMapping {

struct MountId {
    QString value;

    bool isEmpty() const { return value.isEmpty(); }
};

enum class MountState {
    Unmounted,
    Mounting,
    Mounted,
    Unavailable,
    Error,
};

struct MountStatus {
    MountState state = MountState::Unmounted;
    QString displayPath;
    QString message;
};

} // namespace FileMapping
