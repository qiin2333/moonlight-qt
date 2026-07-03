#include "mac_file_provider_bridge.h"

#include <QDir>
#include <QStringList>

#import <FileProvider/FileProvider.h>
#import <Foundation/Foundation.h>

namespace FileMapping {
namespace {
NSString* toNSString(const QString& value)
{
    return [NSString stringWithUTF8String:value.toUtf8().constData()];
}

QString fromNSString(NSString* value)
{
    return value == nil ? QString() : QString::fromNSString(value);
}

MacFileProviderDomainResult unsupportedResult()
{
    MacFileProviderDomainResult result;
    result.unsupported = true;
    result.message = QStringLiteral("macOS File Provider requires macOS 11 or later.");
    result.diagnostics = QStringLiteral("NSFileProviderManager is not available at runtime.");
    return result;
}

MacFileProviderDomainResult resultFromError(NSError* error, const QString& fallbackMessage)
{
    MacFileProviderDomainResult result;
    if (error == nil) {
        result.ok = true;
        return result;
    }

    result.message = fallbackMessage;
    result.errorDomain = fromNSString(error.domain);
    result.errorCode = static_cast<qint64>(error.code);
    QStringList diagnostics;
    diagnostics.append(QStringLiteral("domain=%1").arg(result.errorDomain));
    diagnostics.append(QStringLiteral("code=%1").arg(result.errorCode));

    const QString description = fromNSString(error.localizedDescription);
    if (!description.isEmpty()) {
        diagnostics.append(QStringLiteral("description=\"%1\"").arg(description));
    }

    const QString failureReason = fromNSString(error.localizedFailureReason);
    if (!failureReason.isEmpty()) {
        diagnostics.append(QStringLiteral("failure_reason=\"%1\"").arg(failureReason));
    }

    const QString recoverySuggestion = fromNSString(error.localizedRecoverySuggestion);
    if (!recoverySuggestion.isEmpty()) {
        diagnostics.append(QStringLiteral("recovery_suggestion=\"%1\"").arg(recoverySuggestion));
    }

    id underlyingObject = error.userInfo[NSUnderlyingErrorKey];
    if ([underlyingObject isKindOfClass:[NSError class]]) {
        NSError* underlyingError = (NSError*)underlyingObject;
        result.underlyingErrorDomain = fromNSString(underlyingError.domain);
        result.underlyingErrorCode = static_cast<qint64>(underlyingError.code);
        diagnostics.append(QStringLiteral("underlying_domain=%1").arg(result.underlyingErrorDomain));
        diagnostics.append(QStringLiteral("underlying_code=%1").arg(result.underlyingErrorCode));
        const QString underlyingDescription = fromNSString(underlyingError.localizedDescription);
        if (!underlyingDescription.isEmpty()) {
            diagnostics.append(QStringLiteral("underlying_description=\"%1\"").arg(underlyingDescription));
        }
    }

    result.diagnostics = diagnostics.join(QStringLiteral(" "));
    return result;
}
} // namespace

MacFileProviderDomainResult MacFileProviderBridge::registerDomain(const MacFileProviderDomainRequest& request)
{
    if (@available(macOS 11.0, *)) {
        __block MacFileProviderDomainResult result;
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);

        NSFileProviderDomain* domain = [[NSFileProviderDomain alloc]
                initWithIdentifier:toNSString(request.identifier)
                        displayName:toNSString(request.displayName)];

        [NSFileProviderManager addDomain:domain completionHandler:^(NSError* error) {
            result = resultFromError(error, QStringLiteral("Could not register the Moonlight File Provider domain."));
            if (result.ok) {
                result.displayPath = QDir(QDir::homePath()).filePath(QStringLiteral("Library/CloudStorage/%1").arg(request.displayName));
                result.message = QStringLiteral("Host files are available in Finder.");
                result.diagnostics = QStringLiteral("Registered File Provider domain %1").arg(request.identifier);
            }
            dispatch_semaphore_signal(sem);
        }];

        dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
        return result;
    }

    return unsupportedResult();
}

MacFileProviderDomainResult MacFileProviderBridge::unregisterDomain(const QString& identifier)
{
    if (@available(macOS 11.0, *)) {
        __block MacFileProviderDomainResult result;
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);

        NSFileProviderDomain* domain = [[NSFileProviderDomain alloc]
                initWithIdentifier:toNSString(identifier)
                        displayName:toNSString(identifier)];

        [NSFileProviderManager removeDomain:domain completionHandler:^(NSError* error) {
            result = resultFromError(error, QStringLiteral("Could not remove the Moonlight File Provider domain."));
            if (result.ok) {
                result.diagnostics = QStringLiteral("Removed File Provider domain %1").arg(identifier);
            }
            dispatch_semaphore_signal(sem);
        }];

        dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
        return result;
    }

    return unsupportedResult();
}

MacFileProviderDomainResult MacFileProviderBridge::revealDomain(const QString& identifier, const QString& fallbackPath)
{
    Q_UNUSED(identifier);

    MacFileProviderDomainResult result;
    result.displayPath = fallbackPath;
    result.ok = !fallbackPath.isEmpty();
    result.message = result.ok
            ? QStringLiteral("Opening the Moonlight File Provider domain in Finder.")
            : QStringLiteral("Moonlight File Provider domain is not mounted.");
    result.diagnostics = QStringLiteral("File Provider reveal uses the current display path.");
    return result;
}

} // namespace FileMapping
