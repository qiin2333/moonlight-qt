#include "mac_file_provider_mount_provider.h"

#include "mac_file_provider_bridge.h"

#include <QDesktopServices>
#include <QDir>
#include <QProcess>
#include <QUrl>

namespace FileMapping {
namespace {
QString sanitizeDomainPart(QString value, const QString& fallback)
{
    value = value.trimmed();
    if (value.isEmpty()) {
        value = fallback;
    }

    QString sanitized;
    sanitized.reserve(value.size());
    for (const QChar ch : value) {
        if (ch.isLetterOrNumber() || ch == QLatin1Char('-') || ch == QLatin1Char('_')) {
            sanitized.append(ch);
        }
        else {
            sanitized.append(QLatin1Char('_'));
        }
    }
    return sanitized.left(96);
}
} // namespace

MacFileProviderMountProvider::MacFileProviderMountProvider()
{
}

MountProviderKind MacFileProviderMountProvider::kind() const
{
    return MountProviderKind::MacFileProvider;
}

QString MacFileProviderMountProvider::displayName() const
{
    return QStringLiteral("macOS File Provider");
}

MountStatus MacFileProviderMountProvider::status(const MountId& id)
{
    const auto it = m_Domains.find(id.value);
    if (it == m_Domains.end()) {
        MountStatus status;
        status.state = MountState::Unavailable;
        status.message = unavailableMessage();
        return status;
    }
    return it->status;
}

MountResult MacFileProviderMountProvider::mount(const MountRequest& request)
{
    MountResult result;
    result.providerName = displayName();
    if (request.hostUuid.isEmpty() || request.sessionId.isEmpty()) {
        result.error = MountError::make(ErrorKind::Unavailable, QStringLiteral("Host files cannot be registered without an active host session."));
        result.status.state = MountState::Unavailable;
        result.status.message = result.error.message;
        return result;
    }

    MacFileProviderDomainRequest domainRequest;
    domainRequest.identifier = domainIdentifier(request);
    domainRequest.displayName = request.hostName.isEmpty()
            ? QStringLiteral("Moonlight Host Files")
            : QStringLiteral("Moonlight Host Files - %1").arg(request.hostName);

    MacFileProviderDomainResult domainResult = MacFileProviderBridge::registerDomain(domainRequest);
    if (!domainResult.ok) {
        result.error = MountError::make(domainResult.unsupported ? ErrorKind::Unsupported : ErrorKind::Internal,
                                        domainResult.message.isEmpty() ? unavailableMessage() : domainResult.message);
        result.status.state = domainResult.unsupported ? MountState::Unavailable : MountState::Error;
        result.status.message = result.error.message;
        if (!domainResult.diagnostics.isEmpty()) {
            result.diagnostics.append(domainResult.diagnostics);
        }
        return result;
    }

    MountId id;
    id.value = domainRequest.identifier;

    MountStatus status;
    status.state = MountState::Mounted;
    status.displayPath = domainResult.displayPath.isEmpty()
            ? displayPathForDomain(domainRequest.displayName)
            : domainResult.displayPath;
    status.message = domainResult.message.isEmpty()
            ? QStringLiteral("Host files are available in Finder.")
            : domainResult.message;

    DomainState state;
    state.status = status;
    m_Domains.insert(id.value, state);

    result.id = id;
    result.status = status;
    if (!domainResult.diagnostics.isEmpty()) {
        result.diagnostics.append(domainResult.diagnostics);
    }

    return result;
}

MountError MacFileProviderMountProvider::reveal(const MountId& id)
{
    MountStatus current = status(id);
    if (current.state != MountState::Mounted || current.displayPath.isEmpty()) {
        return MountError::make(ErrorKind::NotFound, unavailableMessage());
    }

    MacFileProviderDomainResult domainResult = MacFileProviderBridge::revealDomain(id.value, current.displayPath);
    if (!domainResult.ok && !domainResult.unsupported) {
        return MountError::make(ErrorKind::Internal,
                                domainResult.message.isEmpty() ? QStringLiteral("Could not open the macOS File Provider location.") : domainResult.message);
    }

    bool opened = false;
#if defined(Q_OS_MACOS)
    opened = QProcess::startDetached(QStringLiteral("/usr/bin/open"), { current.displayPath });
#endif
    if (!opened) {
        opened = QDesktopServices::openUrl(QUrl::fromLocalFile(current.displayPath));
    }
    return opened ? MountError::none()
                  : MountError::make(ErrorKind::Internal, QStringLiteral("Could not open the macOS File Provider location."));
}

void MacFileProviderMountProvider::unmount(const MountId& id)
{
    MacFileProviderBridge::unregisterDomain(id.value);
    m_Domains.remove(id.value);
}

QString MacFileProviderMountProvider::domainIdentifier(const MountRequest& request)
{
    return QStringLiteral("moonlight.%1.%2")
            .arg(sanitizeDomainPart(request.hostUuid, QStringLiteral("host")),
                 sanitizeDomainPart(request.sessionId, QStringLiteral("session")));
}

QString MacFileProviderMountProvider::displayPathForDomain(const QString& domainId)
{
    return QDir(QDir::homePath()).filePath(QStringLiteral("Library/CloudStorage/%1").arg(domainId));
}

QString MacFileProviderMountProvider::unavailableMessage()
{
    return QStringLiteral("Moonlight's macOS File Provider integration is not available in this build yet.");
}

} // namespace FileMapping
