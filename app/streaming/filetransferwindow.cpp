#include "filetransferwindow.h"

#include <algorithm>

#include <QDir>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileInfo>
#include <QFileInfoList>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHash>
#include <QImage>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QStyleHints>
#include <QTemporaryFile>
#include <QUrl>
#include <QUuid>
#include <QWheelEvent>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

#include "filemappingprotocoladapter.h"

namespace {
constexpr int kTransferTimeoutMs = 10000;
constexpr quint32 kTransferChunkBytes = 256U * 1024U;
constexpr int kMargin = 18;
constexpr int kHeaderHeight = 42;
constexpr int kPathHeight = 38;
constexpr int kRowsTop = 92;
constexpr int kStatusHeight = 48;
constexpr int kCenterWidth = 116;
constexpr int kColumnHeaderHeight = 30;
constexpr int kRowHeight = 36;
constexpr int kIconSize = 22;

QImage fileIcon(const QString& path,
                const QString& name,
                bool directory,
                bool drive,
                bool remote)
{
    static QHash<QString, QImage> cache;
    QString cacheKey;
    if (drive) {
        cacheKey = QStringLiteral("drive:%1").arg(name.left(1).toUpper());
    }
    else if (directory) {
        cacheKey = QStringLiteral("folder");
    }
    else {
        const QString suffix = QFileInfo(name).suffix().toLower();
        if (!remote &&
            (suffix == QStringLiteral("exe") ||
             suffix == QStringLiteral("ico") ||
             suffix == QStringLiteral("lnk"))) {
            cacheKey = QStringLiteral("path:%1").arg(path.toLower());
        }
        else {
            cacheKey = QStringLiteral("file:%1").arg(suffix);
        }
    }
    const auto cached = cache.constFind(cacheKey);
    if (cached != cache.cend()) {
        return cached.value();
    }

#ifdef Q_OS_WIN
    if (drive && remote) {
        SHSTOCKICONINFO stockInfo {};
        stockInfo.cbSize = sizeof(stockInfo);
        if (SUCCEEDED(SHGetStockIconInfo(
                    SIID_DRIVEFIXED,
                    SHGSI_ICON | SHGSI_SMALLICON,
                    &stockInfo)) &&
            stockInfo.hIcon != nullptr) {
            QImage image = QImage::fromHICON(stockInfo.hIcon);
            DestroyIcon(stockInfo.hIcon);
            if (!image.isNull()) {
                image = image.scaled(kIconSize,
                                     kIconSize,
                                     Qt::KeepAspectRatio,
                                     Qt::SmoothTransformation);
                cache.insert(cacheKey, image);
                return image;
            }
        }
    }

    QString lookupPath = path;
    if (drive && remote) {
        lookupPath = name.left(2) + QStringLiteral("\\");
    }
    else if (remote) {
        lookupPath = name;
    }
    lookupPath = QDir::toNativeSeparators(lookupPath);

    SHFILEINFOW info {};
    DWORD attributes = directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    UINT flags = SHGFI_ICON | SHGFI_SMALLICON;
    if (remote) {
        flags |= SHGFI_USEFILEATTRIBUTES;
    }
    const DWORD_PTR result = SHGetFileInfoW(
            reinterpret_cast<LPCWSTR>(lookupPath.utf16()),
            attributes,
            &info,
            sizeof(info),
            flags);
    if (result != 0 && info.hIcon != nullptr) {
        QImage image = QImage::fromHICON(info.hIcon);
        DestroyIcon(info.hIcon);
        if (!image.isNull()) {
            image = image.scaled(kIconSize,
                                 kIconSize,
                                 Qt::KeepAspectRatio,
                                 Qt::SmoothTransformation);
            cache.insert(cacheKey, image);
            return image;
        }
    }
#else
    Q_UNUSED(path);
    Q_UNUSED(name);
    Q_UNUSED(remote);
#endif

    QImage fallback(kIconSize, kIconSize, QImage::Format_ARGB32_Premultiplied);
    fallback.fill(Qt::transparent);
    QPainter painter(&fallback);
    painter.setRenderHint(QPainter::Antialiasing);
    if (directory || drive) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(drive ? QColor(95, 176, 255) : QColor(255, 194, 71));
        painter.drawRoundedRect(QRectF(1, 6, 20, 14), 2, 2);
        painter.drawRoundedRect(QRectF(3, 3, 9, 6), 2, 2);
    }
    else {
        QPainterPath page;
        page.moveTo(4, 1);
        page.lineTo(14, 1);
        page.lineTo(20, 7);
        page.lineTo(20, 21);
        page.lineTo(4, 21);
        page.closeSubpath();
        painter.setPen(QPen(QColor(145, 156, 170), 1));
        painter.setBrush(QColor(239, 243, 247));
        painter.drawPath(page);
    }
    cache.insert(cacheKey, fallback);
    return fallback;
}

QString remoteJoin(const QString& parent, const QString& name)
{
    return parent.isEmpty() ? name : parent + QLatin1Char('/') + name;
}

QString remoteParent(QString path)
{
    while (path.endsWith(QLatin1Char('/'))) {
        path.chop(1);
    }
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    return slash < 0 ? QString() : path.left(slash);
}

QString displaySize(quint64 bytes)
{
    if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        return QStringLiteral("%1 GB").arg(static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0), 0, 'f', 1);
    }
    if (bytes >= 1024ULL * 1024ULL) {
        return QStringLiteral("%1 MB").arg(static_cast<double>(bytes) / (1024.0 * 1024.0), 0, 'f', 1);
    }
    if (bytes >= 1024ULL) {
        return QStringLiteral("%1 KB").arg(static_cast<double>(bytes) / 1024.0, 0, 'f', 1);
    }
    return QStringLiteral("%1 B").arg(bytes);
}

QString errorMessage(const FileMapping::Error& error)
{
    if (error.message.contains(QStringLiteral("closed"), Qt::CaseInsensitive)) {
        return QStringLiteral("远程主机关闭了文件传输连接");
    }

    switch (error.kind) {
    case FileMapping::ErrorKind::None:
        return error.message.isEmpty()
                ? QStringLiteral("未知的文件传输错误")
                : error.message;
    case FileMapping::ErrorKind::Unavailable:
        return QStringLiteral("远程主机的文件传输服务不可用");
    case FileMapping::ErrorKind::Unauthorized:
        return QStringLiteral("远程主机拒绝了文件传输授权");
    case FileMapping::ErrorKind::Timeout:
        return QStringLiteral("连接远程主机超时");
    case FileMapping::ErrorKind::Network:
        return QStringLiteral("文件传输网络连接异常");
    case FileMapping::ErrorKind::NotFound:
        return QStringLiteral("找不到远程文件或文件夹");
    case FileMapping::ErrorKind::ReadOnly:
        return QStringLiteral("远程磁盘为只读，无法上传");
    case FileMapping::ErrorKind::Cancelled:
        return QStringLiteral("传输已取消");
    case FileMapping::ErrorKind::Unsupported:
        return QStringLiteral("远程主机不支持此文件操作");
    case FileMapping::ErrorKind::Internal:
        return error.message.isEmpty()
                ? QStringLiteral("文件传输发生内部错误")
                : QStringLiteral("文件传输失败：%1").arg(error.message);
    }

    return QStringLiteral("未知的文件传输错误");
}

bool connectionError(const FileMapping::Error& error)
{
    return error.kind == FileMapping::ErrorKind::Network ||
           error.kind == FileMapping::ErrorKind::Timeout ||
           error.kind == FileMapping::ErrorKind::Unauthorized ||
           error.kind == FileMapping::ErrorKind::Unavailable;
}
} // namespace

FileTransferWorker::FileTransferWorker(NvComputer computer)
    : m_Computer(std::move(computer))
{
}

FileTransferWorker::~FileTransferWorker() = default;

void FileTransferWorker::requestCancel()
{
    m_Cancelled.store(true);
}

bool FileTransferWorker::ensureConnected(QString& error)
{
    if (m_Client) {
        return true;
    }

    m_Cancelled.store(false);
    auto client = std::make_unique<FileMappingProtocolAdapter>(m_Computer);
    const FileMapping::Capability capability = client->fetchCapability(kTransferTimeoutMs);
    if (!capability.error.ok()) {
        error = errorMessage(capability.error);
        return false;
    }
    if (!capability.enabled || !capability.listening || capability.sessionToken.isEmpty()) {
        error = QStringLiteral("远程主机的文件传输尚未就绪，请保持串流连接后重试");
        return false;
    }

    const FileMapping::Error connectError = client->connectSession(capability, kTransferTimeoutMs);
    if (!connectError.ok()) {
        error = errorMessage(connectError);
        return false;
    }
    m_Client = std::move(client);
    return true;
}

void FileTransferWorker::initialize()
{
    QString error;
    if (!ensureConnected(error)) {
        emit remoteReady({}, error);
        return;
    }

    QVariantList mappings;
    for (const FileMapping::RemoteMapping& mapping : m_Client->mappings()) {
        QVariantMap item;
        item.insert(QStringLiteral("id"), mapping.id);
        item.insert(QStringLiteral("name"), mapping.displayName);
        item.insert(QStringLiteral("writable"),
                    mapping.mode == QStringLiteral("readwrite") &&
                    mapping.capabilities.contains(QStringLiteral("write")));
        mappings.append(item);
    }
    emit remoteReady(mappings, QString());
}

void FileTransferWorker::browseRemote(const QString& mappingId, const QString& path)
{
    QString error;
    if (!ensureConnected(error)) {
        emit remoteListed(mappingId, path, {}, error);
        return;
    }

    const FileMapping::ListResult result = m_Client->list(mappingId, path, kTransferTimeoutMs);
    if (!result.ok()) {
        if (connectionError(result.error)) {
            m_Client.reset();
        }
        emit remoteListed(mappingId, path, {}, errorMessage(result.error));
        return;
    }

    QVariantList entries;
    for (const FileMapping::RemoteEntry& entry : result.entries) {
        QVariantMap item;
        item.insert(QStringLiteral("name"), entry.displayName);
        item.insert(QStringLiteral("path"), entry.path);
        item.insert(QStringLiteral("directory"), entry.directory);
        item.insert(QStringLiteral("size"), QVariant::fromValue(entry.size));
        entries.append(item);
    }
    emit remoteListed(mappingId, path, entries, QString());
}

bool FileTransferWorker::uploadFile(const QString& localPath,
                                    const QString& mappingId,
                                    const QString& remotePath,
                                    QString& error)
{
    QFile file(localPath);
    if (!file.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("无法打开本地文件：%1").arg(localPath);
        return false;
    }

    const quint64 total = static_cast<quint64>(file.size());
    const QString uploadId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    quint64 offset = 0;
    bool first = true;

    do {
        if (m_Cancelled.load()) {
            error = QStringLiteral("传输已取消");
            return false;
        }

        const QByteArray chunk = file.read(kTransferChunkBytes);
        if (chunk.isNull()) {
            error = QStringLiteral("读取本地文件失败：%1").arg(localPath);
            return false;
        }
        const bool complete = offset + static_cast<quint64>(chunk.size()) >= total;
        const FileMapping::WriteResult result = m_Client->write(mappingId,
                                                               remotePath,
                                                               uploadId,
                                                               offset,
                                                               total,
                                                               chunk,
                                                               first,
                                                               complete,
                                                               kTransferTimeoutMs);
        if (!result.ok()) {
            if (connectionError(result.error)) {
                m_Client.reset();
            }
            error = errorMessage(result.error);
            return false;
        }

        offset += static_cast<quint64>(chunk.size());
        first = false;
        emit transferProgress(
                QStringLiteral("正在上传 %1").arg(QFileInfo(localPath).fileName()),
                offset,
                total);
    } while (first || offset < total);

    return true;
}

bool FileTransferWorker::uploadItem(const QString& localPath,
                                    const QString& mappingId,
                                    const QString& remotePath,
                                    QString& error)
{
    if (m_Cancelled.load()) {
        error = QStringLiteral("传输已取消");
        return false;
    }

    const QFileInfo info(localPath);
    if (info.isSymLink()) {
        error = QStringLiteral("不支持传输符号链接：%1").arg(localPath);
        return false;
    }
    if (info.isFile()) {
        return uploadFile(localPath, mappingId, remotePath, error);
    }
    if (!info.isDir()) {
        error = QStringLiteral("不支持的本地项目：%1").arg(localPath);
        return false;
    }

    const FileMapping::Error mkdirError = m_Client->mkdir(mappingId, remotePath, kTransferTimeoutMs);
    if (!mkdirError.ok()) {
        if (connectionError(mkdirError)) {
            m_Client.reset();
        }
        error = errorMessage(mkdirError);
        return false;
    }

    const QFileInfoList children = QDir(localPath).entryInfoList(
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
            QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo& child : children) {
        if (!uploadItem(child.absoluteFilePath(),
                        mappingId,
                        remoteJoin(remotePath, child.fileName()),
                        error)) {
            return false;
        }
    }
    return true;
}

void FileTransferWorker::upload(const QString& localPath,
                                const QString& mappingId,
                                const QString& remoteDirectory)
{
    QString error;
    if (!ensureConnected(error)) {
        emit transferFinished(false, error);
        return;
    }

    m_Cancelled.store(false);
    const QFileInfo source(localPath);
    if (!source.exists()) {
        emit transferFinished(false, QStringLiteral("所选本地文件或文件夹已经不存在"));
        return;
    }

    const QString remotePath = remoteJoin(remoteDirectory, source.fileName());
    // Check the parent directory instead of probing the destination with stat.
    // Early full-disk Sunshine builds incorrectly closed the WebSocket after a
    // valid stat/not_found response, so the following write saw a dead
    // connection. Listing preserves the no-overwrite guarantee and remains
    // compatible with those hosts.
    const FileMapping::ListResult destinationDirectory =
            m_Client->list(mappingId, remoteDirectory, kTransferTimeoutMs);
    if (!destinationDirectory.ok()) {
        if (connectionError(destinationDirectory.error)) {
            m_Client.reset();
        }
        emit transferFinished(false, errorMessage(destinationDirectory.error));
        return;
    }
    const bool destinationExists = std::any_of(
            destinationDirectory.entries.cbegin(),
            destinationDirectory.entries.cend(),
            [&source](const FileMapping::RemoteEntry& entry) {
                return entry.displayName.compare(source.fileName(), Qt::CaseInsensitive) == 0;
            });
    if (destinationExists) {
        emit transferFinished(
                false,
                QStringLiteral("远程主机中已存在“%1”，不会覆盖已有文件")
                        .arg(source.fileName()));
        return;
    }

    if (uploadItem(localPath, mappingId, remotePath, error)) {
        emit transferFinished(
                true,
                QStringLiteral("已将“%1”上传到远程主机").arg(source.fileName()));
    }
    else {
        emit transferFinished(false, error);
    }
}

bool FileTransferWorker::downloadFile(const QString& mappingId,
                                      const QString& remotePath,
                                      const QString& localPath,
                                      QString& error)
{
    const FileMapping::StatResult stat = m_Client->stat(mappingId, remotePath, kTransferTimeoutMs);
    if (!stat.ok()) {
        if (connectionError(stat.error)) {
            m_Client.reset();
        }
        error = errorMessage(stat.error);
        return false;
    }
    if (!stat.stat.exists || stat.stat.directory) {
        error = QStringLiteral("远程文件已经不可用：%1").arg(remotePath);
        return false;
    }

    const QFileInfo targetInfo(localPath);
    QDir().mkpath(targetInfo.absolutePath());
    QTemporaryFile temporary(QDir(targetInfo.absolutePath())
                                    .filePath(QStringLiteral(".moonlight-download-XXXXXX.part")));
    if (!temporary.open()) {
        error = QStringLiteral("无法在 %1 中创建下载临时文件")
                        .arg(targetInfo.absolutePath());
        return false;
    }

    quint64 offset = 0;
    const quint64 total = stat.stat.size;
    while (offset < total) {
        if (m_Cancelled.load()) {
            error = QStringLiteral("传输已取消");
            return false;
        }

        const quint32 length = static_cast<quint32>(
                std::min<quint64>(kTransferChunkBytes, total - offset));
        const FileMapping::ReadResult result = m_Client->read(mappingId,
                                                             remotePath,
                                                             offset,
                                                             length,
                                                             kTransferTimeoutMs);
        if (!result.ok()) {
            if (connectionError(result.error)) {
                m_Client.reset();
            }
            error = errorMessage(result.error);
            return false;
        }
        if (result.data.isEmpty()) {
            error = QStringLiteral("远程主机返回了不完整的文件数据");
            return false;
        }
        if (temporary.write(result.data) != result.data.size()) {
            error = QStringLiteral("写入本地文件失败：%1").arg(localPath);
            return false;
        }
        offset += static_cast<quint64>(result.data.size());
        emit transferProgress(
                QStringLiteral("正在下载 %1").arg(QFileInfo(remotePath).fileName()),
                offset,
                total);
    }

    temporary.flush();
    temporary.close();
    if (QFileInfo::exists(localPath)) {
        error = QStringLiteral("本机已存在“%1”，不会覆盖已有文件")
                        .arg(targetInfo.fileName());
        return false;
    }

    // On Windows, QTemporaryFile retains ownership of its native handle after
    // close(). Renaming it through a second QFile instance fails with a sharing
    // violation. Rename through the owning object so Qt can safely commit the
    // completed download, including paths containing Chinese characters.
    if (!temporary.rename(localPath)) {
        const QString renameError = temporary.errorString();
        error = QStringLiteral("无法保存下载文件：%1（%2）")
                        .arg(localPath, renameError);
        return false;
    }
    // QTemporaryFile keeps auto-remove enabled after a successful rename.
    // Disable it only now, so failures still clean up the temporary file.
    temporary.setAutoRemove(false);
    return true;
}

bool FileTransferWorker::downloadItem(const QString& mappingId,
                                      const QString& remotePath,
                                      bool directory,
                                      const QString& localPath,
                                      QString& error)
{
    if (m_Cancelled.load()) {
        error = QStringLiteral("传输已取消");
        return false;
    }
    if (!directory) {
        return downloadFile(mappingId, remotePath, localPath, error);
    }

    if (!QDir().mkdir(localPath)) {
        error = QStringLiteral("无法创建本地文件夹：%1").arg(localPath);
        return false;
    }

    const FileMapping::ListResult result = m_Client->list(mappingId, remotePath, kTransferTimeoutMs);
    if (!result.ok()) {
        if (connectionError(result.error)) {
            m_Client.reset();
        }
        error = errorMessage(result.error);
        return false;
    }
    for (const FileMapping::RemoteEntry& child : result.entries) {
        if (!downloadItem(mappingId,
                          child.path,
                          child.directory,
                          QDir(localPath).filePath(child.displayName),
                          error)) {
            return false;
        }
    }
    return true;
}

void FileTransferWorker::download(const QString& mappingId,
                                  const QString& remotePath,
                                  bool directory,
                                  const QString& localDirectory)
{
    QString error;
    if (!ensureConnected(error)) {
        emit transferFinished(false, error);
        return;
    }

    m_Cancelled.store(false);
    const QString name = QFileInfo(remotePath).fileName();
    const QString localPath = QDir(localDirectory).filePath(name);
    if (QFileInfo::exists(localPath)) {
        emit transferFinished(
                false,
                QStringLiteral("本机已存在“%1”，不会覆盖已有文件").arg(name));
        return;
    }

    if (downloadItem(mappingId, remotePath, directory, localPath, error)) {
        emit transferFinished(
                true,
                QStringLiteral("已将“%1”下载到本机").arg(name));
    }
    else {
        if (directory) {
            QDir(localPath).removeRecursively();
        }
        emit transferFinished(false, error);
    }
}

FileTransferWindow::FileTransferWindow(NvComputer computer)
{
    setTitle(QStringLiteral("Moonlight 文件传输"));
    setFlags(Qt::Window | Qt::WindowTitleHint | Qt::WindowSystemMenuHint |
             Qt::WindowStaysOnTopHint |
             Qt::WindowMinMaxButtonsHint | Qt::WindowCloseButtonHint);
    resize(1080, 680);
    setMinimumSize(QSize(820, 520));

    if (QScreen* screen = QGuiApplication::primaryScreen()) {
        const QRect area = screen->availableGeometry();
        setPosition(area.center() - QPoint(width() / 2, height() / 2));
    }

    loadLocalRoot();
    setStatus(QStringLiteral("正在连接远程主机磁盘…"));

    m_Worker = new FileTransferWorker(std::move(computer));
    m_Worker->moveToThread(&m_WorkerThread);
    connect(&m_WorkerThread, &QThread::finished, m_Worker, &QObject::deleteLater);
    connect(this, &FileTransferWindow::initializeWorker,
            m_Worker, &FileTransferWorker::initialize);
    connect(this, &FileTransferWindow::requestRemoteList,
            m_Worker, &FileTransferWorker::browseRemote);
    connect(this, &FileTransferWindow::requestUpload,
            m_Worker, &FileTransferWorker::upload);
    connect(this, &FileTransferWindow::requestDownload,
            m_Worker, &FileTransferWorker::download);
    connect(m_Worker, &FileTransferWorker::remoteReady,
            this, &FileTransferWindow::onRemoteReady);
    connect(m_Worker, &FileTransferWorker::remoteListed,
            this, &FileTransferWindow::onRemoteListed);
    connect(m_Worker, &FileTransferWorker::transferProgress,
            this, &FileTransferWindow::onTransferProgress);
    connect(m_Worker, &FileTransferWorker::transferFinished,
            this, &FileTransferWindow::onTransferFinished);
    m_WorkerThread.start();
    emit initializeWorker();
}

FileTransferWindow::~FileTransferWindow()
{
    if (m_Worker) {
        m_Worker->requestCancel();
    }
    m_WorkerThread.quit();
    m_WorkerThread.wait(15000);
}

void FileTransferWindow::showAndActivate()
{
    show();
    raise();
    requestActivate();
}

QRect FileTransferWindow::localPaneRect() const
{
    const int paneWidth = (width() - kMargin * 2 - kCenterWidth) / 2;
    return QRect(kMargin, kRowsTop, paneWidth, height() - kRowsTop - kStatusHeight);
}

QRect FileTransferWindow::remotePaneRect() const
{
    const QRect left = localPaneRect();
    return QRect(left.right() + 1 + kCenterWidth,
                 kRowsTop,
                 left.width(),
                 left.height());
}

QRect FileTransferWindow::localPathRect() const
{
    const QRect pane = localPaneRect();
    return QRect(pane.x(), kHeaderHeight, pane.width(), kPathHeight);
}

QRect FileTransferWindow::remotePathRect() const
{
    const QRect pane = remotePaneRect();
    return QRect(pane.x(), kHeaderHeight, pane.width(), kPathHeight);
}

QRect FileTransferWindow::uploadButtonRect() const
{
    const QRect left = localPaneRect();
    return QRect(left.right() + 13, 205, kCenterWidth - 26, 42);
}

QRect FileTransferWindow::downloadButtonRect() const
{
    const QRect left = localPaneRect();
    return QRect(left.right() + 13, 265, kCenterWidth - 26, 42);
}

QRect FileTransferWindow::refreshButtonRect() const
{
    const QRect left = localPaneRect();
    return QRect(left.right() + 13, 345, kCenterWidth - 26, 38);
}

int FileTransferWindow::visibleRowCount() const
{
    return std::max(
            1,
            (localPaneRect().height() - kColumnHeaderHeight) / kRowHeight);
}

QRect FileTransferWindow::rowRect(bool local, int visibleRow) const
{
    const QRect pane = local ? localPaneRect() : remotePaneRect();
    return QRect(pane.x() + 1,
                 pane.y() + kColumnHeaderHeight +
                         visibleRow * kRowHeight + 1,
                 pane.width() - 2,
                 kRowHeight);
}

int FileTransferWindow::rowAt(bool local, const QPoint& point) const
{
    const QRect pane = local ? localPaneRect() : remotePaneRect();
    if (!pane.contains(point)) {
        return -1;
    }
    if (point.y() < pane.y() + kColumnHeaderHeight) {
        return -1;
    }
    const int visible =
            (point.y() - pane.y() - kColumnHeaderHeight) / kRowHeight;
    const int offset = local ? m_LocalScroll : m_RemoteScroll;
    const int index = offset + visible;
    const int count = local ? m_LocalEntries.size() : m_RemoteEntries.size();
    return index >= 0 && index < count ? index : -1;
}

void FileTransferWindow::loadLocalRoot()
{
    m_LocalPath.clear();
    m_LocalEntries.clear();
    for (const QFileInfo& drive : QDir::drives()) {
        Entry entry;
        entry.name = QDir::toNativeSeparators(drive.absoluteFilePath());
        entry.path = drive.absoluteFilePath();
        entry.directory = true;
        entry.drive = true;
        entry.writable = drive.isWritable();
        entry.icon = fileIcon(
                entry.path, entry.name, entry.directory, entry.drive, false);
        m_LocalEntries.append(std::move(entry));
    }
    m_LocalSelection = -1;
    m_LocalScroll = 0;
    update();
}

void FileTransferWindow::browseLocal(const QString& path)
{
    QDir directory(path);
    if (!directory.exists()) {
        setStatus(QStringLiteral("本地文件夹不可用：%1").arg(path), true);
        return;
    }

    QVector<Entry> entries;
    const QFileInfoList infos = directory.entryInfoList(
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
            QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
    entries.reserve(infos.size());
    for (const QFileInfo& info : infos) {
        Entry entry;
        entry.name = info.fileName();
        entry.path = info.absoluteFilePath();
        entry.directory = info.isDir();
        entry.size = info.isFile() ? static_cast<quint64>(info.size()) : 0;
        entry.icon = fileIcon(
                entry.path, entry.name, entry.directory, false, false);
        entries.append(std::move(entry));
    }

    m_LocalPath = directory.absolutePath();
    m_LocalEntries = std::move(entries);
    m_LocalSelection = -1;
    m_LocalScroll = 0;
    update();
}

void FileTransferWindow::refreshLocal()
{
    if (m_LocalPath.isEmpty()) {
        loadLocalRoot();
    }
    else {
        browseLocal(m_LocalPath);
    }
}

void FileTransferWindow::refreshRemote()
{
    if (m_RemoteMappingId.isEmpty()) {
        if (m_RemoteRoots.isEmpty()) {
            m_Busy = true;
            setStatus(QStringLiteral("正在重新连接远程主机磁盘…"));
            emit initializeWorker();
            return;
        }
        m_RemoteEntries = m_RemoteRoots;
        m_RemoteSelection = -1;
        m_RemoteScroll = 0;
        update();
        return;
    }

    m_Busy = true;
    setStatus(QStringLiteral("正在读取远程文件夹…"));
    emit requestRemoteList(m_RemoteMappingId, m_RemotePath);
}

void FileTransferWindow::openLocalSelection()
{
    if (m_LocalSelection < 0 || m_LocalSelection >= m_LocalEntries.size()) {
        return;
    }
    const Entry& entry = m_LocalEntries.at(m_LocalSelection);
    if (entry.directory) {
        browseLocal(entry.path);
    }
}

void FileTransferWindow::openRemoteSelection()
{
    if (m_RemoteSelection < 0 || m_RemoteSelection >= m_RemoteEntries.size()) {
        return;
    }
    const Entry entry = m_RemoteEntries.at(m_RemoteSelection);
    if (!entry.directory) {
        return;
    }
    if (entry.drive) {
        m_RemoteMappingId = entry.mappingId;
        m_RemoteMappingName = entry.name;
        m_RemotePath.clear();
        m_RemoteWritable = entry.writable;
    }
    else {
        m_RemotePath = entry.path;
    }
    refreshRemote();
}

void FileTransferWindow::localUp()
{
    if (m_LocalPath.isEmpty()) {
        return;
    }
    QDir directory(m_LocalPath);
    if (directory.isRoot() || !directory.cdUp()) {
        loadLocalRoot();
    }
    else {
        browseLocal(directory.absolutePath());
    }
}

void FileTransferWindow::remoteUp()
{
    if (m_RemoteMappingId.isEmpty()) {
        return;
    }
    if (m_RemotePath.isEmpty()) {
        m_RemoteMappingId.clear();
        m_RemoteMappingName.clear();
        m_RemoteWritable = false;
    }
    else {
        m_RemotePath = remoteParent(m_RemotePath);
    }
    refreshRemote();
}

void FileTransferWindow::beginUpload()
{
    if (m_Busy) {
        return;
    }
    if (m_LocalSelection < 0 || m_LocalSelection >= m_LocalEntries.size()) {
        setStatus(QStringLiteral("请先选择本机文件或文件夹"), true);
        return;
    }
    if (m_RemoteMappingId.isEmpty()) {
        setStatus(QStringLiteral("请先打开远程磁盘并进入目标文件夹"), true);
        return;
    }
    if (!m_RemoteWritable) {
        setStatus(QStringLiteral("这个远程磁盘是只读的"), true);
        return;
    }

    const Entry entry = m_LocalEntries.at(m_LocalSelection);
    if (entry.drive) {
        setStatus(QStringLiteral("请打开本机磁盘后选择文件或文件夹，不能传输整个磁盘"), true);
        return;
    }

    m_Busy = true;
    m_ProgressVisible = true;
    m_ProgressPercent = 0;
    setStatus(QStringLiteral("正在准备上传：%1").arg(entry.name));
    emit requestUpload(entry.path, m_RemoteMappingId, m_RemotePath);
}

void FileTransferWindow::beginDownload()
{
    if (m_Busy) {
        return;
    }
    if (m_RemoteSelection < 0 || m_RemoteSelection >= m_RemoteEntries.size()) {
        setStatus(QStringLiteral("请先选择远程文件或文件夹"), true);
        return;
    }
    if (m_LocalPath.isEmpty()) {
        setStatus(QStringLiteral("请先打开本机磁盘并进入目标文件夹"), true);
        return;
    }

    const Entry entry = m_RemoteEntries.at(m_RemoteSelection);
    if (entry.drive) {
        setStatus(QStringLiteral("请打开远程磁盘后选择文件或文件夹，不能传输整个磁盘"), true);
        return;
    }

    m_Busy = true;
    m_ProgressVisible = true;
    m_ProgressPercent = 0;
    setStatus(QStringLiteral("正在准备下载：%1").arg(entry.name));
    emit requestDownload(m_RemoteMappingId, entry.path, entry.directory, m_LocalPath);
}

bool FileTransferWindow::resolveRemoteDropTarget(
        const QPoint& point,
        QString& mappingId,
        QString& remoteDirectory,
        QString& displayPath) const
{
    if (!remotePaneRect().contains(point) &&
        !remotePathRect().contains(point)) {
        return false;
    }

    const int targetIndex = rowAt(false, point);
    if (m_RemoteMappingId.isEmpty()) {
        if (targetIndex < 0 || targetIndex >= m_RemoteEntries.size()) {
            return false;
        }
        const Entry& drive = m_RemoteEntries.at(targetIndex);
        if (!drive.drive || !drive.writable) {
            return false;
        }
        mappingId = drive.mappingId;
        remoteDirectory.clear();
        displayPath = drive.name + QStringLiteral("\\");
        return true;
    }

    if (!m_RemoteWritable) {
        return false;
    }
    mappingId = m_RemoteMappingId;
    remoteDirectory = m_RemotePath;
    if (targetIndex >= 0 && targetIndex < m_RemoteEntries.size()) {
        const Entry& target = m_RemoteEntries.at(targetIndex);
        if (target.directory && !target.drive) {
            remoteDirectory = target.path;
        }
    }

    displayPath = m_RemoteMappingName;
    if (!remoteDirectory.isEmpty()) {
        displayPath += QStringLiteral("\\") +
                       QDir::toNativeSeparators(remoteDirectory);
    }
    else {
        displayPath += QStringLiteral("\\");
    }
    return true;
}

bool FileTransferWindow::resolveLocalDropTarget(
        const QPoint& point,
        QString& localDirectory) const
{
    if (!localPaneRect().contains(point) &&
        !localPathRect().contains(point)) {
        return false;
    }

    localDirectory = m_LocalPath;
    const int targetIndex = rowAt(true, point);
    if (targetIndex >= 0 && targetIndex < m_LocalEntries.size()) {
        const Entry& target = m_LocalEntries.at(targetIndex);
        if (target.directory) {
            localDirectory = target.path;
        }
    }
    return !localDirectory.isEmpty() &&
           QFileInfo(localDirectory).isDir();
}

void FileTransferWindow::updateDrag(const QPoint& point)
{
    m_DragPosition = point;
    m_DragTargetValid = false;
    m_DragHint.clear();

    if (m_DragSourceLocal) {
        QString mappingId;
        QString remoteDirectory;
        QString displayPath;
        m_DragTargetValid = resolveRemoteDropTarget(
                point, mappingId, remoteDirectory, displayPath);
        m_DragHint = m_DragTargetValid
                ? QStringLiteral("松开鼠标，上传到 %1").arg(displayPath)
                : QStringLiteral("请拖到右侧远程文件夹");
    }
    else {
        QString localDirectory;
        m_DragTargetValid = resolveLocalDropTarget(point, localDirectory);
        m_DragHint = m_DragTargetValid
                ? QStringLiteral("松开鼠标，下载到 %1")
                          .arg(QDir::toNativeSeparators(localDirectory))
                : QStringLiteral("请拖到左侧本机文件夹");
    }

    setCursor(m_DragTargetValid
              ? Qt::DragCopyCursor
              : Qt::ForbiddenCursor);
    update();
}

void FileTransferWindow::finishDrag(const QPoint& point)
{
    if (!m_DragActive || m_Busy || m_DragSourceIndex < 0) {
        return;
    }

    if (m_DragSourceLocal) {
        if (m_DragSourceIndex >= m_LocalEntries.size()) {
            return;
        }
        const Entry source = m_LocalEntries.at(m_DragSourceIndex);
        QString mappingId;
        QString remoteDirectory;
        QString displayPath;
        if (!resolveRemoteDropTarget(
                point, mappingId, remoteDirectory, displayPath)) {
            setStatus(QStringLiteral("上传已取消：请拖到右侧可写的远程文件夹"), true);
            return;
        }
        m_Busy = true;
        m_ProgressVisible = true;
        m_ProgressPercent = 0;
        setStatus(QStringLiteral("正在准备上传：%1").arg(source.name));
        emit requestUpload(source.path, mappingId, remoteDirectory);
        return;
    }

    if (m_DragSourceIndex >= m_RemoteEntries.size()) {
        return;
    }
    const Entry source = m_RemoteEntries.at(m_DragSourceIndex);
    QString localDirectory;
    if (!resolveLocalDropTarget(point, localDirectory)) {
        setStatus(QStringLiteral("下载已取消：请拖到左侧本机文件夹"), true);
        return;
    }
    m_Busy = true;
    m_ProgressVisible = true;
    m_ProgressPercent = 0;
    setStatus(QStringLiteral("正在准备下载：%1").arg(source.name));
    emit requestDownload(
            m_RemoteMappingId,
            source.path,
            source.directory,
            localDirectory);
}

void FileTransferWindow::resetDrag()
{
    m_DragActive = false;
    m_DragTargetValid = false;
    m_DragSourceIndex = -1;
    m_DragHint.clear();
    unsetCursor();
    update();
}

void FileTransferWindow::clampScrollOffsets()
{
    const int visible = visibleRowCount();
    const int localCount = static_cast<int>(m_LocalEntries.size());
    const int remoteCount = static_cast<int>(m_RemoteEntries.size());
    m_LocalScroll = std::clamp(m_LocalScroll, 0, std::max(0, localCount - visible));
    m_RemoteScroll = std::clamp(m_RemoteScroll, 0, std::max(0, remoteCount - visible));
}

void FileTransferWindow::setStatus(const QString& status, bool error)
{
    m_Status = status;
    m_StatusError = error;
    update();
}

void FileTransferWindow::onRemoteReady(const QVariantList& mappings, const QString& error)
{
    m_Busy = false;
    m_RemoteRoots.clear();
    if (!error.isEmpty()) {
        setStatus(QStringLiteral("无法连接远程主机磁盘：%1").arg(error), true);
        return;
    }

    for (const QVariant& value : mappings) {
        const QVariantMap item = value.toMap();
        Entry entry;
        entry.name = item.value(QStringLiteral("name")).toString();
        entry.path.clear();
        entry.mappingId = item.value(QStringLiteral("id")).toString();
        entry.directory = true;
        entry.drive = true;
        entry.writable = item.value(QStringLiteral("writable")).toBool();
        entry.icon = fileIcon(
                entry.name + QStringLiteral("\\"),
                entry.name,
                true,
                true,
                true);
        m_RemoteRoots.append(std::move(entry));
    }
    m_RemoteEntries = m_RemoteRoots;
    m_RemoteSelection = -1;
    m_RemoteScroll = 0;
    setStatus(m_RemoteRoots.isEmpty()
              ? QStringLiteral("远程主机没有可访问的磁盘")
              : QStringLiteral("连接成功，可点击按钮或把文件拖到另一侧进行传输"),
              m_RemoteRoots.isEmpty());
}

void FileTransferWindow::onRemoteListed(const QString& mappingId,
                                        const QString& path,
                                        const QVariantList& entries,
                                        const QString& error)
{
    if (mappingId != m_RemoteMappingId || path != m_RemotePath) {
        return;
    }

    m_Busy = false;
    if (!error.isEmpty()) {
        setStatus(QStringLiteral("无法打开远程文件夹：%1").arg(error), true);
        return;
    }

    QVector<Entry> converted;
    converted.reserve(entries.size());
    for (const QVariant& value : entries) {
        const QVariantMap item = value.toMap();
        Entry entry;
        entry.name = item.value(QStringLiteral("name")).toString();
        entry.path = item.value(QStringLiteral("path")).toString();
        entry.mappingId = mappingId;
        entry.directory = item.value(QStringLiteral("directory")).toBool();
        entry.size = item.value(QStringLiteral("size")).toULongLong();
        entry.icon = fileIcon(
                entry.path, entry.name, entry.directory, false, true);
        converted.append(std::move(entry));
    }
    m_RemoteEntries = std::move(converted);
    m_RemoteSelection = -1;
    m_RemoteScroll = 0;
    setStatus(QStringLiteral("远程文件夹已加载"));
}

void FileTransferWindow::onTransferProgress(const QString& message, quint64 completed, quint64 total)
{
    if (total == 0) {
        setStatus(message);
    }
    else {
        const int percent = static_cast<int>(std::min<quint64>(100, completed * 100 / total));
        m_ProgressVisible = true;
        m_ProgressPercent = percent;
        setStatus(QStringLiteral("%1 — %2%").arg(message).arg(percent));
    }
}

void FileTransferWindow::onTransferFinished(bool ok, const QString& message)
{
    m_Busy = false;
    m_ProgressVisible = false;
    m_ProgressPercent = 0;
    setStatus(message, !ok);
    if (ok) {
        refreshLocal();
        refreshRemote();
    }
}

bool FileTransferWindow::event(QEvent* event)
{
    if (event->type() == QEvent::DragEnter ||
        event->type() == QEvent::DragMove) {
        auto* dragEvent = static_cast<QDropEvent*>(event);
        QString mappingId;
        QString remoteDirectory;
        QString displayPath;
        const bool hasLocalItem =
                dragEvent->mimeData()->hasUrls() &&
                std::any_of(
                        dragEvent->mimeData()->urls().cbegin(),
                        dragEvent->mimeData()->urls().cend(),
                        [](const QUrl& url) {
                            return url.isLocalFile() &&
                                   QFileInfo::exists(url.toLocalFile());
                        });
        if (!m_Busy &&
            hasLocalItem &&
            resolveRemoteDropTarget(
                    dragEvent->position().toPoint(),
                    mappingId,
                    remoteDirectory,
                    displayPath)) {
            dragEvent->acceptProposedAction();
            return true;
        }
        dragEvent->ignore();
        return true;
    }

    if (event->type() == QEvent::DragLeave) {
        return true;
    }

    if (event->type() == QEvent::Drop) {
        auto* dropEvent = static_cast<QDropEvent*>(event);
        QString localPath;
        for (const QUrl& url : dropEvent->mimeData()->urls()) {
            if (url.isLocalFile() && QFileInfo::exists(url.toLocalFile())) {
                localPath = url.toLocalFile();
                break;
            }
        }

        QString mappingId;
        QString remoteDirectory;
        QString displayPath;
        if (!m_Busy &&
            !localPath.isEmpty() &&
            resolveRemoteDropTarget(
                    dropEvent->position().toPoint(),
                    mappingId,
                    remoteDirectory,
                    displayPath)) {
            const QFileInfo source(localPath);
            if (!source.isRoot()) {
                m_Busy = true;
                m_ProgressVisible = true;
                m_ProgressPercent = 0;
                setStatus(
                        QStringLiteral("正在准备上传：%1")
                                .arg(source.fileName()));
                emit requestUpload(
                        source.absoluteFilePath(),
                        mappingId,
                        remoteDirectory);
                dropEvent->acceptProposedAction();
                return true;
            }
        }
        dropEvent->ignore();
        return true;
    }

    return QRasterWindow::event(event);
}

void FileTransferWindow::paintEvent(QPaintEvent*)
{
    clampScrollOffsets();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(QRect(QPoint(0, 0), size()), QColor(18, 21, 26));

    const QColor panel(30, 34, 40);
    const QColor header(25, 29, 35);
    const QColor border(58, 67, 78);
    const QColor text(239, 242, 246);
    const QColor muted(157, 167, 180);
    const QColor accent(50, 133, 255);
    const QColor selected(42, 78, 119);

    painter.setPen(text);
    QFont heading = painter.font();
    heading.setPointSize(12);
    heading.setBold(true);
    painter.setFont(heading);
    painter.drawText(QRect(localPaneRect().x(), 4, localPaneRect().width(), 34),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     QStringLiteral("本机"));
    painter.drawText(QRect(remotePaneRect().x(), 4, remotePaneRect().width(), 34),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     QStringLiteral("远程主机"));

    auto drawPath = [&](const QRect& rect, const QString& path, bool canGoUp) {
        painter.setBrush(panel);
        painter.setPen(border);
        painter.drawRoundedRect(rect, 6, 6);
        painter.setPen(text);
        QFont normal = painter.font();
        normal.setBold(false);
        normal.setPointSize(10);
        painter.setFont(normal);
        const QString shown = path.isEmpty()
                ? QStringLiteral("磁盘")
                : QDir::toNativeSeparators(path);
        painter.drawText(rect.adjusted(12, 0, -82, 0),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         painter.fontMetrics().elidedText(
                                 shown, Qt::ElideMiddle, rect.width() - 100));
        painter.setPen(canGoUp ? accent : muted);
        painter.drawText(QRect(rect.right() - 76, rect.y(), 72, rect.height()),
                         Qt::AlignCenter,
                         QStringLiteral("↑ 上一级"));
    };
    drawPath(localPathRect(), m_LocalPath, !m_LocalPath.isEmpty());
    const QString remoteDisplay = m_RemoteMappingId.isEmpty()
            ? QString()
            : m_RemoteMappingName +
                    (m_RemotePath.isEmpty()
                             ? QStringLiteral("\\")
                             : QStringLiteral("\\") +
                                       QDir::toNativeSeparators(m_RemotePath));
    drawPath(remotePathRect(), remoteDisplay, !m_RemoteMappingId.isEmpty());

    painter.setBrush(panel);
    painter.setPen(border);
    painter.drawRoundedRect(localPaneRect(), 6, 6);
    painter.drawRoundedRect(remotePaneRect(), 6, 6);

    auto drawEntries = [&](bool local) {
        const QRect pane = local ? localPaneRect() : remotePaneRect();
        const QRect columnHeader(
                pane.x() + 1,
                pane.y() + 1,
                pane.width() - 2,
                kColumnHeaderHeight - 1);
        painter.fillRect(columnHeader, header);
        painter.setPen(border);
        painter.drawLine(
                columnHeader.bottomLeft(),
                columnHeader.bottomRight());
        QFont columnFont = painter.font();
        columnFont.setBold(false);
        columnFont.setPointSize(9);
        painter.setFont(columnFont);
        painter.setPen(muted);
        painter.drawText(
                columnHeader.adjusted(42, 0, -90, 0),
                Qt::AlignLeft | Qt::AlignVCenter,
                QStringLiteral("名称"));
        painter.drawText(
                columnHeader.adjusted(
                        columnHeader.width() - 84, 0, -12, 0),
                Qt::AlignRight | Qt::AlignVCenter,
                QStringLiteral("大小"));

        const QVector<Entry>& entries = local ? m_LocalEntries : m_RemoteEntries;
        const int selection = local ? m_LocalSelection : m_RemoteSelection;
        const int offset = local ? m_LocalScroll : m_RemoteScroll;
        const int visible = visibleRowCount();
        for (int row = 0; row < visible && offset + row < entries.size(); ++row) {
            const int index = offset + row;
            const Entry& entry = entries.at(index);
            const QRect rect = rowRect(local, row);
            if (index == selection) {
                painter.fillRect(rect.adjusted(2, 1, -2, -1), selected);
            }
            else if (row % 2 != 0) {
                painter.fillRect(
                        rect.adjusted(2, 1, -2, -1),
                        QColor(32, 37, 44));
            }

            const QRect iconRect(
                    rect.x() + 10,
                    rect.center().y() - kIconSize / 2,
                    kIconSize,
                    kIconSize);
            if (!entry.icon.isNull()) {
                painter.drawImage(iconRect, entry.icon);
            }

            painter.setPen(index == selection ? Qt::white : text);
            QFont normal = painter.font();
            normal.setBold(entry.drive);
            normal.setPointSize(10);
            painter.setFont(normal);
            const int sizeWidth = 82;
            painter.drawText(rect.adjusted(42, 0, -sizeWidth, 0),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             painter.fontMetrics().elidedText(entry.name,
                                                              Qt::ElideRight,
                                                              rect.width() - sizeWidth - 48));
            if (!entry.directory) {
                painter.setPen(muted);
                painter.drawText(rect.adjusted(rect.width() - sizeWidth, 0, -10, 0),
                                 Qt::AlignRight | Qt::AlignVCenter,
                                 displaySize(entry.size));
            }
        }
    };
    drawEntries(true);
    drawEntries(false);

    auto drawButton = [&](const QRect& rect, const QString& label, bool enabled) {
        painter.setBrush(enabled ? accent : QColor(55, 62, 72));
        painter.setPen(Qt::NoPen);
        painter.drawRoundedRect(rect, 7, 7);
        painter.setPen(enabled ? Qt::white : muted);
        QFont button = painter.font();
        button.setBold(true);
        button.setPointSize(10);
        painter.setFont(button);
        painter.drawText(rect, Qt::AlignCenter, label);
    };
    drawButton(uploadButtonRect(), QStringLiteral("上传 →"), !m_Busy);
    drawButton(downloadButtonRect(), QStringLiteral("← 下载"), !m_Busy);
    drawButton(refreshButtonRect(), QStringLiteral("刷新"), !m_Busy);

    const QRect statusRect(0, height() - kStatusHeight, width(), kStatusHeight);
    painter.fillRect(statusRect, QColor(13, 16, 20));
    if (m_ProgressVisible) {
        const QRect progressTrack(
                kMargin,
                statusRect.y() + 5,
                width() - kMargin * 2,
                4);
        painter.fillRect(progressTrack, QColor(45, 51, 60));
        painter.fillRect(
                QRect(
                        progressTrack.x(),
                        progressTrack.y(),
                        progressTrack.width() * m_ProgressPercent / 100,
                        progressTrack.height()),
                accent);
    }
    painter.setPen(m_StatusError ? QColor(255, 112, 112) : muted);
    QFont statusFont = painter.font();
    statusFont.setBold(false);
    statusFont.setPointSize(10);
    painter.setFont(statusFont);
    painter.drawText(statusRect.adjusted(kMargin, 0, -kMargin, 0),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     painter.fontMetrics().elidedText(m_Status, Qt::ElideRight, width() - kMargin * 2));

    if (m_DragActive) {
        const QRect targetPane = m_DragSourceLocal
                ? remotePaneRect()
                : localPaneRect();
        QPen targetPen(
                m_DragTargetValid ? accent : QColor(220, 91, 91),
                2,
                Qt::DashLine);
        painter.setPen(targetPen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(targetPane.adjusted(2, 2, -2, -2), 7, 7);

        const QString sourceName = m_DragSourceLocal
                ? m_LocalEntries.value(m_DragSourceIndex).name
                : m_RemoteEntries.value(m_DragSourceIndex).name;
        QFont hintFont = painter.font();
        hintFont.setPointSize(9);
        hintFont.setBold(false);
        painter.setFont(hintFont);
        const QString hint = sourceName + QStringLiteral("\n") + m_DragHint;
        const QRect textBounds = painter.fontMetrics().boundingRect(
                QRect(0, 0, 360, 70),
                Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap,
                hint);
        QSize bubbleSize(
                std::min(380, std::max(180, textBounds.width() + 26)),
                std::max(58, textBounds.height() + 18));
        QPoint bubbleTopLeft =
                m_DragPosition + QPoint(18, 18);
        bubbleTopLeft.setX(std::clamp(
                bubbleTopLeft.x(), 8, width() - bubbleSize.width() - 8));
        bubbleTopLeft.setY(std::clamp(
                bubbleTopLeft.y(), 8, height() - bubbleSize.height() - 8));
        const QRect bubble(bubbleTopLeft, bubbleSize);
        painter.setPen(border);
        painter.setBrush(QColor(38, 43, 51, 245));
        painter.drawRoundedRect(bubble, 8, 8);
        painter.setPen(m_DragTargetValid ? text : QColor(255, 145, 145));
        painter.drawText(
                bubble.adjusted(13, 8, -13, -8),
                Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap,
                hint);
    }
}

void FileTransferWindow::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QRasterWindow::mousePressEvent(event);
        return;
    }

    m_DragSourceIndex = -1;
    m_DragActive = false;
    m_DragHint.clear();
    const QPoint point = event->position().toPoint();
    if (localPathRect().contains(point) && point.x() >= localPathRect().right() - 80) {
        localUp();
        return;
    }
    if (remotePathRect().contains(point) && point.x() >= remotePathRect().right() - 80) {
        remoteUp();
        return;
    }
    if (uploadButtonRect().contains(point)) {
        beginUpload();
        return;
    }
    if (downloadButtonRect().contains(point)) {
        beginDownload();
        return;
    }
    if (refreshButtonRect().contains(point) && !m_Busy) {
        refreshLocal();
        refreshRemote();
        return;
    }

    const int localIndex = rowAt(true, point);
    if (localIndex >= 0) {
        m_LocalSelection = localIndex;
        if (!m_LocalEntries.at(localIndex).drive && !m_Busy) {
            m_DragSourceLocal = true;
            m_DragSourceIndex = localIndex;
            m_DragStart = point;
            m_DragPosition = point;
        }
        update();
        return;
    }
    const int remoteIndex = rowAt(false, point);
    if (remoteIndex >= 0) {
        m_RemoteSelection = remoteIndex;
        if (!m_RemoteEntries.at(remoteIndex).drive && !m_Busy) {
            m_DragSourceLocal = false;
            m_DragSourceIndex = remoteIndex;
            m_DragStart = point;
            m_DragPosition = point;
        }
        update();
    }
}

void FileTransferWindow::mouseMoveEvent(QMouseEvent* event)
{
    if (m_DragSourceIndex < 0 ||
        m_Busy ||
        !(event->buttons() & Qt::LeftButton)) {
        QRasterWindow::mouseMoveEvent(event);
        return;
    }

    const QPoint point = event->position().toPoint();
    if (!m_DragActive) {
        const int dragDistance =
                QGuiApplication::styleHints()->startDragDistance();
        if ((point - m_DragStart).manhattanLength() < dragDistance) {
            return;
        }
        m_DragActive = true;
    }
    updateDrag(point);
}

void FileTransferWindow::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && m_DragActive) {
        const QPoint point = event->position().toPoint();
        finishDrag(point);
        resetDrag();
        event->accept();
        return;
    }
    m_DragSourceIndex = -1;
    QRasterWindow::mouseReleaseEvent(event);
}

void FileTransferWindow::mouseDoubleClickEvent(QMouseEvent* event)
{
    const QPoint point = event->position().toPoint();
    const int localIndex = rowAt(true, point);
    if (localIndex >= 0) {
        m_LocalSelection = localIndex;
        openLocalSelection();
        return;
    }
    const int remoteIndex = rowAt(false, point);
    if (remoteIndex >= 0) {
        m_RemoteSelection = remoteIndex;
        openRemoteSelection();
    }
}

void FileTransferWindow::wheelEvent(QWheelEvent* event)
{
    const int delta = event->angleDelta().y() > 0 ? -3 : 3;
    if (localPaneRect().contains(event->position().toPoint())) {
        m_LocalScroll += delta;
    }
    else if (remotePaneRect().contains(event->position().toPoint())) {
        m_RemoteScroll += delta;
    }
    clampScrollOffsets();
    update();
}

void FileTransferWindow::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape) {
        hide();
    }
    else if (event->key() == Qt::Key_F5 && !m_Busy) {
        refreshLocal();
        refreshRemote();
    }
    else {
        QRasterWindow::keyPressEvent(event);
    }
}
