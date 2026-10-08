#include "pipwbackground.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QUrlQuery>

#include <algorithm>

namespace {
constexpr qint64 MaxBytes = 20 * 1024 * 1024;
bool isRedirect(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}
const QStringList Extensions = {"webp", "png", "jpg", "jpeg", "avif", "gif", "bmp"};
}

PipwReviewIndex::PipwReviewIndex(const QByteArray &json)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return;
    const QJsonObject root = document.object();
    if (root.keys() != QStringList({"pc", "phone"})) return;
    const QRegularExpression md5Pattern("^[0-9a-f]{32}$");
    for (int pool = 0; pool < 2; ++pool) {
        const QString key = pool == 0 ? "pc" : "phone";
        if (!root.value(key).isArray() || root.value(key).toArray().isEmpty()) return;
        QHash<QString, QByteArray> names;
        const QRegularExpression namePattern("^image-" + key + "-[0-9]+\\.([a-zA-Z0-9]+)$");
        for (const QJsonValue &value : root.value(key).toArray()) {
            if (!value.isObject()) return;
            const QJsonObject record = value.toObject();
            if (record.keys() != QStringList({"id", "md5"}) ||
                !record.value("id").isString() || !record.value("md5").isString()) return;
            const QString name = record.value("id").toString();
            const QString digest = record.value("md5").toString();
            const auto match = namePattern.match(name);
            if (!match.hasMatch() || !Extensions.contains(match.captured(1).toLower()) ||
                !md5Pattern.match(digest).hasMatch() || names.contains(name)) return;
            const QByteArray rawDigest = QByteArray::fromHex(digest.toLatin1());
            names.insert(name, rawDigest);
            auto &byDigest = m_byDigest[pool];
            if (!byDigest.contains(rawDigest) || name < byDigest.value(rawDigest)) {
                byDigest.insert(rawDigest, name);
            }
        }
    }
    m_valid = true;
}

bool PipwReviewIndex::accepts(const QByteArray &digest, bool phone) const
{
    return m_valid && m_byDigest[phone ? 1 : 0].contains(digest);
}

QStringList PipwReviewIndex::fallbackFilenames(bool phone) const
{
    if (!m_valid) return {};
    QStringList names = m_byDigest[phone ? 1 : 0].values();
    std::sort(names.begin(), names.end());
    return names;
}

const PipwReviewIndex &PipwReviewIndex::bundled()
{
    static const PipwReviewIndex index([] {
        QFile resource(":/res/backgrounds/pipw_review_index.json");
        return resource.open(QIODevice::ReadOnly) ? resource.readAll() : QByteArray();
    }());
    return index;
}

bool PipwUrlPolicy::handles(const QUrl &url)
{
    return url.host().compare("img-api.pipw.top", Qt::CaseInsensitive) == 0;
}

bool PipwUrlPolicy::isApi(const QUrl &url)
{
    return handles(url) && url.isValid() && url.scheme() == "https" &&
           url.userInfo().isEmpty() && !url.hasFragment();
}

bool PipwUrlPolicy::isCandidate(const QUrl &url, bool phone)
{
    if (!url.isValid() || url.scheme() != "https" || url.host().isEmpty() ||
        !url.userInfo().isEmpty() || url.hasFragment()) return false;
    const QRegularExpression encodedSeparators("%(2f|5c|25|2e)", QRegularExpression::CaseInsensitiveOption);
    if (encodedSeparators.match(url.path(QUrl::FullyEncoded)).hasMatch()) return false;
    const QString name = url.fileName(QUrl::FullyDecoded);
    const int dot = name.lastIndexOf('.');
    if (dot <= 0 || name.contains('/') || name.contains('\\') || name.contains(QChar(0)) ||
        !Extensions.contains(name.mid(dot + 1).toLower())) return false;
    if (name.startsWith(phone ? "image-pc-" : "image-phone-")) return false;
    return true;
}

QUrl PipwUrlPolicy::fallback(const QUrl &url, const QString &filename)
{
    QUrl result(url);
    QString path = url.path(QUrl::FullyDecoded);
    path = path.left(path.lastIndexOf('/') + 1) + filename;
    result.setPath(path);
    result.setQuery(QString());
    result.setFragment(QString());
    return result;
}

PipwBackgroundDownloader::PipwBackgroundDownloader(QObject *parent)
    : QObject(parent), m_manager(this)
{
    m_timeout.setSingleShot(true);
    m_idleTimeout.setSingleShot(true);
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        if (m_reply) {
            m_readError = tr("Background download timed out");
            m_reply->abort();
        }
    });
    connect(&m_idleTimeout, &QTimer::timeout, this, [this] {
        if (m_reply) {
            m_readError = tr("Background download timed out");
            m_reply->abort();
        }
    });
}

void PipwBackgroundDownloader::cancel()
{
    ++m_generation;
    m_timeout.stop();
    m_idleTimeout.stop();
    if (m_reply) {
        auto *reply = m_reply.data();
        m_reply.clear();
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
    m_bytes.clear();
}

void PipwBackgroundDownloader::start(const QUrl &api)
{
    cancel();
    m_phone = QUrlQuery(api).queryItemValue("phone") == "true";
    m_fallbackTried = false;
    m_template = {};
    m_visited.clear();
    m_overall.start();
    if (!PipwUrlPolicy::isApi(api) || !PipwReviewIndex::bundled().isValid()) {
        const quint64 generation = m_generation;
        QTimer::singleShot(0, this, [this, generation] {
            if (generation == m_generation) completeFailure(tr("Invalid Pipw source or review index"));
        });
        return;
    }
    request(api, true);
}

void PipwBackgroundDownloader::request(const QUrl &url, bool api)
{
    const qint64 budget = api ? 7000 : 15000 - m_candidate.elapsed();
    const int remaining = static_cast<int>(qMin(budget, 30000 - m_overall.elapsed()));
    if (remaining <= 0) {
        completeFailure(tr("Background download timed out"));
        return;
    }
    if (!api) {
        if (m_visited.contains(url.toString()) || m_visited.size() >= 3) {
            reject(tr("Invalid Pipw redirect chain"));
            return;
        }
        m_visited.append(url.toString());
    }
    m_readError.clear();
    m_bytes.clear();
    m_digest.reset();
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setRawHeader("Accept-Encoding", "identity");
    request.setRawHeader("User-Agent", "Moonlight-VPlus");
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setTransferTimeout(5000);
#endif
    auto *reply = m_manager.get(request);
    m_reply = reply;
    reply->setReadBufferSize(64 * 1024);
#if QT_VERSION < QT_VERSION_CHECK(5, 15, 0)
    // Qt 5.12 has no request transfer timeout. Retain an idle timeout as
    // well as the API/candidate/overall deadlines on these builds.
    m_idleTimeout.start(5000);
    connect(reply, &QNetworkReply::metaDataChanged, this, [this] {
        m_idleTimeout.start(5000);
    });
#endif
    connect(reply, &QNetworkReply::readyRead, this, [this, reply, api] {
#if QT_VERSION < QT_VERSION_CHECK(5, 15, 0)
        m_idleTimeout.start(5000);
#endif
        // A redirect response body is never used as image data.
        if (!api && !isRedirect(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt())) {
            consume(reply);
        } else {
            reply->readAll();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, url, api] {
        finish(reply, url, api);
    });
    m_timeout.start(remaining);
}

void PipwBackgroundDownloader::consume(QNetworkReply *reply)
{
    if (!m_readError.isEmpty()) return;
    bool hasLength = false;
    const qint64 length = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong(&hasLength);
    const QByteArray encoding = reply->rawHeader("Content-Encoding");
    if ((hasLength && length > MaxBytes) || (!encoding.isEmpty() && encoding.toLower() != "identity")) {
        m_readError = tr("Invalid or oversized Pipw image");
        reply->abort();
        return;
    }
    while (reply->bytesAvailable() > 0) {
        const QByteArray chunk = reply->read(64 * 1024);
        if (m_bytes.size() + chunk.size() > MaxBytes) {
            m_readError = tr("Pipw image exceeds the 20 MiB limit");
            reply->abort();
            return;
        }
        m_digest.addData(chunk);
        m_bytes.append(chunk);
    }
}

void PipwBackgroundDownloader::finish(QNetworkReply *reply, const QUrl &url, bool api)
{
    if (m_reply != reply) {
        reply->deleteLater();
        return;
    }
    m_timeout.stop();
    m_idleTimeout.stop();
    // consume() can abort synchronously; finish this reply only once.
    reply->disconnect(this);
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (!api && !isRedirect(status)) consume(reply);
    m_reply.clear();
    reply->deleteLater();
    if (!m_readError.isEmpty() || reply->error() != QNetworkReply::NoError) {
        const QString error = m_readError.isEmpty() ? reply->errorString() : m_readError;
        if (api) completeFailure(error); else reject(error);
        return;
    }
    if (isRedirect(status)) {
        const QUrl location = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
        const QUrl target = location.isEmpty() ? QUrl() : url.resolved(location);
        if (!PipwUrlPolicy::isCandidate(target, m_phone)) {
            if (api) completeFailure(tr("Invalid Pipw redirect")); else reject(tr("Invalid Pipw redirect"));
            return;
        }
        m_template = target;
        if (api) m_candidate.start();
        request(target, false);
        return;
    }
    if (api || status != 200 || m_bytes.isEmpty()) {
        if (api) completeFailure(tr("Pipw API did not return an image redirect"));
        else reject(tr("Pipw image is unavailable"));
        return;
    }
    bool hasLength = false;
    const qint64 length = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong(&hasLength);
    if ((hasLength && length != m_bytes.size()) ||
        !PipwReviewIndex::bundled().accepts(m_digest.result(), m_phone)) {
        reject(tr("Pipw image has not passed content review"));
        return;
    }
    emit ready(m_bytes);
    m_bytes.clear();
}

void PipwBackgroundDownloader::reject(const QString &message)
{
    m_bytes.clear();
    if (!m_fallbackTried && !m_template.isEmpty() && m_overall.elapsed() < 30000) {
        m_fallbackTried = true;
        QStringList candidates = PipwReviewIndex::bundled().fallbackFilenames(m_phone);
        for (const QString &visited : m_visited) candidates.removeAll(QUrl(visited).fileName());
        if (!candidates.isEmpty()) {
            const QString name = candidates.at(QRandomGenerator::global()->bounded(static_cast<int>(candidates.size())));
            const QUrl fallback = PipwUrlPolicy::fallback(m_template, name);
            if (PipwUrlPolicy::isCandidate(fallback, m_phone)) {
                m_visited.clear();
                m_candidate.start();
                request(fallback, false);
                return;
            }
        }
    }
    completeFailure(message);
}

void PipwBackgroundDownloader::completeFailure(const QString &message)
{
    cancel();
    emit failed(message);
}

void PipwBackgroundDownloader::rejectDecodedImage()
{
    reject(tr("Unable to decode the reviewed background image"));
}
