#include "macos_finder_mirror_provider.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QUrl>

namespace FileMapping {

namespace {
QString errorMessage(const Error& error)
{
    return error.message.isEmpty() ? QStringLiteral("Host files could not be copied.") : error.message;
}

QString skippedFileText(const QString& reason)
{
    return QStringLiteral("This host file was not copied into the Finder snapshot.\n\nReason: %1\n\nTry opening a smaller folder or increase the Moonlight file mapping mirror limits for testing.\n").arg(reason);
}
} // namespace

MacOSFinderMirrorProvider::MacOSFinderMirrorProvider()
{
}

MountProviderKind MacOSFinderMirrorProvider::kind() const
{
    return MountProviderKind::MacFileProvider;
}

QString MacOSFinderMirrorProvider::displayName() const
{
    return QStringLiteral("macOS Finder mirror");
}

MountStatus MacOSFinderMirrorProvider::status(const MountId& id)
{
    const auto it = m_Mounts.find(id.value);
    if (it == m_Mounts.end()) {
        MountStatus status;
        status.state = MountState::Unmounted;
        return status;
    }

    if (!QFileInfo::exists(it->rootPath)) {
        MountStatus status;
        status.state = MountState::Unmounted;
        return status;
    }

    return it->status;
}

MountResult MacOSFinderMirrorProvider::mount(const MountRequest& request)
{
    MountResult result;
    if (!request.vfs) {
        result.error = MountError::make(ErrorKind::Unavailable, QStringLiteral("Host files are unavailable."));
        result.status.state = MountState::Unavailable;
        result.status.message = result.error.message;
        return result;
    }

    const QString rootPath = cacheRootPath(request);
    QDir root(rootPath);
    if (root.exists() && !root.removeRecursively()) {
        result.error = MountError::make(ErrorKind::Internal, QStringLiteral("Could not refresh the local Finder snapshot."));
        result.status.state = MountState::Error;
        result.status.message = result.error.message;
        return result;
    }

    if (!QDir().mkpath(rootPath)) {
        result.error = MountError::make(ErrorKind::Internal, QStringLiteral("Could not create the local Finder snapshot."));
        result.status.state = MountState::Error;
        result.status.message = result.error.message;
        return result;
    }

    writeTextFile(rootPath + QStringLiteral("/README.txt"),
                  QStringLiteral("Moonlight Host Files\n\nThis folder is a read-only snapshot of folders shared by the host for the current streaming session. Large files or very deep folders may be skipped.\n"));

    MirrorCounters counters;
    const MirrorLimits limits = limitsFromEnvironment();
    VfsItem rootItem;
    rootItem.id = VfsItemId::root();
    rootItem.directory = true;

    const Error error = mirrorChildren(*request.vfs, rootItem, rootPath, 0, limits, counters);
    if (!error.ok()) {
        result.error = error;
        result.status.state = MountState::Error;
        result.status.message = errorMessage(error);
        QDir(rootPath).removeRecursively();
        return result;
    }

    if (!counters.warnings.isEmpty()) {
        writeTextFile(rootPath + QStringLiteral("/Moonlight skipped files.txt"),
                      QStringLiteral("Some host files were not copied into this Finder snapshot:\n\n") +
                      counters.warnings.join(QLatin1Char('\n')) +
                      QStringLiteral("\n"));
    }

    makeReadOnly(rootPath + QStringLiteral("/README.txt"), false);

    MountStatus status;
    status.state = MountState::Mounted;
    status.displayPath = rootPath;
    status.message = counters.warnings.isEmpty()
            ? QStringLiteral("Host files are ready in Finder.")
            : QStringLiteral("Host files are ready in Finder. Some large or deep items were skipped.");

    MountId id;
    id.value = rootPath;
    m_Mounts.insert(id.value, { status, rootPath });

    result.id = id;
    result.status = status;
    return result;
}

MountError MacOSFinderMirrorProvider::reveal(const MountId& id)
{
    MountStatus current = status(id);
    if (current.state != MountState::Mounted || current.displayPath.isEmpty()) {
        return MountError::make(ErrorKind::NotFound, QStringLiteral("Host files are not mounted for this session."));
    }

    bool opened = false;
#if defined(Q_OS_MACOS)
    opened = QProcess::startDetached(QStringLiteral("/usr/bin/open"),
                                     { QStringLiteral("-a"),
                                       QStringLiteral("Finder"),
                                       current.displayPath });
#endif
    if (!opened) {
        opened = QDesktopServices::openUrl(QUrl::fromLocalFile(current.displayPath));
    }
    if (!opened) {
        return MountError::make(ErrorKind::Internal, QStringLiteral("Could not open host files in Finder."));
    }

    return MountError::none();
}

void MacOSFinderMirrorProvider::unmount(const MountId& id)
{
    auto it = m_Mounts.find(id.value);
    if (it == m_Mounts.end()) {
        return;
    }

    if (!it->rootPath.isEmpty()) {
        QDir(it->rootPath).removeRecursively();
    }
    m_Mounts.erase(it);
}

QString MacOSFinderMirrorProvider::safeName(const QString& name, const QString& fallback)
{
    QString safe = name.trimmed();
    if (safe.isEmpty()) {
        safe = fallback;
    }

    static const QString invalidChars = QStringLiteral("\\/:*?\"<>|");
    for (int i = 0; i < safe.size(); ++i) {
        if (safe.at(i).unicode() < 32 || invalidChars.contains(safe.at(i))) {
            safe[i] = QLatin1Char('_');
        }
    }

    while (safe.endsWith(QLatin1Char('.')) || safe.endsWith(QLatin1Char(' '))) {
        safe.chop(1);
    }

    if (safe.isEmpty() || safe == QStringLiteral(".") || safe == QStringLiteral("..")) {
        safe = fallback;
    }
    return safe.left(160);
}

QString MacOSFinderMirrorProvider::uniqueChildPath(const QString& parentPath, const QString& requestedName)
{
    const QString safe = safeName(requestedName, QStringLiteral("item"));
    QString candidate = QDir(parentPath).filePath(safe);
    if (!QFileInfo::exists(candidate)) {
        return candidate;
    }

    const QFileInfo info(safe);
    const QString base = info.completeBaseName().isEmpty() ? safe : info.completeBaseName();
    const QString suffix = info.suffix().isEmpty() ? QString() : QStringLiteral(".") + info.suffix();
    for (int i = 2; i < 10000; ++i) {
        candidate = QDir(parentPath).filePath(QStringLiteral("%1 %2%3").arg(base).arg(i).arg(suffix));
        if (!QFileInfo::exists(candidate)) {
            return candidate;
        }
    }

    return QDir(parentPath).filePath(safe + QStringLiteral(" copy"));
}

QString MacOSFinderMirrorProvider::cacheRootPath(const MountRequest& request)
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (!base.isEmpty()) {
        base = QDir(base).filePath(QStringLiteral("Moonlight Host Files"));
    }
    if (base.isEmpty()) {
        base = QDir::homePath() + QStringLiteral("/Moonlight Host Files");
    }
    if (base.isEmpty()) {
        base = QDir::tempPath() + QStringLiteral("/Moonlight Host Files");
    }

    const QString host = safeName(request.hostName.isEmpty() ? request.hostUuid : request.hostName,
                                  QStringLiteral("host"));
    const QString session = safeName(request.sessionId, QStringLiteral("session"));
    return QDir(base).filePath(QStringLiteral("%1-%2").arg(host, session));
}

MacOSFinderMirrorProvider::MirrorLimits MacOSFinderMirrorProvider::limitsFromEnvironment()
{
    MirrorLimits limits;
    bool ok = false;
    int value = qEnvironmentVariableIntValue("MOONLIGHT_FILE_MAPPING_MIRROR_MAX_DEPTH", &ok);
    if (ok) {
        limits.maxDepth = qBound(1, value, 32);
    }
    value = qEnvironmentVariableIntValue("MOONLIGHT_FILE_MAPPING_MIRROR_MAX_FILES", &ok);
    if (ok) {
        limits.maxFiles = qBound(1, value, 100000);
    }
    value = qEnvironmentVariableIntValue("MOONLIGHT_FILE_MAPPING_MIRROR_MAX_MB", &ok);
    if (ok) {
        limits.maxBytes = static_cast<quint64>(qBound(1, value, 1024 * 10)) * 1024ULL * 1024ULL;
    }
    value = qEnvironmentVariableIntValue("MOONLIGHT_FILE_MAPPING_MIRROR_MAX_FILE_MB", &ok);
    if (ok) {
        limits.maxFileBytes = static_cast<quint64>(qBound(1, value, 1024 * 10)) * 1024ULL * 1024ULL;
    }
    value = qEnvironmentVariableIntValue("MOONLIGHT_FILE_MAPPING_MIRROR_CHUNK_KB", &ok);
    if (ok) {
        limits.chunkBytes = static_cast<quint32>(qBound(16, value, 4096)) * 1024U;
    }
    return limits;
}

bool MacOSFinderMirrorProvider::writeTextFile(const QString& path, const QString& text)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return false;
    }
    file.write(text.toUtf8());
    file.close();
    return true;
}

void MacOSFinderMirrorProvider::makeReadOnly(const QString& path, bool directory)
{
    QFileDevice::Permissions permissions = QFileDevice::ReadOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther;
    if (directory) {
        permissions |= QFileDevice::ExeOwner | QFileDevice::ExeGroup | QFileDevice::ExeOther;
    }
    QFile::setPermissions(path, permissions);
}

Error MacOSFinderMirrorProvider::mirrorChildren(RemoteVfs& vfs,
                                                const VfsItem& parent,
                                                const QString& localPath,
                                                int depth,
                                                const MirrorLimits& limits,
                                                MirrorCounters& counters) const
{
    if (depth > limits.maxDepth) {
        counters.warnings.append(QStringLiteral("%1: skipped because the folder is too deep").arg(localPath));
        writeTextFile(QDir(localPath).filePath(QStringLiteral("Moonlight folder depth limit.txt")),
                      skippedFileText(QStringLiteral("folder depth limit reached")));
        return Error::none();
    }

    ChildrenResult children = vfs.children(parent.id);
    if (!children.ok()) {
        return children.error;
    }

    for (const VfsItem& item : children.items) {
        if (item.directory) {
            const QString childPath = uniqueChildPath(localPath, item.displayName);
            if (!QDir().mkpath(childPath)) {
                return Error::make(ErrorKind::Internal, QStringLiteral("Could not create local folder: %1").arg(childPath));
            }

            Error error = mirrorChildren(vfs, item, childPath, depth + 1, limits, counters);
            if (!error.ok()) {
                return error;
            }
        }
        else {
            const QString childPath = uniqueChildPath(localPath, item.displayName);
            Error error = mirrorFile(vfs, item, childPath, limits, counters);
            if (!error.ok()) {
                return error;
            }
        }
    }

    return Error::none();
}

Error MacOSFinderMirrorProvider::mirrorFile(RemoteVfs& vfs,
                                            const VfsItem& item,
                                            const QString& localPath,
                                            const MirrorLimits& limits,
                                            MirrorCounters& counters) const
{
    if (counters.files >= limits.maxFiles) {
        counters.warnings.append(QStringLiteral("%1: skipped because the file count limit was reached").arg(localPath));
        writeTextFile(localPath + QStringLiteral(".moonlight-skipped.txt"),
                      skippedFileText(QStringLiteral("file count limit reached")));
        return Error::none();
    }
    if (item.size > limits.maxFileBytes) {
        counters.warnings.append(QStringLiteral("%1: skipped because the file is too large").arg(localPath));
        writeTextFile(localPath + QStringLiteral(".moonlight-skipped.txt"),
                      skippedFileText(QStringLiteral("file exceeds the per-file mirror limit")));
        return Error::none();
    }
    if (counters.bytes + item.size > limits.maxBytes) {
        counters.warnings.append(QStringLiteral("%1: skipped because the snapshot size limit was reached").arg(localPath));
        writeTextFile(localPath + QStringLiteral(".moonlight-skipped.txt"),
                      skippedFileText(QStringLiteral("snapshot size limit reached")));
        return Error::none();
    }

    OpenResult open = vfs.open(item.id);
    if (!open.ok()) {
        return open.error;
    }

    QFile file(localPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        vfs.close(open.handle);
        return Error::make(ErrorKind::Internal, QStringLiteral("Could not create local file: %1").arg(localPath));
    }

    quint64 offset = 0;
    Error result = Error::none();
    for (;;) {
        if (offset >= limits.maxFileBytes || counters.bytes >= limits.maxBytes) {
            counters.warnings.append(QStringLiteral("%1: truncated because the snapshot size limit was reached").arg(localPath));
            break;
        }

        const quint64 remainingTotal = limits.maxBytes - counters.bytes;
        const quint64 remainingFile = limits.maxFileBytes - offset;
        const quint32 readLength = static_cast<quint32>(qMin<quint64>(limits.chunkBytes, qMin(remainingTotal, remainingFile)));
        if (readLength == 0) {
            break;
        }

        ReadResult read = vfs.read(open.handle, offset, readLength);
        if (!read.ok()) {
            result = read.error;
            break;
        }
        if (read.data.isEmpty()) {
            break;
        }
        if (file.write(read.data) != read.data.size()) {
            result = Error::make(ErrorKind::Internal, QStringLiteral("Could not write local file: %1").arg(localPath));
            break;
        }

        const quint64 bytesRead = static_cast<quint64>(read.data.size());
        offset += bytesRead;
        counters.bytes += bytesRead;
        if (bytesRead < readLength) {
            break;
        }
    }

    file.close();
    vfs.close(open.handle);
    if (!result.ok()) {
        QFile::remove(localPath);
        return result;
    }

    ++counters.files;
    makeReadOnly(localPath, false);
    return Error::none();
}

} // namespace FileMapping
