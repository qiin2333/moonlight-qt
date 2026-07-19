#include "filemappingtransfer.h"

#include "filemappingprotocoladapter.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QObject>
#include <QRunnable>
#include <QThread>
#include <QThreadPool>
#include <QUuid>

#include <memory>
#include <utility>

namespace {

constexpr quint32 kUploadChunkBytes = 256U * 1024U;

QString transferRoot(const QString& mirrorRoot)
{
    return QDir(mirrorRoot).filePath(QStringLiteral("Send to Host"));
}

QString sentRoot(const QString& mirrorRoot)
{
    return QDir(mirrorRoot).filePath(QStringLiteral("Sent to Host"));
}

bool writeTextFile(const QString& path, const QString& text)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return false;
    }
    return file.write(text.toUtf8()) == text.toUtf8().size();
}

void publishEvent(const std::shared_ptr<FileMappingTransfer::State>& state, const QString& message)
{
    QMutexLocker locker(&state->lock);
    state->events.append(message);
    while (state->events.size() > 20) {
        state->events.removeFirst();
    }
}

bool stopRequested(const std::shared_ptr<FileMappingTransfer::State>& state)
{
    return state->stopRequested.load(std::memory_order_relaxed);
}

QString fileSignature(const QFileInfo& info)
{
    return QStringLiteral("%1:%2")
            .arg(info.size())
            .arg(info.lastModified().toMSecsSinceEpoch());
}

QString remotePathFromLocal(const QString& mappingRoot, const QString& localPath)
{
    return QDir::fromNativeSeparators(QDir(mappingRoot).relativeFilePath(localPath));
}

bool isTransferMetadata(const QString& path)
{
    const QString name = QFileInfo(path).fileName();
    // README.txt is created beside the mapping folders, so a real file with
    // that name inside a share must remain transferable. Error sidecars are
    // the only files that should be ignored by the upload scanner.
    return name.endsWith(QStringLiteral(".moonlight-error.txt"), Qt::CaseInsensitive);
}

QString uniqueSentPath(const QString& directory, const QString& fileName)
{
    QDir().mkpath(directory);
    QString candidate = QDir(directory).filePath(fileName);
    if (!QFileInfo::exists(candidate)) {
        return candidate;
    }

    const QFileInfo info(fileName);
    const QString base = info.completeBaseName().isEmpty() ? fileName : info.completeBaseName();
    const QString suffix = info.suffix().isEmpty() ? QString() : QStringLiteral(".") + info.suffix();
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    candidate = QDir(directory).filePath(QStringLiteral("%1-%2%3").arg(base, stamp, suffix));
    for (int index = 2; QFileInfo::exists(candidate); ++index) {
        candidate = QDir(directory).filePath(
                QStringLiteral("%1-%2-%3%4").arg(base, stamp).arg(index).arg(suffix));
    }
    return candidate;
}

FileMapping::Error ensureRemoteParents(FileMapping::ProtocolClient& client,
                                       const QString& mappingId,
                                       const QString& remotePath,
                                       int timeoutMs)
{
    const QStringList segments = remotePath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (segments.size() <= 1) {
        return FileMapping::Error::none();
    }

    QString current;
    for (int index = 0; index + 1 < segments.size(); ++index) {
        if (!current.isEmpty()) {
            current += QLatin1Char('/');
        }
        current += segments.at(index);
        const FileMapping::Error error = client.mkdir(mappingId, current, timeoutMs);
        if (!error.ok()) {
            return error;
        }
    }
    return FileMapping::Error::none();
}

FileMapping::Error uploadFile(FileMapping::ProtocolClient& client,
                              const QString& mappingId,
                              const QString& remotePath,
                              const QString& localPath,
                              const std::shared_ptr<FileMappingTransfer::State>& state,
                              int timeoutMs)
{
    QFile file(localPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return FileMapping::Error::make(
                FileMapping::ErrorKind::Internal,
                QObject::tr("Could not open %1 for upload.").arg(QFileInfo(localPath).fileName()));
    }

    const quint64 totalSize = static_cast<quint64>(file.size());
    const QFileInfo initialInfo(localPath);
    const qint64 initialModified = initialInfo.lastModified().toMSecsSinceEpoch();
    const QString uploadId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    quint64 offset = 0;
    bool first = true;

    do {
        if (stopRequested(state)) {
            return FileMapping::Error::make(FileMapping::ErrorKind::Unavailable,
                                            QObject::tr("File transfer stopped."));
        }

        // The scan loop only starts stable files, but a user can still edit a
        // file while it is being sent. Never commit a chunk from a changed
        // source into a remote file with a different size/mtime.
        QFileInfo currentInfo(localPath);
        currentInfo.refresh();
        if (!currentInfo.exists() || static_cast<quint64>(currentInfo.size()) != totalSize ||
                currentInfo.lastModified().toMSecsSinceEpoch() != initialModified) {
            return FileMapping::Error::make(
                    FileMapping::ErrorKind::Unavailable,
                    QObject::tr("%1 changed during upload; it will be retried after it is stable.")
                            .arg(QFileInfo(localPath).fileName()));
        }

        const QByteArray chunk = file.read(kUploadChunkBytes);
        if (chunk.isNull()) {
            return FileMapping::Error::make(
                    FileMapping::ErrorKind::Internal,
                    QObject::tr("Could not read %1 during upload.").arg(QFileInfo(localPath).fileName()));
        }
        if (chunk.isEmpty() && offset < totalSize) {
            return FileMapping::Error::make(
                    FileMapping::ErrorKind::Internal,
                    QObject::tr("%1 changed or became unavailable during upload.")
                            .arg(QFileInfo(localPath).fileName()));
        }
        const bool complete = offset + static_cast<quint64>(chunk.size()) >= totalSize;
        const FileMapping::WriteResult result = client.write(mappingId,
                                                             remotePath,
                                                             uploadId,
                                                             offset,
                                                             totalSize,
                                                             chunk,
                                                             first,
                                                             complete,
                                                             timeoutMs);
        if (!result.ok()) {
            return result.error;
        }

        const quint64 expectedOffset = offset + static_cast<quint64>(chunk.size());
        if (result.nextOffset != expectedOffset) {
            return FileMapping::Error::make(
                    FileMapping::ErrorKind::Internal,
                    QObject::tr("Host acknowledged an unexpected upload offset."));
        }
        offset = expectedOffset;
        first = false;
    } while (offset < totalSize || first);

    return FileMapping::Error::none();
}

bool isTransient(const FileMapping::Error& error)
{
    return error.kind == FileMapping::ErrorKind::Network ||
           error.kind == FileMapping::ErrorKind::Timeout ||
           error.kind == FileMapping::ErrorKind::Unavailable;
}

class TransferTask : public QRunnable
{
public:
    TransferTask(NvComputer computer,
                 QString mirrorRoot,
                 std::shared_ptr<FileMappingTransfer::State> state,
                 int timeoutMs)
        : m_Computer(std::move(computer)),
          m_MirrorRoot(std::move(mirrorRoot)),
          m_State(std::move(state)),
          m_TimeoutMs(timeoutMs)
    {
        setAutoDelete(true);
    }

    void run() override
    {
        const QString outbox = transferRoot(m_MirrorRoot);
        QDir().mkpath(outbox);
        QDir().mkpath(sentRoot(m_MirrorRoot));
        writeTextFile(QDir(outbox).filePath(QStringLiteral("README.txt")),
                      QStringLiteral(
                              "Moonlight file transfer / Moonlight 文件传输\n\n"
                              "Folders for writable host shares will appear here.\n"
                              "Drop files into a share folder. Moonlight uploads them automatically and moves successful files to \"Sent to Host\".\n"
                              "主机上设置为“读写”的共享会显示在这里。把文件拖进对应目录即可自动上传；成功后文件会移到“Sent to Host”。\n\n"
                              "Existing host files are never overwritten. Rename the local file if the host already has the same name.\n"
                              "不会覆盖主机上的同名文件；如遇重名，请重命名本地文件后重试。\n"));
        {
            QMutexLocker locker(&m_State->lock);
            m_State->outboxPath = outbox;
        }

        std::unique_ptr<FileMappingProtocolAdapter> client;
        QList<FileMapping::RemoteMapping> writableMappings;
        QHash<QString, QString> observedSignatures;
        QHash<QString, QString> failedSignatures;
        bool announcedNoWritableMappings = false;
        int emptyMappingPolls = 0;

        while (!stopRequested(m_State)) {
            if (!client) {
                client = std::make_unique<FileMappingProtocolAdapter>(m_Computer);
                const FileMapping::Capability capability = client->fetchCapability(m_TimeoutMs);
                if (!capability.error.ok()) {
                    publishEvent(m_State, QObject::tr("File transfer is waiting for the host: %1")
                                             .arg(capability.error.message));
                    client.reset();
                    waitForRetry();
                    continue;
                }

                const FileMapping::Error connectError = client->connectSession(capability, m_TimeoutMs);
                if (!connectError.ok()) {
                    publishEvent(m_State, QObject::tr("File transfer could not connect: %1")
                                             .arg(connectError.message));
                    client.reset();
                    waitForRetry();
                    continue;
                }

                writableMappings.clear();
                for (const FileMapping::RemoteMapping& mapping : client->mappings()) {
                    if (mapping.mode == QStringLiteral("readwrite") &&
                            mapping.capabilities.contains(QStringLiteral("write"))) {
                        writableMappings.append(mapping);
                        QDir().mkpath(QDir(outbox).filePath(mapping.id));
                        QDir().mkpath(QDir(sentRoot(m_MirrorRoot)).filePath(mapping.id));
                    }
                }

                if (writableMappings.isEmpty() && !announcedNoWritableMappings) {
                    announcedNoWritableMappings = true;
                    publishEvent(m_State,
                                 QObject::tr("Host folders are read-only. Enable Read & Write on a Sunshine share to send files."));
                }
                else if (!writableMappings.isEmpty()) {
                    emptyMappingPolls = 0;
                    publishEvent(m_State,
                                 QObject::tr("File transfer is ready. Drop files into \"Send to Host\"."));
                }
            }

            bool reconnect = false;
            QHash<QString, QString> currentlySeen;
            for (const FileMapping::RemoteMapping& mapping : writableMappings) {
                const QString mappingRoot = QDir(outbox).filePath(mapping.id);
                QDirIterator iterator(mappingRoot,
                                      QDir::Files | QDir::NoDotAndDotDot,
                                      QDirIterator::Subdirectories);
                while (iterator.hasNext() && !stopRequested(m_State)) {
                    const QString localPath = iterator.next();
                    if (isTransferMetadata(localPath)) {
                        continue;
                    }

                    const QFileInfo info(localPath);
                    const QString signature = fileSignature(info);
                    currentlySeen.insert(localPath, signature);
                    if (observedSignatures.value(localPath) != signature) {
                        observedSignatures.insert(localPath, signature);
                        failedSignatures.remove(localPath);
                        QFile::remove(localPath + QStringLiteral(".moonlight-error.txt"));
                        continue;
                    }
                    if (failedSignatures.value(localPath) == signature) {
                        continue;
                    }

                    const QString remotePath = remotePathFromLocal(mappingRoot, localPath);
                    FileMapping::Error error = ensureRemoteParents(*client,
                                                                  mapping.id,
                                                                  remotePath,
                                                                  m_TimeoutMs);
                    if (error.ok()) {
                        error = uploadFile(*client,
                                           mapping.id,
                                           remotePath,
                                           localPath,
                                           m_State,
                                           m_TimeoutMs);
                    }

                    if (!error.ok()) {
                        if (stopRequested(m_State)) {
                            break;
                        }
                        if (isTransient(error)) {
                            publishEvent(m_State,
                                         QObject::tr("File transfer paused; it will retry %1.")
                                                 .arg(info.fileName()));
                            client.reset();
                            reconnect = true;
                            break;
                        }

                        failedSignatures.insert(localPath, signature);
                        writeTextFile(localPath + QStringLiteral(".moonlight-error.txt"),
                                      QObject::tr("Moonlight could not send this file.\n\n%1\n\n"
                                                  "Fix the problem, then modify or rename the file to retry.\n")
                                              .arg(error.message));
                        publishEvent(m_State,
                                     QObject::tr("Could not send %1: %2")
                                             .arg(info.fileName(), error.message));
                        continue;
                    }

                    const QString sentDirectory = QDir(sentRoot(m_MirrorRoot)).filePath(mapping.id);
                    const QString sentPath = uniqueSentPath(sentDirectory, info.fileName());
                    if (!QFile::rename(localPath, sentPath)) {
                        publishEvent(m_State,
                                     QObject::tr("%1 reached the host, but Moonlight could not move the local copy to \"Sent to Host\".")
                                             .arg(info.fileName()));
                    }
                    else {
                        publishEvent(m_State,
                                     QObject::tr("Sent %1 to %2.")
                                             .arg(info.fileName(), mapping.displayName));
                    }
                    observedSignatures.remove(localPath);
                    failedSignatures.remove(localPath);
                }
                if (reconnect || stopRequested(m_State)) {
                    break;
                }
            }

            for (auto it = observedSignatures.begin(); it != observedSignatures.end();) {
                if (!currentlySeen.contains(it.key())) {
                    failedSignatures.remove(it.key());
                    it = observedSignatures.erase(it);
                }
                else {
                    ++it;
                }
            }

            if (!reconnect) {
                if (writableMappings.isEmpty() && ++emptyMappingPolls >= 10) {
                    emptyMappingPolls = 0;
                    client.reset();
                }
                waitForScan();
            }
        }

        QMutexLocker locker(&m_State->lock);
        m_State->finished = true;
        m_State->finishedCondition.wakeAll();
    }

private:
    void waitForScan() const
    {
        for (int elapsed = 0; elapsed < 1000 && !stopRequested(m_State); elapsed += 100) {
            QThread::msleep(100);
        }
    }

    void waitForRetry() const
    {
        for (int elapsed = 0; elapsed < 3000 && !stopRequested(m_State); elapsed += 100) {
            QThread::msleep(100);
        }
    }

    NvComputer m_Computer;
    QString m_MirrorRoot;
    std::shared_ptr<FileMappingTransfer::State> m_State;
    int m_TimeoutMs;
};

} // namespace

namespace FileMappingTransfer {

void start(NvComputer computer,
           QString mirrorRoot,
           std::shared_ptr<State> state,
           int timeoutMs)
{
    if (!state) {
        return;
    }
    QThreadPool::globalInstance()->start(
            new TransferTask(std::move(computer), std::move(mirrorRoot), std::move(state), timeoutMs));
}

void stopAndWait(const std::shared_ptr<State>& state, int timeoutMs)
{
    if (!state) {
        return;
    }

    state->stopRequested.store(true, std::memory_order_relaxed);
    QMutexLocker locker(&state->lock);
    if (!state->finished) {
        state->finishedCondition.wait(&state->lock, timeoutMs);
    }
}

} // namespace FileMappingTransfer
