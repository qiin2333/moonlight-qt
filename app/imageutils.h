#ifndef IMAGEUTILS_H
#define IMAGEUTILS_H

#include <QObject>
#include <QUrl>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QHash>

class QNetworkReply;
class PipwBackgroundDownloader;

class ImageUtils : public QObject
{
    Q_OBJECT
public:
    explicit ImageUtils(QObject *parent = nullptr);
    ~ImageUtils() override;

    Q_INVOKABLE void saveImageToFile(const QString &imageUrl, const QUrl &localPath);
    Q_INVOKABLE QString backgroundExportUrl(const QString &imageUrl) const;
    Q_INVOKABLE bool prepareBackgroundExport(const QString &imageUrl);
    Q_INVOKABLE void cancelBackgroundExport();
    Q_INVOKABLE QUrl backgroundExportDirectory();
    Q_INVOKABLE bool isPipwSource(const QString &url) const;
    Q_INVOKABLE bool isReviewedBackground(const QString &path, const QString &apiUrl);
    Q_INVOKABLE void cancelBackgroundFetch();
    // Returns false when another background request is already in flight. The
    // caller can then coalesce the refresh and retry after the active request.
    Q_INVOKABLE bool fetchAndSaveRandomBackground(const QString &apiUrl);
    Q_INVOKABLE bool fileExists(const QString &path);
    Q_INVOKABLE bool isValidCache(const QString &cachePath);
    Q_INVOKABLE bool validateExtension(const QString &filePath);
    // Returns an empty string when the file is suitable for a local background,
    // otherwise a translated explanation that can be shown directly by QML.
    Q_INVOKABLE QString validateLocalBackgroundImage(const QString &fileUrl);

private:
    void startBackgroundRequest();
    void retryOrFailBackground(const QString &errorMessage);
    void decodeBackground(const QByteArray &imageData, bool reviewed);
    bool restoreReviewedBackground(const QString &path, bool phone);
    static QString decodeAndSaveBackground(const QByteArray &imageData, bool reviewed = false);
    static QByteArray convertToJpeg(const QByteArray &imageData);

    QNetworkAccessManager m_backgroundNetworkManager;
    QUrl m_backgroundApiUrl;
    int m_backgroundAttempt = 0;
    bool m_backgroundFetchInProgress = false;
    quint64 m_backgroundGeneration = 0;
    QPointer<QNetworkReply> m_backgroundReply;
    QPointer<PipwBackgroundDownloader> m_pipwDownloader;
    struct ReviewedBackground
    {
        QString originalPath;
        QByteArray originalDigest;
        QByteArray previewDigest;
        bool phone;
    };
    QHash<QString, ReviewedBackground> m_reviewedBackgrounds;
    struct
    {
        QString source;
        QString originalUrl;
        QString extension;
        QByteArray bytes;
        QByteArray digest;
    } m_pendingExport;

signals:
    void saveCompleted(bool success, const QString &message);
    void backgroundReady(const QString &filePath);
    void backgroundError(const QString &errorMessage);
    void backgroundBusy();
};

#endif // IMAGEUTILS_H
