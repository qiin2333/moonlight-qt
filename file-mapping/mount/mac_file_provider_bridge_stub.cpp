#include "mac_file_provider_bridge.h"

namespace FileMapping {

MacFileProviderDomainResult MacFileProviderBridge::registerDomain(const MacFileProviderDomainRequest& request)
{
    Q_UNUSED(request);

    MacFileProviderDomainResult result;
    result.unsupported = true;
    result.message = QStringLiteral("macOS File Provider is only available on macOS.");
    result.diagnostics = QStringLiteral("File Provider bridge stub is active.");
    return result;
}

MacFileProviderDomainResult MacFileProviderBridge::unregisterDomain(const QString& identifier)
{
    Q_UNUSED(identifier);
    MacFileProviderDomainResult result;
    result.ok = true;
    return result;
}

MacFileProviderDomainResult MacFileProviderBridge::revealDomain(const QString& identifier, const QString& fallbackPath)
{
    Q_UNUSED(identifier);
    Q_UNUSED(fallbackPath);

    MacFileProviderDomainResult result;
    result.unsupported = true;
    result.message = QStringLiteral("macOS File Provider is only available on macOS.");
    result.diagnostics = QStringLiteral("File Provider bridge stub is active.");
    return result;
}

} // namespace FileMapping
