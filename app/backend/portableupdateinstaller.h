#pragma once

#include <QObject>
#include <QNetworkAccessManager>

class QFile;
class QNetworkReply;

class PortableUpdateInstaller : public QObject
{
    Q_OBJECT
public:
    explicit PortableUpdateInstaller(QObject *parent = nullptr);

    bool supportsInAppUpdate() const;
    void installUpdate(const QString& url);

signals:
    void onPortableUpdateStatusChanged(QString message);
    void onPortableUpdateFailed(QString message);

private slots:
    void handlePortableUpdateMetaDataChanged();
    void handlePortableUpdateDownloadReadyRead();
    void handlePortableUpdateDownloadProgress(qint64 bytesReceived, qint64 bytesTotal);
    void handlePortableUpdateDownloadFinished();

private:
    bool isPortableInstall() const;
    QString getPortableUpdaterExecutable() const;
    bool hasLikelyWritableInstallDir() const;
    bool ensureWritableInstallDir(QString& errorMessage) const;
    QString createPortableUpdateWorkspace() const;
    QString materializePortableUpdateScript(const QString& workspace) const;
    bool ensureSufficientDiskSpace(qint64 requiredBytes, QString& errorMessage) const;
    qint64 estimateRequiredWorkspaceBytes(qint64 archiveBytes) const;
    void resetPortableUpdateState(bool removeWorkspace);

    QNetworkAccessManager* m_UpdateNam;
    QNetworkReply* m_UpdateReply;
    QFile* m_UpdateFile;
    QString m_PortableUpdateWorkspace;
    QString m_PortableUpdateError;
    bool m_MetadataChecked;
};
