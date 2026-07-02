#include "macfuse_mount_provider.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QLibrary>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>
#include <QStringList>
#include <QUrl>
#include <QWaitCondition>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <memory>
#include <thread>
#include <utility>

#if defined(Q_OS_MACOS)
#include "macfuse_runtime_abi.h"
#include <unistd.h>
#endif

namespace FileMapping {

namespace {
QString unsupportedMessage()
{
#if defined(Q_OS_MACOS)
    return QStringLiteral("macFUSE is not installed or its user-space library could not be loaded. Install macFUSE to mount Host Files as a Finder volume.");
#else
    return QStringLiteral("macFUSE mounting is only available on macOS.");
#endif
}

QString normalizeFusePath(const char* path)
{
    QString normalized = QString::fromUtf8(path == nullptr || path[0] == '\0' ? "/" : path);
    if (!normalized.startsWith(QLatin1Char('/'))) {
        normalized.prepend(QLatin1Char('/'));
    }
    normalized = QDir::cleanPath(normalized);
    return normalized == QStringLiteral(".") ? QStringLiteral("/") : normalized;
}

QString childPath(const QString& parentPath, const QString& childName)
{
    if (parentPath == QStringLiteral("/")) {
        return QStringLiteral("/") + childName;
    }
    return parentPath + QLatin1Char('/') + childName;
}

QString safeFuseName(const QString& name, const QString& fallback)
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
    return safe.left(120);
}

QString uniqueChildName(const QString& requestedName, QSet<QString>& usedNames)
{
    QString candidate = safeFuseName(requestedName, QStringLiteral("item"));
    if (!usedNames.contains(candidate)) {
        usedNames.insert(candidate);
        return candidate;
    }

    const QFileInfo info(candidate);
    const QString base = info.completeBaseName().isEmpty() ? candidate : info.completeBaseName();
    const QString suffix = info.suffix().isEmpty() ? QString() : QStringLiteral(".") + info.suffix();
    for (int i = 2; i < 10000; ++i) {
        candidate = QStringLiteral("%1 %2%3").arg(base).arg(i).arg(suffix);
        if (!usedNames.contains(candidate)) {
            usedNames.insert(candidate);
            return candidate;
        }
    }

    candidate = candidate + QStringLiteral(" copy");
    usedNames.insert(candidate);
    return candidate;
}

int errnoForError(const Error& error)
{
    switch (error.kind) {
    case ErrorKind::None:
        return 0;
    case ErrorKind::NotFound:
        return ENOENT;
    case ErrorKind::Unauthorized:
        return EACCES;
    case ErrorKind::ReadOnly:
        return EROFS;
    case ErrorKind::Timeout:
        return ETIMEDOUT;
    case ErrorKind::Unsupported:
        return ENOTSUP;
    case ErrorKind::Cancelled:
        return ECANCELED;
    case ErrorKind::Unavailable:
    case ErrorKind::Network:
    case ErrorKind::Internal:
        return EIO;
    }
    return EIO;
}

#if defined(Q_OS_MACOS)
class MacFuseRuntime
{
public:
    using FuseMainRealFn = int (*)(int, char**, const struct fuse_operations*, size_t, void*);
    using FuseGetContextFn = struct fuse_context* (*)();

    static MacFuseRuntime& instance()
    {
        static MacFuseRuntime runtime;
        return runtime;
    }

    bool load(QString& errorMessage)
    {
        QMutexLocker locker(&m_Lock);
        if (m_Loaded) {
            return true;
        }

        const QStringList candidates {
            QStringLiteral("/usr/local/lib/libfuse.dylib"),
            QStringLiteral("/usr/local/lib/libfuse.2.dylib"),
            QStringLiteral("/opt/homebrew/lib/libfuse.dylib"),
            QStringLiteral("/opt/homebrew/lib/libfuse.2.dylib"),
            QStringLiteral("/usr/local/lib/libosxfuse.dylib"),
            QStringLiteral("/usr/local/lib/libosxfuse.2.dylib"),
            QStringLiteral("/opt/homebrew/lib/libosxfuse.dylib"),
            QStringLiteral("/opt/homebrew/lib/libosxfuse.2.dylib"),
            QStringLiteral("/Library/Filesystems/macfuse.fs/Contents/Frameworks/libfuse.dylib"),
            QStringLiteral("/Library/Filesystems/macfuse.fs/Contents/Frameworks/libfuse.2.dylib"),
            QStringLiteral("/Library/Filesystems/osxfuse.fs/Contents/Frameworks/libfuse.dylib"),
            QStringLiteral("/Library/Filesystems/osxfuse.fs/Contents/Frameworks/libfuse.2.dylib"),
            QStringLiteral("libfuse.2.dylib"),
            QStringLiteral("libfuse.dylib"),
            QStringLiteral("libosxfuse.2.dylib"),
            QStringLiteral("libosxfuse.dylib"),
        };

        QStringList missing;
        QStringList failed;
        for (const QString& path : candidates) {
            const QFileInfo libraryInfo(path);
            if (libraryInfo.isAbsolute() && !libraryInfo.exists()) {
                missing.append(path);
                continue;
            }
            m_Library.setFileName(path);
            if (!m_Library.load()) {
                failed.append(path);
                continue;
            }

            auto fuseMainReal = reinterpret_cast<FuseMainRealFn>(m_Library.resolve("fuse_main_real"));
            auto fuseGetContext = reinterpret_cast<FuseGetContextFn>(m_Library.resolve("fuse_get_context"));
            if (fuseMainReal == nullptr || fuseGetContext == nullptr) {
                failed.append(QStringLiteral("%1: missing libfuse2 symbols").arg(path));
                m_Library.unload();
                continue;
            }

            m_FuseMainReal = fuseMainReal;
            m_FuseGetContext = fuseGetContext;
            m_LoadedPath = path;
            m_Loaded = true;
            return true;
        }

        m_ErrorMessage = unsupportedMessage();
        m_Diagnostics = failed.isEmpty()
                ? QStringLiteral("No macFUSE library was found in the standard locations.")
                : QStringLiteral("macFUSE libraries were found but could not be loaded: %1").arg(failed.join(QStringLiteral("; ")));
        if (!missing.isEmpty()) {
            m_Diagnostics += QStringLiteral(" Missing paths checked: %1").arg(missing.join(QStringLiteral(", ")));
        }
        errorMessage = m_ErrorMessage;
        return false;
    }

    int fuseMain(int argc, char** argv, const struct fuse_operations* operations, void* userData)
    {
        return m_FuseMainReal(argc, argv, operations, sizeof(*operations), userData);
    }

    struct fuse_context* context()
    {
        return m_FuseGetContext == nullptr ? nullptr : m_FuseGetContext();
    }

    QString loadedPath() const
    {
        return m_LoadedPath;
    }

    QString diagnostics()
    {
        QMutexLocker locker(&m_Lock);
        return m_Diagnostics;
    }

private:
    MacFuseRuntime() = default;

    QMutex m_Lock;
    bool m_Loaded = false;
    QString m_ErrorMessage;
    QString m_Diagnostics;
    QString m_LoadedPath;
    QLibrary m_Library;
    FuseMainRealFn m_FuseMainReal = nullptr;
    FuseGetContextFn m_FuseGetContext = nullptr;
};

class MacFuseSession
{
public:
    MacFuseSession(QString mountPath,
                   QString hostName,
                   QString sessionId,
                   std::shared_ptr<RemoteVfs> vfs)
        : m_MountPath(std::move(mountPath)),
          m_HostName(std::move(hostName)),
          m_SessionId(std::move(sessionId)),
          m_Vfs(std::move(vfs))
    {
        VfsItem root;
        root.id = VfsItemId::root();
        root.displayName = QStringLiteral("/");
        root.directory = true;
        m_ItemsByPath.insert(QStringLiteral("/"), root);
    }

    ~MacFuseSession()
    {
        requestUnmount();
        join();
    }

    QString mountPath() const
    {
        return m_MountPath;
    }

    bool start(QString& errorMessage)
    {
        if (!MacFuseRuntime::instance().load(errorMessage)) {
            return false;
        }

        m_Thread = std::thread([this]() {
            runFuse();
        });

        QMutexLocker locker(&m_StateLock);
        if (!m_StateChanged.wait(&m_StateLock, 5000)) {
            errorMessage = QStringLiteral("Timed out while starting macFUSE.");
            return false;
        }
        if (m_StartFailed) {
            errorMessage = m_ErrorMessage.isEmpty()
                    ? QStringLiteral("macFUSE could not mount the host files volume.")
                    : m_ErrorMessage;
            return false;
        }
        return m_Mounted;
    }

    MountStatus status()
    {
        QMutexLocker locker(&m_StateLock);
        MountStatus status;
        status.displayPath = m_MountPath;
        status.state = m_Mounted ? MountState::Mounted : MountState::Unmounted;
        status.message = m_Mounted
                ? QStringLiteral("Host files are mounted in Finder.")
                : QStringLiteral("Host files are not mounted.");
        return status;
    }

    void requestUnmount()
    {
        if (m_UnmountRequested.exchange(true)) {
            return;
        }
        QProcess::execute(QStringLiteral("/sbin/umount"), { m_MountPath });
    }

    void join()
    {
        if (m_Thread.joinable()) {
            m_Thread.join();
        }
    }

    void markMounted()
    {
        QMutexLocker locker(&m_StateLock);
        m_Mounted = true;
        m_StateChanged.wakeAll();
    }

    void markStopped(int result)
    {
        QMutexLocker locker(&m_StateLock);
        if (!m_Mounted && result != 0) {
            m_StartFailed = true;
            m_ErrorMessage = QStringLiteral("macFUSE exited with status %1.").arg(result);
        }
        m_Mounted = false;
        m_StateChanged.wakeAll();
    }

    int getattr(const char* path, struct stat* stbuf)
    {
        if (stbuf == nullptr) {
            return -EINVAL;
        }
        memset(stbuf, 0, sizeof(struct stat));

        VfsItem item;
        const int resolved = resolvePath(normalizeFusePath(path), item);
        if (resolved != 0) {
            return resolved;
        }

        stbuf->st_uid = getuid();
        stbuf->st_gid = getgid();
        stbuf->st_nlink = item.directory ? 2 : 1;
        stbuf->st_mode = item.directory ? (S_IFDIR | 0555) : (S_IFREG | 0444);
        stbuf->st_size = item.directory ? 0 : static_cast<off_t>(item.size);
        const time_t modified = item.modifiedAt.isValid()
                ? static_cast<time_t>(item.modifiedAt.toSecsSinceEpoch())
                : time(nullptr);
        stbuf->st_atime = modified;
        stbuf->st_mtime = modified;
        stbuf->st_ctime = modified;
        return 0;
    }

    int readdir(const char* path, void* buf, fuse_fill_dir_t filler, off_t, struct fuse_file_info*)
    {
        if (buf == nullptr || filler == nullptr) {
            return -EINVAL;
        }

        VfsItem item;
        const QString parentPath = normalizeFusePath(path);
        int resolved = resolvePath(parentPath, item);
        if (resolved != 0) {
            return resolved;
        }
        if (!item.directory) {
            return -ENOTDIR;
        }

        resolved = ensureChildren(parentPath, item);
        if (resolved != 0) {
            return resolved;
        }

        filler(buf, ".", nullptr, 0);
        filler(buf, "..", nullptr, 0);

        QList<QString> names;
        {
            QMutexLocker locker(&m_DataLock);
            names = m_ChildrenByPath.value(parentPath);
        }
        for (const QString& name : names) {
            filler(buf, name.toUtf8().constData(), nullptr, 0);
        }
        return 0;
    }

    int open(const char* path, struct fuse_file_info* fi)
    {
        if (fi == nullptr) {
            return -EINVAL;
        }

        VfsItem item;
        const int resolved = resolvePath(normalizeFusePath(path), item);
        if (resolved != 0) {
            return resolved;
        }
        if (item.directory) {
            return -EISDIR;
        }

        OpenResult openResult = m_Vfs->open(item.id);
        if (!openResult.ok()) {
            return -errnoForError(openResult.error);
        }

        QMutexLocker locker(&m_DataLock);
        const quint64 handleId = ++m_NextHandleId;
        m_OpenHandles.insert(handleId, openResult.handle);
        fi->fh = handleId;
        return 0;
    }

    int read(const char*, char* buf, size_t size, off_t offset, struct fuse_file_info* fi)
    {
        if (buf == nullptr || fi == nullptr) {
            return -EINVAL;
        }
        if (offset < 0) {
            return -EINVAL;
        }

        ReadHandle handle;
        {
            QMutexLocker locker(&m_DataLock);
            if (!m_OpenHandles.contains(fi->fh)) {
                return -EBADF;
            }
            handle = m_OpenHandles.value(fi->fh);
        }

        const quint32 readSize = static_cast<quint32>(qMin<quint64>(static_cast<quint64>(size), 1024ULL * 1024ULL));
        ReadResult result = m_Vfs->read(handle, static_cast<quint64>(offset), readSize);
        if (!result.ok()) {
            return -errnoForError(result.error);
        }
        if (!result.data.isEmpty()) {
            memcpy(buf, result.data.constData(), static_cast<size_t>(result.data.size()));
        }
        return result.data.size();
    }

    int release(const char*, struct fuse_file_info* fi)
    {
        if (fi == nullptr) {
            return -EINVAL;
        }

        ReadHandle handle;
        bool found = false;
        {
            QMutexLocker locker(&m_DataLock);
            auto it = m_OpenHandles.find(fi->fh);
            if (it != m_OpenHandles.end()) {
                handle = it.value();
                m_OpenHandles.erase(it);
                found = true;
            }
        }
        if (found) {
            m_Vfs->close(handle);
        }
        return 0;
    }

private:
    int resolvePath(const QString& path, VfsItem& item)
    {
        {
            QMutexLocker locker(&m_DataLock);
            auto it = m_ItemsByPath.find(path);
            if (it != m_ItemsByPath.end()) {
                item = it.value();
                return 0;
            }
        }

        const int slash = path.lastIndexOf(QLatin1Char('/'));
        const QString parentPath = slash <= 0 ? QStringLiteral("/") : path.left(slash);
        VfsItem parent;
        int resolved = resolvePath(parentPath, parent);
        if (resolved != 0) {
            return resolved;
        }
        if (!parent.directory) {
            return -ENOTDIR;
        }

        resolved = ensureChildren(parentPath, parent);
        if (resolved != 0) {
            return resolved;
        }

        QMutexLocker locker(&m_DataLock);
        auto it = m_ItemsByPath.find(path);
        if (it == m_ItemsByPath.end()) {
            return -ENOENT;
        }
        item = it.value();
        return 0;
    }

    int ensureChildren(const QString& parentPath, const VfsItem& parent)
    {
        {
            QMutexLocker locker(&m_DataLock);
            if (m_LoadedChildren.contains(parentPath)) {
                return 0;
            }
        }

        ChildrenResult result = m_Vfs->children(parent.id);
        if (!result.ok()) {
            return -errnoForError(result.error);
        }

        QSet<QString> usedNames;
        QList<QString> childNames;
        QHash<QString, VfsItem> childItems;
        for (const VfsItem& child : result.items) {
            const QString name = uniqueChildName(child.displayName, usedNames);
            childNames.append(name);
            childItems.insert(childPath(parentPath, name), child);
        }

        QMutexLocker locker(&m_DataLock);
        if (!m_LoadedChildren.contains(parentPath)) {
            for (auto it = childItems.constBegin(); it != childItems.constEnd(); ++it) {
                m_ItemsByPath.insert(it.key(), it.value());
            }
            m_ChildrenByPath.insert(parentPath, childNames);
            m_LoadedChildren.insert(parentPath);
        }
        return 0;
    }

    void runFuse()
    {
        struct fuse_operations operations;
        memset(&operations, 0, sizeof(operations));
        operations.getattr = &MacFuseSession::getattrCallback;
        operations.readdir = &MacFuseSession::readdirCallback;
        operations.open = &MacFuseSession::openCallback;
        operations.read = &MacFuseSession::readCallback;
        operations.release = &MacFuseSession::releaseCallback;
        operations.init = &MacFuseSession::initCallback;

        const QString volumeName = m_HostName.isEmpty()
                ? QStringLiteral("Moonlight Host Files")
                : QStringLiteral("Moonlight Host Files - %1").arg(m_HostName.left(40));

        QVector<QByteArray> args;
        args << QByteArray("moonlight-macfuse")
             << QByteArray("-f")
             << QByteArray("-s")
             << QByteArray("-oro")
             << QByteArray("-olocal")
             << QByteArray("-ovolname=") + volumeName.toUtf8()
             << m_MountPath.toUtf8();

        QVector<char*> argv;
        argv.reserve(args.size());
        for (QByteArray& arg : args) {
            argv.append(arg.data());
        }

        const int result = MacFuseRuntime::instance().fuseMain(static_cast<int>(argv.size()), argv.data(), &operations, this);
        markStopped(result);
        QDir().rmdir(m_MountPath);
    }

    static MacFuseSession* session()
    {
        struct fuse_context* context = MacFuseRuntime::instance().context();
        return context == nullptr ? nullptr : static_cast<MacFuseSession*>(context->private_data);
    }

    static void* initCallback(struct fuse_conn_info*)
    {
        MacFuseSession* current = session();
        if (current != nullptr) {
            current->markMounted();
        }
        return current;
    }

    static int getattrCallback(const char* path, struct stat* stbuf)
    {
        MacFuseSession* current = session();
        return current == nullptr ? -EIO : current->getattr(path, stbuf);
    }

    static int readdirCallback(const char* path, void* buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info* fi)
    {
        MacFuseSession* current = session();
        return current == nullptr ? -EIO : current->readdir(path, buf, filler, offset, fi);
    }

    static int openCallback(const char* path, struct fuse_file_info* fi)
    {
        MacFuseSession* current = session();
        return current == nullptr ? -EIO : current->open(path, fi);
    }

    static int readCallback(const char* path, char* buf, size_t size, off_t offset, struct fuse_file_info* fi)
    {
        MacFuseSession* current = session();
        return current == nullptr ? -EIO : current->read(path, buf, size, offset, fi);
    }

    static int releaseCallback(const char* path, struct fuse_file_info* fi)
    {
        MacFuseSession* current = session();
        return current == nullptr ? -EIO : current->release(path, fi);
    }

    QString m_MountPath;
    QString m_HostName;
    QString m_SessionId;
    std::shared_ptr<RemoteVfs> m_Vfs;
    std::thread m_Thread;
    std::atomic_bool m_UnmountRequested { false };

    QMutex m_StateLock;
    QWaitCondition m_StateChanged;
    bool m_Mounted = false;
    bool m_StartFailed = false;
    QString m_ErrorMessage;

    QMutex m_DataLock;
    QHash<QString, VfsItem> m_ItemsByPath;
    QHash<QString, QList<QString>> m_ChildrenByPath;
    QSet<QString> m_LoadedChildren;
    QHash<quint64, ReadHandle> m_OpenHandles;
    quint64 m_NextHandleId = 0;
};

QMutex g_MountsLock;
QHash<QString, std::shared_ptr<MacFuseSession>> g_Mounts;
#endif
} // namespace

MacFuseMountProvider::MacFuseMountProvider()
{
}

MacFuseMountProvider::~MacFuseMountProvider() = default;

MountProviderKind MacFuseMountProvider::kind() const
{
    return MountProviderKind::MacFuse;
}

QString MacFuseMountProvider::displayName() const
{
    return QStringLiteral("macFUSE Finder volume");
}

MountStatus MacFuseMountProvider::status(const MountId& id)
{
#if defined(Q_OS_MACOS)
    QMutexLocker locker(&g_MountsLock);
    auto it = g_Mounts.find(id.value);
    if (it != g_Mounts.end()) {
        return it.value()->status();
    }
#endif
    MountStatus status = m_KnownMounts.value(id.value);
    if (status.displayPath.isEmpty()) {
        status.state = MountState::Unmounted;
    }
    return status;
}

MountResult MacFuseMountProvider::mount(const MountRequest& request)
{
    MountResult result;
#if defined(Q_OS_MACOS)
    if (!request.vfs) {
        result.error = MountError::make(ErrorKind::Unavailable, QStringLiteral("Host files are unavailable."));
        result.status.state = MountState::Unavailable;
        result.status.message = result.error.message;
        return result;
    }

    const QString mountPath = mountPathForRequest(request);
    QDir mountDir(mountPath);
    if (mountDir.exists()) {
        QProcess::execute(QStringLiteral("/sbin/umount"), { mountPath });
        mountDir.removeRecursively();
    }
    if (!QDir().mkpath(mountPath)) {
        result.error = MountError::make(ErrorKind::Internal, QStringLiteral("Could not create macFUSE mount point."));
        result.status.state = MountState::Error;
        result.status.message = result.error.message;
        return result;
    }

    auto session = std::make_shared<MacFuseSession>(mountPath, request.hostName, request.sessionId, request.vfs);
    QString errorMessage;
    if (!session->start(errorMessage)) {
        session->requestUnmount();
        session->join();
        QDir(mountPath).removeRecursively();
        result.error = MountError::make(ErrorKind::Unsupported, errorMessage.isEmpty() ? unsupportedMessage() : errorMessage);
        result.status.state = MountState::Unavailable;
        result.status.message = result.error.message;
        const QString diagnostics = MacFuseRuntime::instance().diagnostics();
        if (!diagnostics.isEmpty()) {
            result.diagnostics.append(diagnostics);
        }
        return result;
    }

    {
        QMutexLocker locker(&g_MountsLock);
        g_Mounts.insert(mountPath, session);
    }

    MountStatus status;
    status.state = MountState::Mounted;
    status.displayPath = mountPath;
    status.message = QStringLiteral("Host files are mounted in Finder.");
    result.diagnostics.append(QStringLiteral("macFUSE loaded from %1").arg(MacFuseRuntime::instance().loadedPath()));

    MountId id;
    id.value = mountPath;
    m_KnownMounts.insert(id.value, status);

    result.id = id;
    result.status = status;
    return result;
#else
    result.error = MountError::make(ErrorKind::Unsupported, unsupportedMessage());
    result.status.state = MountState::Unavailable;
    result.status.message = result.error.message;
    result.providerName = displayName();
    result.diagnostics.append(QStringLiteral("macFUSE support is only compiled on macOS"));
    Q_UNUSED(request);
    return result;
#endif
}

MountError MacFuseMountProvider::reveal(const MountId& id)
{
    MountStatus current = status(id);
    if (current.state != MountState::Mounted || current.displayPath.isEmpty()) {
        return MountError::make(ErrorKind::NotFound, QStringLiteral("Host files are not mounted for this session."));
    }

    bool opened = false;
#if defined(Q_OS_MACOS)
    opened = QProcess::startDetached(QStringLiteral("/usr/bin/open"), { current.displayPath });
#endif
    if (!opened) {
        opened = QDesktopServices::openUrl(QUrl::fromLocalFile(current.displayPath));
    }
    if (!opened) {
        return MountError::make(ErrorKind::Internal, QStringLiteral("Could not open host files in Finder."));
    }
    return MountError::none();
}

void MacFuseMountProvider::unmount(const MountId& id)
{
#if defined(Q_OS_MACOS)
    std::shared_ptr<MacFuseSession> session;
    {
        QMutexLocker locker(&g_MountsLock);
        auto it = g_Mounts.find(id.value);
        if (it != g_Mounts.end()) {
            session = it.value();
            g_Mounts.erase(it);
        }
    }
    if (session) {
        session->requestUnmount();
        session->join();
    }
#endif
    m_KnownMounts.remove(id.value);
    if (!id.value.isEmpty()) {
        QDir(id.value).removeRecursively();
    }
}

QString MacFuseMountProvider::safeName(const QString& name, const QString& fallback)
{
    return safeFuseName(name, fallback);
}

QString MacFuseMountProvider::mountBasePath()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    if (base.isEmpty()) {
        base = QDir::homePath();
    }
    if (base.isEmpty()) {
        base = QDir::tempPath();
    }
    return QDir(QDir(base).filePath(QStringLiteral("Moonlight Host Files"))).filePath(QStringLiteral("Mounted"));
}

QString MacFuseMountProvider::mountPathForRequest(const MountRequest& request)
{
    const QString host = safeName(request.hostName.isEmpty() ? request.hostUuid : request.hostName,
                                  QStringLiteral("host"));
    const QString session = safeName(request.sessionId, QStringLiteral("session"));
    return QDir(mountBasePath()).filePath(QStringLiteral("%1-%2").arg(host, session));
}

} // namespace FileMapping
