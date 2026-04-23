#include "autoupdatechecker.h"
#include "portableupdateinstaller.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QSysInfo>
#include <QTextStream>

// GitHub repository for update checks
#define GITHUB_OWNER "qiin2333"
#define GITHUB_REPO  "moonlight-qt"

AutoUpdateChecker::AutoUpdateChecker(QObject *parent) :
    QObject(parent)
{
    m_Nam = new QNetworkAccessManager(this);
    m_PortableUpdateInstaller = new PortableUpdateInstaller(this);

    // Never communicate over HTTP
    m_Nam->setStrictTransportSecurityEnabled(true);

    // Allow HTTP redirects
    m_Nam->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);

    connect(m_Nam, &QNetworkAccessManager::finished,
            this, &AutoUpdateChecker::handleUpdateCheckRequestFinished);

    QString currentVersion(VERSION_STR);
    qDebug() << "Current Moonlight version:" << currentVersion;
    parseStringToVersionQuad(currentVersion, m_CurrentVersionQuad);

    // Should at least have a 1.0-style version number
    Q_ASSERT(m_CurrentVersionQuad.count() > 1);

    connect(m_PortableUpdateInstaller, &PortableUpdateInstaller::onPortableUpdateStatusChanged,
            this, &AutoUpdateChecker::onPortableUpdateStatusChanged);
    connect(m_PortableUpdateInstaller, &PortableUpdateInstaller::onPortableUpdateFailed,
            this, &AutoUpdateChecker::onPortableUpdateFailed);
}

bool AutoUpdateChecker::supportsInAppUpdate() const
{
    return m_PortableUpdateInstaller->supportsInAppUpdate();
}

void AutoUpdateChecker::installUpdate(QString url)
{
    m_PortableUpdateInstaller->installUpdate(url);
}

void AutoUpdateChecker::start()
{
    if (!m_Nam) {
        Q_ASSERT(m_Nam);
        return;
    }

#if defined(Q_OS_WIN32) || defined(Q_OS_DARWIN) || defined(STEAM_LINK) || defined(APP_IMAGE)
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0) && QT_VERSION < QT_VERSION_CHECK(5, 15, 1) && !defined(QT_NO_BEARERMANAGEMENT)
    // HACK: Set network accessibility to work around QTBUG-80947 (introduced in Qt 5.14.0 and fixed in Qt 5.15.1)
    QT_WARNING_PUSH
    QT_WARNING_DISABLE_DEPRECATED
    m_Nam->setNetworkAccessible(QNetworkAccessManager::Accessible);
    QT_WARNING_POP
#endif

    // Query GitHub Releases API for the latest release
    QUrl url(QString("https://api.github.com/repos/%1/%2/releases/latest")
                 .arg(GITHUB_OWNER, GITHUB_REPO));
    QNetworkRequest request(url);

    // GitHub API requires a User-Agent header
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QString("Moonlight/%1").arg(VERSION_STR));
    // Request JSON response
    request.setRawHeader("Accept", "application/vnd.github+json");

#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, true);
#else
    request.setAttribute(QNetworkRequest::HTTP2AllowedAttribute, true);
#endif
    m_Nam->get(request);
#endif
}

void AutoUpdateChecker::parseStringToVersionQuad(QString& string, QVector<int>& version)
{
    // Strip leading 'v' if present (e.g., "v6.2.21" -> "6.2.21")
    QString versionStr = string;
    if (versionStr.startsWith('v') || versionStr.startsWith('V')) {
        versionStr = versionStr.mid(1);
    }

    QStringList list = versionStr.split('.');
    for (const QString& component : std::as_const(list)) {
        version.append(component.toInt());
    }
}

QString AutoUpdateChecker::getExpectedAssetSuffix()
{
#if defined(Q_OS_WIN32)
    return isPortableInstall() ? QStringLiteral(".zip") : QStringLiteral(".exe");
#elif defined(Q_OS_DARWIN)
    return QStringLiteral(".dmg");
#elif defined(APP_IMAGE)
    return QStringLiteral(".AppImage");
#else
    return QString();
#endif
}

bool AutoUpdateChecker::isPortableInstall() const
{
#if defined(Q_OS_WIN32)
    return QFile::exists(QDir::currentPath() + "/portable.dat");
#else
    return false;
#endif
}

QString AutoUpdateChecker::getExpectedAssetPrefix() const
{
#if defined(Q_OS_WIN32)
    if (isPortableInstall()) {
        return QStringLiteral("MoonlightPortable-%1-").arg(getCurrentBuildArch());
    }

    return QStringLiteral("MoonlightSetup-");
#else
    return QString();
#endif
}

QString AutoUpdateChecker::getCurrentBuildArch() const
{
    QString buildArch = QSysInfo::buildCpuArchitecture();

    if (buildArch == "x86_64") {
        return QStringLiteral("x64");
    }
    else if (buildArch == "i386") {
        return QStringLiteral("x86");
    }

    return buildArch.toLower();
}

int AutoUpdateChecker::compareVersion(QVector<int>& version1, QVector<int>& version2) {
    for (int i = 0;; i++) {
        int v1Val = 0;
        int v2Val = 0;

        // Treat missing decimal places as 0
        if (i < version1.count()) {
            v1Val = version1[i];
        }
        if (i < version2.count()) {
            v2Val = version2[i];
        }
        if (i >= version1.count() && i >= version2.count()) {
            // Equal versions
            return 0;
        }

        if (v1Val < v2Val) {
            return -1;
        }
        else if (v1Val > v2Val) {
            return 1;
        }
    }
}

void AutoUpdateChecker::handleUpdateCheckRequestFinished(QNetworkReply* reply)
{
    Q_ASSERT(reply->isFinished());

    // Delete the QNetworkAccessManager to free resources and
    // prevent the bearer plugin from polling in the background.
    m_Nam->deleteLater();
    m_Nam = nullptr;

    if (reply->error() == QNetworkReply::NoError) {
        QTextStream stream(reply);

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        stream.setEncoding(QStringConverter::Utf8);
#else
        stream.setCodec("UTF-8");
#endif

        // Read all data and queue the reply for deletion
        QString jsonString = stream.readAll();
        reply->deleteLater();

        QJsonParseError error;
        QJsonDocument jsonDoc = QJsonDocument::fromJson(jsonString.toUtf8(), &error);
        if (jsonDoc.isNull()) {
            qWarning() << "GitHub release response malformed:" << error.errorString();
            return;
        }

        if (!jsonDoc.isObject()) {
            qWarning() << "GitHub release response is not a JSON object";
            return;
        }

        QJsonObject releaseObj = jsonDoc.object();

        // GitHub Releases API response format:
        // {
        //   "tag_name": "v6.3.0",
        //   "name": "Release 6.3.0",
        //   "html_url": "https://github.com/owner/repo/releases/tag/v6.3.0",
        //   "prerelease": false,
        //   "draft": false,
        //   "assets": [
        //     {
        //       "name": "MoonlightSetup-x64-6.3.0.exe",
        //       "browser_download_url": "https://github.com/..."
        //     }
        //   ]
        // }

        // Skip pre-releases and drafts
        if (releaseObj["prerelease"].toBool(false) || releaseObj["draft"].toBool(false)) {
            qDebug() << "Latest GitHub release is a pre-release or draft, skipping";
            return;
        }

        if (!releaseObj.contains("tag_name") || !releaseObj["tag_name"].isString()) {
            qWarning() << "GitHub release missing tag_name";
            return;
        }

        QString tagName = releaseObj["tag_name"].toString();
        qDebug() << "Latest GitHub release tag:" << tagName;

        // Parse version from tag (strip 'v' prefix if present)
        QVector<int> latestVersionQuad;
        parseStringToVersionQuad(tagName, latestVersionQuad);

        int res = compareVersion(m_CurrentVersionQuad, latestVersionQuad);
        if (res < 0) {
            // Current version is older than latest release
            qDebug() << "Update available:" << tagName;

            // Try to find a platform-specific download URL from assets
            QString downloadUrl;
            QString expectedPrefix = getExpectedAssetPrefix();
            QString expectedSuffix = getExpectedAssetSuffix();

            if (!expectedSuffix.isEmpty() && releaseObj.contains("assets") && releaseObj["assets"].isArray()) {
                QJsonArray assets = releaseObj["assets"].toArray();
                for (const auto& asset : std::as_const(assets)) {
                    if (asset.isObject()) {
                        QJsonObject assetObj = asset.toObject();
                        QString assetName = assetObj["name"].toString();
                        bool prefixMatches = expectedPrefix.isEmpty() ||
                                             assetName.startsWith(expectedPrefix, Qt::CaseInsensitive);
                        bool suffixMatches = assetName.endsWith(expectedSuffix, Qt::CaseInsensitive);

                        if (prefixMatches && suffixMatches) {
                            downloadUrl = assetObj["browser_download_url"].toString();
                            qDebug() << "Found matching asset:" << assetName;
                            break;
                        }
                    }
                }
            }

            // Fall back to the release page URL if no matching asset found
            if (downloadUrl.isEmpty()) {
                downloadUrl = releaseObj["html_url"].toString();
            }

            emit onUpdateAvailable(tagName, downloadUrl);
        }
        else if (res > 0) {
            qDebug() << "Current version is newer than latest release";
        }
        else {
            qDebug() << "Current version matches latest release";
        }
    }
    else {
        qWarning() << "Update checking failed:" << reply->error() << reply->errorString();
        reply->deleteLater();
    }
}
