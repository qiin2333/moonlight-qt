#include "portableupdateinstaller.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QTemporaryFile>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTimer>
#include <QUrl>

PortableUpdateInstaller::PortableUpdateInstaller(QObject *parent) :
    QObject(parent),
    m_UpdateNam(nullptr),
    m_UpdateReply(nullptr),
    m_UpdateFile(nullptr),
    m_MetadataChecked(false)
{
}

bool PortableUpdateInstaller::supportsInAppUpdate() const
{
#if defined(Q_OS_WIN32)
    return isPortableInstall() &&
            !getPortableUpdaterExecutable().isEmpty() &&
            hasLikelyWritableInstallDir();
#else
    return false;
#endif
}

void PortableUpdateInstaller::installUpdate(const QString& url)
{
    if (!supportsInAppUpdate()) {
        emit onPortableUpdateFailed(tr("In-app update is only supported for the portable Windows build."));
        return;
    }

    if (m_UpdateReply != nullptr) {
        emit onPortableUpdateStatusChanged(tr("Portable update is already in progress."));
        return;
    }

    QUrl downloadUrl = QUrl::fromUserInput(url.trimmed());
    if (!downloadUrl.isValid() || downloadUrl.scheme().isEmpty()) {
        emit onPortableUpdateFailed(tr("The update URL is invalid."));
        return;
    }

    if (!downloadUrl.path().endsWith(".zip", Qt::CaseInsensitive)) {
        emit onPortableUpdateFailed(tr("The portable update package was not found for this release."));
        return;
    }

    QString errorMessage;
    if (!ensureWritableInstallDir(errorMessage)) {
        emit onPortableUpdateFailed(errorMessage);
        return;
    }

    QString workspace = createPortableUpdateWorkspace();
    if (workspace.isEmpty()) {
        emit onPortableUpdateFailed(tr("Unable to create a temporary folder for the update."));
        return;
    }

    resetPortableUpdateState(true);
    m_PortableUpdateWorkspace = workspace;
    m_PortableUpdateError.clear();
    m_MetadataChecked = false;

    if (m_UpdateNam == nullptr) {
        m_UpdateNam = new QNetworkAccessManager(this);
        m_UpdateNam->setStrictTransportSecurityEnabled(true);
        m_UpdateNam->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
    }

    QString archivePath = QDir(workspace).filePath("MoonlightPortableUpdate.zip");
    m_UpdateFile = new QFile(archivePath, this);
    if (!m_UpdateFile->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        m_PortableUpdateError = tr("Unable to create the update package on disk.");
        resetPortableUpdateState(true);
        emit onPortableUpdateFailed(m_PortableUpdateError);
        return;
    }

    QNetworkRequest request(downloadUrl);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QString("Moonlight/%1").arg(VERSION_STR));
    request.setRawHeader("Accept", "application/octet-stream");

#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, true);
#else
    request.setAttribute(QNetworkRequest::HTTP2AllowedAttribute, true);
#endif

    m_UpdateReply = m_UpdateNam->get(request);
    connect(m_UpdateReply, &QNetworkReply::metaDataChanged,
            this, &PortableUpdateInstaller::handlePortableUpdateMetaDataChanged);
    connect(m_UpdateReply, &QNetworkReply::readyRead,
            this, &PortableUpdateInstaller::handlePortableUpdateDownloadReadyRead);
    connect(m_UpdateReply, &QNetworkReply::downloadProgress,
            this, &PortableUpdateInstaller::handlePortableUpdateDownloadProgress);
    connect(m_UpdateReply, &QNetworkReply::finished,
            this, &PortableUpdateInstaller::handlePortableUpdateDownloadFinished);

    emit onPortableUpdateStatusChanged(tr("Downloading portable update..."));
}

bool PortableUpdateInstaller::isPortableInstall() const
{
#if defined(Q_OS_WIN32)
    return QFile::exists(QDir::currentPath() + "/portable.dat");
#else
    return false;
#endif
}

QString PortableUpdateInstaller::getPortableUpdaterExecutable() const
{
#if defined(Q_OS_WIN32)
    QString executable = QStandardPaths::findExecutable(QStringLiteral("powershell.exe"));
    if (!executable.isEmpty()) {
        return executable;
    }

    return QStandardPaths::findExecutable(QStringLiteral("pwsh.exe"));
#else
    return QString();
#endif
}

bool PortableUpdateInstaller::hasLikelyWritableInstallDir() const
{
#if defined(Q_OS_WIN32)
    return QFileInfo(QDir::currentPath()).isWritable();
#else
    return false;
#endif
}

bool PortableUpdateInstaller::ensureWritableInstallDir(QString& errorMessage) const
{
#if defined(Q_OS_WIN32)
    QTemporaryFile probeFile(QDir(QDir::currentPath()).filePath("MoonlightUpdateWriteProbe-XXXXXX.tmp"));
    probeFile.setAutoRemove(true);

    if (!probeFile.open()) {
        errorMessage = tr("The current Moonlight folder is not writable. Move the portable build to a writable location or run it with sufficient permissions.");
        return false;
    }

    probeFile.close();
    return true;
#else
    Q_UNUSED(errorMessage);
    return false;
#endif
}

QString PortableUpdateInstaller::createPortableUpdateWorkspace() const
{
    QString workspace = QDir(QDir::currentPath()).filePath(
                QString("MoonlightPortableUpdate-%1-%2")
                .arg(QCoreApplication::applicationPid())
                .arg(QDateTime::currentMSecsSinceEpoch()));

    if (!QDir().mkpath(workspace)) {
        return QString();
    }

    return workspace;
}

QString PortableUpdateInstaller::materializePortableUpdateScript(const QString& workspace) const
{
    QFile resourceFile(":/data/install-portable-update.ps1");
    if (!resourceFile.open(QIODevice::ReadOnly)) {
        return QString();
    }

    QString scriptPath = QDir(workspace).filePath("install-portable-update.ps1");
    QFile scriptFile(scriptPath);
    if (!scriptFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return QString();
    }

    QByteArray scriptContents = resourceFile.readAll();
    if (scriptFile.write(scriptContents) != scriptContents.size()) {
        return QString();
    }

    scriptFile.close();
    return scriptPath;
}

bool PortableUpdateInstaller::ensureSufficientDiskSpace(qint64 requiredBytes, QString& errorMessage) const
{
    if (requiredBytes <= 0) {
        return true;
    }

    QStorageInfo storage(QDir::currentPath());
    storage.refresh();

    if (!storage.isValid() || !storage.isReady()) {
        errorMessage = tr("Unable to determine free disk space for the portable update.");
        return false;
    }

    if (storage.bytesAvailable() < requiredBytes) {
        errorMessage = tr("Not enough free disk space for the portable update. Need about %1 MB free.")
                .arg(QString::number(requiredBytes / (1024.0 * 1024.0), 'f', 0));
        return false;
    }

    return true;
}

qint64 PortableUpdateInstaller::estimateRequiredWorkspaceBytes(qint64 archiveBytes) const
{
    if (archiveBytes <= 0) {
        return 0;
    }

    static const qint64 kSafetyMarginBytes = 64LL * 1024 * 1024;

    // We keep the downloaded ZIP and an extracted copy side-by-side in the app directory.
    return archiveBytes * 3 + kSafetyMarginBytes;
}

void PortableUpdateInstaller::resetPortableUpdateState(bool removeWorkspace)
{
    if (m_UpdateReply != nullptr) {
        m_UpdateReply->deleteLater();
        m_UpdateReply = nullptr;
    }

    if (m_UpdateFile != nullptr) {
        if (m_UpdateFile->isOpen()) {
            m_UpdateFile->close();
        }
        m_UpdateFile->deleteLater();
        m_UpdateFile = nullptr;
    }

    if (removeWorkspace && !m_PortableUpdateWorkspace.isEmpty()) {
        QDir(m_PortableUpdateWorkspace).removeRecursively();
    }

    if (removeWorkspace) {
        m_PortableUpdateWorkspace.clear();
    }

    m_MetadataChecked = false;
}

void PortableUpdateInstaller::handlePortableUpdateMetaDataChanged()
{
    if (m_UpdateReply == nullptr || m_MetadataChecked) {
        return;
    }

    m_MetadataChecked = true;

    bool ok = false;
    qint64 archiveBytes = m_UpdateReply->header(QNetworkRequest::ContentLengthHeader).toLongLong(&ok);
    if (!ok || archiveBytes <= 0) {
        return;
    }

    QString errorMessage;
    if (!ensureSufficientDiskSpace(estimateRequiredWorkspaceBytes(archiveBytes), errorMessage)) {
        m_PortableUpdateError = errorMessage;
        m_UpdateReply->abort();
    }
}

void PortableUpdateInstaller::handlePortableUpdateDownloadReadyRead()
{
    if (m_UpdateReply == nullptr || m_UpdateFile == nullptr) {
        return;
    }

    QByteArray data = m_UpdateReply->readAll();
    if (data.isEmpty()) {
        return;
    }

    if (m_UpdateFile->write(data) != data.size()) {
        m_PortableUpdateError = tr("Failed while writing the update package to disk.");
        m_UpdateReply->abort();
    }
}

void PortableUpdateInstaller::handlePortableUpdateDownloadProgress(qint64 bytesReceived, qint64 bytesTotal)
{
    if (bytesTotal > 0) {
        int progress = static_cast<int>((bytesReceived * 100) / bytesTotal);
        emit onPortableUpdateStatusChanged(tr("Downloading portable update... %1%").arg(progress));
    }
    else {
        emit onPortableUpdateStatusChanged(
                    tr("Downloading portable update... %1 MB")
                    .arg(QString::number(bytesReceived / (1024.0 * 1024.0), 'f', 1)));
    }
}

void PortableUpdateInstaller::handlePortableUpdateDownloadFinished()
{
    handlePortableUpdateDownloadReadyRead();

    QString failureMessage = m_PortableUpdateError;
    if (m_UpdateReply != nullptr && m_UpdateReply->error() != QNetworkReply::NoError) {
        if (failureMessage.isEmpty()) {
            failureMessage = tr("Failed to download the update: %1").arg(m_UpdateReply->errorString());
        }
    }

    if (m_UpdateFile != nullptr) {
        m_UpdateFile->flush();
        m_UpdateFile->close();
    }

    if (!failureMessage.isEmpty()) {
        m_PortableUpdateError.clear();
        resetPortableUpdateState(true);
        emit onPortableUpdateFailed(failureMessage);
        return;
    }

    QString scriptPath = materializePortableUpdateScript(m_PortableUpdateWorkspace);
    if (scriptPath.isEmpty()) {
        resetPortableUpdateState(true);
        emit onPortableUpdateFailed(tr("Unable to prepare the portable update installer."));
        return;
    }

    QString zipPath = QDir(m_PortableUpdateWorkspace).filePath("MoonlightPortableUpdate.zip");
    QString workspacePath = QDir::toNativeSeparators(m_PortableUpdateWorkspace);
    QString installDir = QDir::toNativeSeparators(QDir::currentPath());
    QString exePath = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());

    QString updaterExecutable = getPortableUpdaterExecutable();
    QStringList arguments;
    arguments << "-NoProfile"
              << "-ExecutionPolicy" << "Bypass"
              << "-WindowStyle" << "Hidden"
              << "-File" << QDir::toNativeSeparators(scriptPath)
              << "-WorkspaceDir" << workspacePath
              << "-InstallDir" << installDir
              << "-ZipPath" << QDir::toNativeSeparators(zipPath)
              << "-ExePath" << exePath;

    if (updaterExecutable.isEmpty() ||
            !QProcess::startDetached(updaterExecutable, arguments, m_PortableUpdateWorkspace)) {
        resetPortableUpdateState(true);
        emit onPortableUpdateFailed(tr("Unable to launch the portable updater."));
        return;
    }

    emit onPortableUpdateStatusChanged(tr("Installing update and restarting Moonlight..."));

    if (m_UpdateReply != nullptr) {
        m_UpdateReply->deleteLater();
        m_UpdateReply = nullptr;
    }
    if (m_UpdateFile != nullptr) {
        if (m_UpdateFile->isOpen()) {
            m_UpdateFile->close();
        }
        m_UpdateFile->deleteLater();
        m_UpdateFile = nullptr;
    }

    QTimer::singleShot(0, qApp, &QCoreApplication::quit);
}
