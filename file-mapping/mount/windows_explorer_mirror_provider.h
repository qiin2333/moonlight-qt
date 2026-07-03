#pragma once

#include "macos_finder_mirror_provider.h"

namespace FileMapping {

class WindowsExplorerMirrorProvider : public MacOSFinderMirrorProvider
{
public:
    WindowsExplorerMirrorProvider();

    MountProviderKind kind() const override;
    QString displayName() const override;
    MountResult mount(const MountRequest& request) override;
    MountError reveal(const MountId& id) override;
};

} // namespace FileMapping
