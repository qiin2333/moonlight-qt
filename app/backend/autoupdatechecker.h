#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QVector>

class QNetworkReply;
class PortableUpdateInstaller;

class AutoUpdateChecker : public QObject
{
    Q_OBJECT
public:
    explicit AutoUpdateChecker(QObject *parent = nullptr);

    Q_INVOKABLE void start();
    Q_INVOKABLE bool supportsInAppUpdate() const;
    Q_INVOKABLE void installUpdate(QString url);

signals:
    void onUpdateAvailable(QString newVersion, QString url);
    void onPortableUpdateStatusChanged(QString message);
    void onPortableUpdateFailed(QString message);

private slots:
    void handleUpdateCheckRequestFinished(QNetworkReply* reply);

private:
    void parseStringToVersionQuad(QString& string, QVector<int>& version);

    int compareVersion(QVector<int>& version1, QVector<int>& version2);

    bool isPortableInstall() const;
    QString getExpectedAssetPrefix() const;
    QString getExpectedAssetSuffix();
    QString getCurrentBuildArch() const;

    QVector<int> m_CurrentVersionQuad;
    QNetworkAccessManager* m_Nam;
    PortableUpdateInstaller* m_PortableUpdateInstaller;
};
