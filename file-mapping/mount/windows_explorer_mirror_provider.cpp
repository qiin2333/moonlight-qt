#include "windows_explorer_mirror_provider.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QUrl>

namespace FileMapping {

namespace {
QString revealMarkerPath(const QString& rootPath)
{
    return QDir(rootPath).filePath(QStringLiteral("README.txt"));
}

QString explorerPathArgument(const QString& path)
{
    return QDir::toNativeSeparators(path);
}
} // namespace

WindowsExplorerMirrorProvider::WindowsExplorerMirrorProvider()
{
}

MountProviderKind WindowsExplorerMirrorProvider::kind() const
{
    return MountProviderKind::WindowsWinFsp;
}

QString WindowsExplorerMirrorProvider::displayName() const
{
    return QStringLiteral("Windows Explorer mirror");
}

MountResult WindowsExplorerMirrorProvider::mount(const MountRequest& request)
{
    MountResult result = MacOSFinderMirrorProvider::mount(request);
    if (result.ok() && result.status.state == MountState::Mounted) {
        result.status.message = result.status.message.isEmpty()
                ? QStringLiteral("Host files are ready in Explorer.")
                : result.status.message.replace(QStringLiteral("Host files are ready"),
                                                QStringLiteral("Host files are ready in Explorer"));
    }
    return result;
}

MountError WindowsExplorerMirrorProvider::reveal(const MountId& id)
{
    MountStatus current = status(id);
    if (current.state != MountState::Mounted || current.displayPath.isEmpty()) {
        return MountError::make(ErrorKind::NotFound, QStringLiteral("Host files are not mounted for this session."));
    }

    bool opened = false;
#if defined(Q_OS_WIN32) || defined(Q_OS_WIN)
    const QString markerPath = revealMarkerPath(current.displayPath);
    if (QFileInfo::exists(markerPath)) {
        opened = QProcess::startDetached(QStringLiteral("explorer.exe"),
                                         { QStringLiteral("/select,%1").arg(explorerPathArgument(markerPath)) });
    }
    if (!opened) {
        opened = QProcess::startDetached(QStringLiteral("explorer.exe"),
                                         { explorerPathArgument(current.displayPath) });
    }
#endif
    if (!opened) {
        opened = QDesktopServices::openUrl(QUrl::fromLocalFile(current.displayPath));
    }
    if (!opened) {
        return MountError::make(ErrorKind::Internal, QStringLiteral("Could not open host files in Explorer."));
    }

    return MountError::none();
}

} // namespace FileMapping
