#pragma once

#include "mount_provider.h"

#include <QHash>

namespace FileMapping {

class MacFuseMountProvider : public MountProvider
{
public:
    MacFuseMountProvider();
    ~MacFuseMountProvider() override;

    MountProviderKind kind() const override;
    QString displayName() const override;
    MountStatus status(const MountId& id) override;
    MountResult mount(const MountRequest& request) override;
    MountError reveal(const MountId& id) override;
    void unmount(const MountId& id) override;

private:
    static QString safeName(const QString& name, const QString& fallback);
    static QString mountBasePath();
    static QString mountPathForRequest(const MountRequest& request);

    QHash<QString, MountStatus> m_KnownMounts;
};

} // namespace FileMapping
