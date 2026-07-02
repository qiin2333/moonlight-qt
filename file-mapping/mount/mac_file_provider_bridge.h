#pragma once

#include <QString>

namespace FileMapping {

struct MacFileProviderDomainRequest {
    QString identifier;
    QString displayName;
};

struct MacFileProviderDomainResult {
    bool ok = false;
    bool unsupported = false;
    QString displayPath;
    QString message;
    QString diagnostics;
};

class MacFileProviderBridge
{
public:
    static MacFileProviderDomainResult registerDomain(const MacFileProviderDomainRequest& request);
    static MacFileProviderDomainResult unregisterDomain(const QString& identifier);
    static MacFileProviderDomainResult revealDomain(const QString& identifier, const QString& fallbackPath);
};

} // namespace FileMapping
