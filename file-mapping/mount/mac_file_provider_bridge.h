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
    QString errorDomain;
    qint64 errorCode = 0;
    QString underlyingErrorDomain;
    qint64 underlyingErrorCode = 0;
};

class MacFileProviderBridge
{
public:
    static MacFileProviderDomainResult registerDomain(const MacFileProviderDomainRequest& request);
    static MacFileProviderDomainResult unregisterDomain(const QString& identifier);
    static MacFileProviderDomainResult revealDomain(const QString& identifier, const QString& fallbackPath);
};

} // namespace FileMapping
