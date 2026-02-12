#ifndef IMAGEUTILS_H
#define IMAGEUTILS_H

#include <QObject>
#include <QUrl>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QFile>
#include <QFileInfo>

class ImageUtils : public QObject
{
    Q_OBJECT
public:
    explicit ImageUtils(QObject *parent = nullptr);
    
    Q_INVOKABLE void saveImageToFile(const QString &imageUrl, const QUrl &localPath);
    Q_INVOKABLE QString saveImageFromUrl(const QString &url);
    Q_INVOKABLE QString fetchAndSaveRandomBackground(const QString &apiUrl);
    Q_INVOKABLE bool fileExists(const QString &path);
    Q_INVOKABLE bool isValidCache(const QString &cachePath);
    Q_INVOKABLE bool validateExtension(const QString &filePath);

private:
    QByteArray convertToJpeg(const QByteArray &imageData);

signals:
    void saveCompleted(bool success, const QString &message);
    void backgroundReady(const QString &filePath);
    void backgroundError(const QString &errorMessage);
};

#endif // IMAGEUTILS_H 