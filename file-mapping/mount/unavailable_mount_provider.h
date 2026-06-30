#pragma once

#include "mount_provider.h"

namespace FileMapping {

class UnavailableMountProvider : public MountProvider
{
public:
    UnavailableMountProvider(MountProviderKind kind, QString displayName, QString message);

    MountProviderKind kind() const override;
    QString displayName() const override;
    MountStatus status(const MountId& id) override;
    MountResult mount(const MountRequest& request) override;
    MountError reveal(const MountId& id) override;
    void unmount(const MountId& id) override;

private:
    MountProviderKind m_Kind;
    QString m_DisplayName;
    QString m_Message;
};

} // namespace FileMapping
