#pragma once

#include <QByteArray>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QHash>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QTimer>
#include <QUrl>

class QNetworkReply;

// Content allowlist shared with moonlight-vplus PR #696. Names only identify
// fallback candidates; actual response bytes must match a reviewed digest.
class PipwReviewIndex
{
public:
    explicit PipwReviewIndex(const QByteArray &json);
    bool isValid() const { return m_valid; }
    bool accepts(const QByteArray &digest, bool phone) const;
    QStringList fallbackFilenames(bool phone) const;
    static const PipwReviewIndex &bundled();

private:
    bool m_valid = false;
    QHash<QByteArray, QString> m_byDigest[2];
};

namespace PipwUrlPolicy {
bool handles(const QUrl &url);
bool isApi(const QUrl &url);
bool isCandidate(const QUrl &url, bool phone);
QUrl fallback(const QUrl &url, const QString &filename);
}

class PipwBackgroundDownloader : public QObject
{
    Q_OBJECT
public:
    explicit PipwBackgroundDownloader(QObject *parent = nullptr);
    void start(const QUrl &api);
    void cancel();
    void rejectDecodedImage();

signals:
    void ready(const QByteArray &bytes);
    void failed(const QString &message);

private:
    void request(const QUrl &url, bool api);
    void finish(QNetworkReply *reply, const QUrl &url, bool api);
    void consume(QNetworkReply *reply);
    void reject(const QString &message);
    void completeFailure(const QString &message);

    QNetworkAccessManager m_manager;
    QPointer<QNetworkReply> m_reply;
    QTimer m_timeout;
    QTimer m_idleTimeout;
    QElapsedTimer m_overall;
    QElapsedTimer m_candidate;
    QByteArray m_bytes;
    QCryptographicHash m_digest{QCryptographicHash::Md5};
    QUrl m_template;
    QStringList m_visited;
    bool m_phone = false;
    bool m_fallbackTried = false;
    QString m_readError;
    quint64 m_generation = 0;
};
