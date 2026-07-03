#include "mount_provider_factory.h"

#include "mac_file_provider_mount_provider.h"
#include "macfuse_mount_provider.h"
#include "macos_finder_mirror_provider.h"
#include "unavailable_mount_provider.h"
#include "windows_explorer_mirror_provider.h"

#include <QByteArray>
#include <QtGlobal>

#include <memory>

namespace FileMapping {
namespace {
QString nativeProviderName(MountProviderKind kind)
{
    switch (kind) {
    case MountProviderKind::MacFileProvider:
        return QStringLiteral("macOS File Provider");
    case MountProviderKind::WindowsWinFsp:
        return QStringLiteral("Windows Explorer integration");
    case MountProviderKind::LinuxFuse:
        return QStringLiteral("Linux FUSE integration");
    case MountProviderKind::MobileDocumentProvider:
        return QStringLiteral("Mobile document provider");
    case MountProviderKind::MacFuse:
        return QStringLiteral("macFUSE integration");
    case MountProviderKind::Fallback:
        return QStringLiteral("Moonlight file browser");
    }
    return QStringLiteral("Host files integration");
}

QString nativeUnavailableMessage(MountProviderKind kind)
{
    return QStringLiteral("%1 is not available in this build yet.").arg(nativeProviderName(kind));
}

bool envFlagEnabled(const char* name)
{
    const QByteArray value = qgetenv(name).trimmed().toLower();
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

bool experimentalMacFileProviderEnabled()
{
    return envFlagEnabled("MOONLIGHT_EXPERIMENTAL_MAC_FILE_PROVIDER");
}

bool experimentalMacFuseEnabled()
{
    return envFlagEnabled("MOONLIGHT_EXPERIMENTAL_MACFUSE");
}
} // namespace

MountProviderKind platformNativeMountProviderKind()
{
#if defined(Q_OS_MACOS)
    return MountProviderKind::MacFileProvider;
#elif defined(Q_OS_WIN)
    return MountProviderKind::WindowsWinFsp;
#elif defined(Q_OS_LINUX)
    return MountProviderKind::LinuxFuse;
#elif defined(Q_OS_IOS) || defined(Q_OS_ANDROID)
    return MountProviderKind::MobileDocumentProvider;
#else
    return MountProviderKind::Fallback;
#endif
}

QList<MountProviderPtr> createDefaultMountProviders()
{
    QList<MountProviderPtr> providers;
#if defined(Q_OS_MACOS)
    if (experimentalMacFileProviderEnabled()) {
        providers.append(std::make_shared<MacFileProviderMountProvider>());
    }
    if (experimentalMacFuseEnabled()) {
        providers.append(std::make_shared<MacFuseMountProvider>());
    }
    providers.append(std::make_shared<MacOSFinderMirrorProvider>());
#elif defined(Q_OS_WIN32) || defined(Q_OS_WIN)
    providers.append(std::make_shared<WindowsExplorerMirrorProvider>());
#else
    const MountProviderKind nativeKind = platformNativeMountProviderKind();
    providers.append(std::make_shared<UnavailableMountProvider>(
                         nativeKind,
                         nativeProviderName(nativeKind),
                         nativeUnavailableMessage(nativeKind)));
#endif
    providers.append(std::make_shared<UnavailableMountProvider>(
                         MountProviderKind::Fallback,
                         nativeProviderName(MountProviderKind::Fallback),
                         QStringLiteral("Moonlight's built-in host files browser is not available yet.")));
    return providers;
}

} // namespace FileMapping
