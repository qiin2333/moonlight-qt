#include "mount_coordinator.h"

#include <QStringList>

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
        MountResult result = unavailableResult(QStringLiteral("Host files cannot be mounted without an active host session."));
        result.diagnostics.append(QStringLiteral("coordinator unavailable: missing active host session"));
        return result;
    }
    if (!request.vfs) {
        MountResult result = unavailableResult(QStringLiteral("Host files cannot be mounted because the remote file tree is unavailable."));
        result.diagnostics.append(QStringLiteral("coordinator unavailable: remote file tree missing"));
        return result;
    }

    QStringList diagnostics;
    const QString key = sessionKey(request);
    auto active = m_ActiveMounts.find(key);
    if (active != m_ActiveMounts.end() && active->provider) {
        MountStatus current = active->provider->status(active->id);
        active->status = current;
        if (current.state == MountState::Mounted || current.state == MountState::Mounting) {
            MountResult result;
            result.id = active->id;
            result.status = current;
            result.providerName = active->provider->displayName();
            result.diagnostics.append(QStringLiteral("provider=\"%1\" reused=true state=%2 display_path=\"%3\"")
                                      .arg(result.providerName)
                                      .arg(static_cast<int>(current.state))
                                      .arg(current.displayPath));
            return result;
        }
        active->provider->unmount(active->id);
        m_ActiveMounts.erase(active);
    }

    MountResult lastResult = unavailableResult(QStringLiteral("No host files mount provider is available."));
    MountResult firstConcreteFailure;
    bool hasConcreteFailure = false;
    for (const MountProviderPtr& provider : m_Providers) {
        if (!provider) {
            continue;
        }

        const QString providerName = provider->displayName();
        MountResult result = provider->mount(request);
        const QStringList providerDiagnostics = result.diagnostics;
        result.providerName = providerName;
        diagnostics.append(QStringLiteral("provider=\"%1\" ok=%2 state=%3 error_kind=%4 error=\"%5\" message=\"%6\" display_path=\"%7\"")
                           .arg(providerName)
                           .arg(result.ok() ? QStringLiteral("true") : QStringLiteral("false"))
                           .arg(static_cast<int>(result.status.state))
                           .arg(static_cast<int>(result.error.kind))
                           .arg(result.error.message)
                           .arg(result.status.message)
                           .arg(result.status.displayPath));
        for (const QString& diagnostic : providerDiagnostics) {
            if (!diagnostic.isEmpty()) {
                diagnostics.append(QStringLiteral("provider=\"%1\" diagnostic=\"%2\"")
                                   .arg(providerName, diagnostic));
            }
        }
        result.diagnostics = diagnostics;
        lastResult = result;
        if (!result.ok()) {
            if (!hasConcreteFailure && result.error.kind != ErrorKind::Unsupported) {
                firstConcreteFailure = result;
                hasConcreteFailure = true;
            }
            continue;
        }

        ActiveMount mounted;
        mounted.provider = provider;
        mounted.id = result.id;
        mounted.status = result.status;
        m_ActiveMounts.insert(key, mounted);
        result.diagnostics = diagnostics;
        return result;
    }

    if (hasConcreteFailure) {
        firstConcreteFailure.diagnostics = diagnostics;
        return firstConcreteFailure;
    }
    lastResult.diagnostics = diagnostics;
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
