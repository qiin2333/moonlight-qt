#pragma once

#include "mount_provider.h"

#include <QHash>

namespace FileMapping {

class MacFileProviderMountProvider : public MountProvider
{
public:
    MacFileProviderMountProvider();

    MountProviderKind kind() const override;
    QString displayName() const override;
    MountStatus status(const MountId& id) override;
    MountResult mount(const MountRequest& request) override;
    MountError reveal(const MountId& id) override;
    void unmount(const MountId& id) override;

private:
    struct DomainState {
        MountStatus status;
    };

    static QString domainIdentifier(const MountRequest& request);
    static QString displayPathForDomain(const QString& domainId);
    static QString unavailableMessage();

    QHash<QString, DomainState> m_Domains;
};

} // namespace FileMapping
