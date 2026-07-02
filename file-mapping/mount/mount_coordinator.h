#pragma once

#include "mount_provider.h"

#include <QHash>
#include <QList>

namespace FileMapping {

class MountCoordinator
{
public:
    explicit MountCoordinator(QList<MountProviderPtr> providers = {});

    void addProvider(MountProviderPtr provider);
    QList<MountProviderPtr> providers() const;

    MountResult ensureMounted(const MountRequest& request);
    MountError reveal(const QString& hostUuid, const QString& sessionId);
    MountStatus status(const QString& hostUuid, const QString& sessionId);
    void unmount(const QString& hostUuid, const QString& sessionId);
    void unmountAll();

private:
    struct ActiveMount {
        MountProviderPtr provider;
        MountId id;
        MountStatus status;
    };

    static QString sessionKey(const QString& hostUuid, const QString& sessionId);
    static QString sessionKey(const MountRequest& request);
    MountResult unavailableResult(const QString& message) const;

    QList<MountProviderPtr> m_Providers;
    QHash<QString, ActiveMount> m_ActiveMounts;
};

} // namespace FileMapping
