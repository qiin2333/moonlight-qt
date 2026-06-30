#include "mount_coordinator.h"

#include <utility>

namespace FileMapping {

MountCoordinator::MountCoordinator(QList<MountProviderPtr> providers)
    : m_Providers(std::move(providers))
{
}

void MountCoordinator::addProvider(MountProviderPtr provider)
{
    if (provider) {
        m_Providers.append(std::move(provider));
    }
}

QList<MountProviderPtr> MountCoordinator::providers() const
{
    return m_Providers;
}

MountResult MountCoordinator::ensureMounted(const MountRequest& request)
{
    if (request.hostUuid.isEmpty() || request.sessionId.isEmpty()) {
        return unavailableResult(QStringLiteral("Host files cannot be mounted without an active host session."));
    }
    if (!request.vfs) {
        return unavailableResult(QStringLiteral("Host files cannot be mounted because the remote file tree is unavailable."));
    }

    const QString key = sessionKey(request);
    auto active = m_ActiveMounts.find(key);
    if (active != m_ActiveMounts.end() && active->provider) {
        MountStatus current = active->provider->status(active->id);
        active->status = current;
        if (current.state == MountState::Mounted || current.state == MountState::Mounting) {
            MountResult result;
            result.id = active->id;
            result.status = current;
            return result;
        }
        active->provider->unmount(active->id);
        m_ActiveMounts.erase(active);
    }

    MountResult lastResult = unavailableResult(QStringLiteral("No host files mount provider is available."));
    for (const MountProviderPtr& provider : m_Providers) {
        if (!provider) {
            continue;
        }

        MountResult result = provider->mount(request);
        lastResult = result;
        if (!result.ok()) {
            continue;
        }

        ActiveMount mounted;
        mounted.provider = provider;
        mounted.id = result.id;
        mounted.status = result.status;
        m_ActiveMounts.insert(key, mounted);
        return result;
    }

    return lastResult;
}

MountError MountCoordinator::reveal(const QString& hostUuid, const QString& sessionId)
{
    const QString key = sessionKey(hostUuid, sessionId);
    auto active = m_ActiveMounts.find(key);
    if (active == m_ActiveMounts.end() || !active->provider) {
        return MountError::make(ErrorKind::NotFound, QStringLiteral("Host files are not mounted for this session."));
    }
    return active->provider->reveal(active->id);
}

MountStatus MountCoordinator::status(const QString& hostUuid, const QString& sessionId)
{
    const QString key = sessionKey(hostUuid, sessionId);
    auto active = m_ActiveMounts.find(key);
    if (active == m_ActiveMounts.end() || !active->provider) {
        MountStatus status;
        status.state = MountState::Unmounted;
        return status;
    }

    active->status = active->provider->status(active->id);
    return active->status;
}

void MountCoordinator::unmount(const QString& hostUuid, const QString& sessionId)
{
    const QString key = sessionKey(hostUuid, sessionId);
    auto active = m_ActiveMounts.find(key);
    if (active == m_ActiveMounts.end()) {
        return;
    }
    if (active->provider) {
        active->provider->unmount(active->id);
    }
    m_ActiveMounts.erase(active);
}

void MountCoordinator::unmountAll()
{
    const QList<ActiveMount> mounts = m_ActiveMounts.values();
    m_ActiveMounts.clear();
    for (const ActiveMount& mount : mounts) {
        if (mount.provider) {
            mount.provider->unmount(mount.id);
        }
    }
}

QString MountCoordinator::sessionKey(const QString& hostUuid, const QString& sessionId)
{
    return hostUuid + QLatin1Char(':') + sessionId;
}

QString MountCoordinator::sessionKey(const MountRequest& request)
{
    return sessionKey(request.hostUuid, request.sessionId);
}

MountResult MountCoordinator::unavailableResult(const QString& message) const
{
    MountResult result;
    result.error = MountError::make(ErrorKind::Unavailable, message);
    result.status.state = MountState::Unavailable;
    result.status.message = message;
    return result;
}

} // namespace FileMapping
