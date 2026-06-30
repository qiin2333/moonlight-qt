#pragma once

#include "mount_provider.h"

#include <QList>

namespace FileMapping {

MountProviderKind platformNativeMountProviderKind();
QList<MountProviderPtr> createDefaultMountProviders();

} // namespace FileMapping
