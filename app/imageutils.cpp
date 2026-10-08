#include "imageutils.h"
#include "pipwbackground.h"

#include <QBuffer>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QUrlQuery>
#include <QThread>
#include <QTimer>

#include <limits>
#include <memory>

namespace {

QString localPathFromUrlOrPath(const QString &value)
{
    const QUrl url(value);
    if (url.isLocalFile()) {
        return url.toLocalFile();
    }

    const QFileInfo fileInfo(value);
    return fileInfo.isAbsolute() ? fileInfo.absoluteFilePath() : QString();
}

bool hasSupportedBackgroundExtension(const QString &filePath)
{
    static const QStringList allowedExtensions = {"jpg", "jpeg", "png", "webp", "bmp"};
    return allowedExtensions.contains(QFileInfo(filePath).suffix().toLower());
}

QString reviewedImageExtension(const QByteArray &data)
{
    if (data.startsWith(QByteArray::fromHex("ffd8ff")))
        return "jpg";
    if (data.startsWith(QByteArray::fromHex("89504e470d0a1a0a")))
        return "png";
    if (data.startsWith("RIFF") && data.mid(8, 4) == "WEBP")
        return "webp";
    if (data.startsWith("GIF87a") || data.startsWith("GIF89a"))
        return "gif";
    if (data.startsWith("BM"))
        return "bmp";
    if (data.mid(4, 4) == "ftyp" &&
        (data.mid(8, 32).contains("avif") || data.mid(8, 32).contains("avis")))
        return "avif";
    return {};
}

bool writeNewImage(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return false;
    const bool written = file.write(bytes) == bytes.size() && file.flush();
    file.close();
    if (!written || file.error() != QFileDevice::NoError) {
        file.remove();
        return false;
    }
    return true;
}

QString originalFormatDestination(const QUrl &requestedUrl, const QString &extension)
{
    QString path = requestedUrl.toLocalFile();
    if (!path.isEmpty() && QFileInfo(path).suffix().compare(extension, Qt::CaseInsensitive) != 0) {
        const QFileInfo requested(path);
        path = requested.dir().filePath(requested.completeBaseName() + "." + extension);
    }
    return path;
}
}

#ifdef Q_OS_WIN
#include <wincodec.h>
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#endif

ImageUtils::ImageUtils(QObject *parent)
    : QObject(parent),
      m_backgroundNetworkManager(this)
{
}

ImageUtils::~ImageUtils()
{
    cancelBackgroundFetch();
    for (auto it = m_reviewedBackgrounds.cbegin(); it != m_reviewedBackgrounds.cend(); ++it) {
        QFile::remove(it.key());
        QFile::remove(it.value().originalPath);
    }
}

void ImageUtils::saveImageToFile(const QString &imageUrl, const QUrl &localPath)
{
    if (!m_pendingExport.bytes.isEmpty() && m_pendingExport.source == imageUrl) {
        const QString destination = originalFormatDestination(localPath, m_pendingExport.extension);
        const bool verified =
            QCryptographicHash::hash(m_pendingExport.bytes, QCryptographicHash::Md5) ==
            m_pendingExport.digest;
        const bool written =
            verified && !destination.isEmpty() && writeNewImage(destination, m_pendingExport.bytes);
        cancelBackgroundExport();
        emit saveCompleted(written, written ? destination : tr("Unable to write file"));
        return;
    }
    cancelBackgroundExport();
    const QString exportedUrl = backgroundExportUrl(imageUrl);
    if (exportedUrl.isEmpty()) {
        emit saveCompleted(false, tr("The reviewed background is no longer available"));
        return;
    }
    if (exportedUrl != imageUrl &&
        !isReviewedBackground(localPathFromUrlOrPath(imageUrl), m_backgroundApiUrl.toString())) {
        emit saveCompleted(false, tr("The reviewed background is no longer available"));
        return;
    }
    QNetworkAccessManager *manager = new QNetworkAccessManager(this);
    QNetworkReply *reply = manager->get(QNetworkRequest(QUrl(exportedUrl)));
    const auto reviewed = m_reviewedBackgrounds.constFind(localPathFromUrlOrPath(imageUrl));
    const QByteArray expectedDigest =
        reviewed == m_reviewedBackgrounds.cend() ? QByteArray() : reviewed->originalDigest;
    QString destination = localPath.toLocalFile();
    if (!expectedDigest.isEmpty()) {
        const QString extension = QFileInfo(reviewed->originalPath).suffix();
        if (QFileInfo(destination).suffix().compare(extension, Qt::CaseInsensitive) != 0) {
            const QFileInfo requested(destination);
            destination = requested.dir().filePath(requested.completeBaseName() + "." + extension);
        }
    }

    connect(
        reply, &QNetworkReply::finished, this,
        [this, manager, reply, destination, expectedDigest]() {
            if (reply->error() == QNetworkReply::NoError) {
                const QByteArray payload = reply->readAll();
                if (!expectedDigest.isEmpty() &&
                    QCryptographicHash::hash(payload, QCryptographicHash::Md5) != expectedDigest) {
                    emit saveCompleted(false, tr("The reviewed background is no longer available"));
                    reply->deleteLater();
                    manager->deleteLater();
                    return;
                }
                const QString filePath = destination;
                const bool written = !filePath.isEmpty() && writeNewImage(filePath, payload);
                emit saveCompleted(written, written ? filePath : tr("Unable to write file"));
            } else {
                emit saveCompleted(false, reply->errorString());
            }

            reply->deleteLater();
            manager->deleteLater();
        });
}

QString ImageUtils::backgroundExportUrl(const QString &imageUrl) const
{
    if (!m_pendingExport.bytes.isEmpty() && m_pendingExport.source == imageUrl) {
        return m_pendingExport.originalUrl;
    }
    const auto found = m_reviewedBackgrounds.constFind(localPathFromUrlOrPath(imageUrl));
    return found == m_reviewedBackgrounds.cend()
               ? imageUrl
               : QUrl::fromLocalFile(found->originalPath).toString();
}

bool ImageUtils::prepareBackgroundExport(const QString &imageUrl)
{
    cancelBackgroundExport();
    const QString path = localPathFromUrlOrPath(imageUrl);
    const auto found = m_reviewedBackgrounds.constFind(path);
    if (found == m_reviewedBackgrounds.cend()) {
        // A retired Pipw preview must not silently become an ordinary export.
        if (QFileInfo(path).fileName().startsWith("pipw-preview-")) {
            emit saveCompleted(false, tr("The reviewed background is no longer available"));
            return false;
        }
        return !imageUrl.isEmpty();
    }
    QFile original(found->originalPath);
    if (!original.open(QIODevice::ReadOnly) || original.size() > 20 * 1024 * 1024) {
        emit saveCompleted(false, tr("The reviewed background is no longer available"));
        return false;
    }
    const QByteArray bytes = original.read(20 * 1024 * 1024 + 1);
    const QByteArray digest = QCryptographicHash::hash(bytes, QCryptographicHash::Md5);
    if (original.error() != QFileDevice::NoError || bytes.isEmpty() ||
        bytes.size() > 20 * 1024 * 1024 || digest != found->originalDigest ||
        !PipwReviewIndex::bundled().accepts(digest, found->phone)) {
        emit saveCompleted(false, tr("The reviewed background is no longer available"));
        return false;
    }
    m_pendingExport.source = imageUrl;
    m_pendingExport.originalUrl = QUrl::fromLocalFile(found->originalPath).toString();
    m_pendingExport.extension = QFileInfo(found->originalPath).suffix();
    m_pendingExport.bytes = bytes;
    m_pendingExport.digest = digest;
    return true;
}

void ImageUtils::cancelBackgroundExport()
{
    m_pendingExport = {};
}

QUrl ImageUtils::backgroundExportDirectory()
{
    const QString directory =
        QStandardPaths::writableLocation(QStandardPaths::PicturesLocation) + "/vplus";
    if (!QDir().mkpath(directory)) {
        emit saveCompleted(false, tr("Unable to create the background export directory"));
        return {};
    }
    return QUrl::fromLocalFile(directory);
}

bool ImageUtils::isPipwSource(const QString &url) const
{
    return PipwUrlPolicy::handles(QUrl(url));
}

bool ImageUtils::isReviewedBackground(const QString &path, const QString &apiUrl) const
{
    const auto found = m_reviewedBackgrounds.constFind(path);
    if (found == m_reviewedBackgrounds.cend() ||
        found->phone != (QUrlQuery(QUrl(apiUrl)).queryItemValue("phone") == "true"))
        return false;
    QFile original(found->originalPath);
    QFile preview(path);
    QCryptographicHash digest(QCryptographicHash::Md5);
    if (!original.open(QIODevice::ReadOnly) || original.size() > 20 * 1024 * 1024 ||
        !digest.addData(&original) || digest.result() != found->originalDigest ||
        !PipwReviewIndex::bundled().accepts(digest.result(), found->phone))
        return false;
    digest.reset();
    return preview.open(QIODevice::ReadOnly) && digest.addData(&preview) &&
           digest.result() == found->previewDigest;
}

void ImageUtils::cancelBackgroundFetch()
{
    ++m_backgroundGeneration;
    if (m_pipwDownloader) {
        m_pipwDownloader->disconnect(this);
        m_pipwDownloader->cancel();
        m_pipwDownloader->deleteLater();
        m_pipwDownloader.clear();
    }
    if (m_backgroundReply) {
        m_backgroundReply->disconnect(this);
        m_backgroundReply->abort();
        m_backgroundReply->deleteLater();
        m_backgroundReply.clear();
    }
    m_backgroundFetchInProgress = false;
}

bool ImageUtils::fetchAndSaveRandomBackground(const QString &apiUrl)
{
    if (m_backgroundFetchInProgress) {
        emit backgroundBusy();
        return false;
    }

    cancelBackgroundFetch();
    m_backgroundApiUrl = QUrl(apiUrl);
    if (!m_backgroundApiUrl.isValid() ||
            (m_backgroundApiUrl.scheme() != QStringLiteral("http") &&
             m_backgroundApiUrl.scheme() != QStringLiteral("https"))) {
        // Keep signal delivery asynchronous like a real network failure. This
        // lets QML record which source the failed request belonged to first.
        const quint64 generation = m_backgroundGeneration;
        QTimer::singleShot(0, this, [this, generation]() {
            if (generation == m_backgroundGeneration)
                emit backgroundError(tr("Invalid background image URL"));
        });
        return false;
    }

    m_backgroundAttempt = 0;
    m_backgroundFetchInProgress = true;
    if (PipwUrlPolicy::handles(m_backgroundApiUrl)) {
        auto *downloader = new PipwBackgroundDownloader(this);
        m_pipwDownloader = downloader;
        connect(downloader, &PipwBackgroundDownloader::ready, this,
                [this](const QByteArray &bytes) { decodeBackground(bytes, true); });
        connect(downloader, &PipwBackgroundDownloader::failed, this,
                [this](const QString &message) {
                    m_backgroundFetchInProgress = false;
                    emit backgroundError(message);
                });
        downloader->start(m_backgroundApiUrl);
        return true;
    }
    startBackgroundRequest();
    return true;
}

void ImageUtils::startBackgroundRequest()
{
    ++m_backgroundAttempt;

    QNetworkRequest request(m_backgroundApiUrl);
    request.setRawHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                                       "AppleWebKit/537.36 (KHTML, like Gecko) "
                                       "Chrome/125.0.0.0 Safari/537.36");
    request.setRawHeader("Accept", "image/avif,image/webp,image/apng,image/svg+xml,image/*,*/*;q=0.8");
    request.setRawHeader("Accept-Language", "zh-CN,zh;q=0.9,en;q=0.8");
    request.setRawHeader("Referer", m_backgroundApiUrl.toEncoded());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setTransferTimeout(15000);
#endif

    QNetworkReply *reply = m_backgroundNetworkManager.get(request);
    m_backgroundReply = reply;
#if QT_VERSION < QT_VERSION_CHECK(5, 15, 0)
    QTimer::singleShot(15000, reply, [reply]() {
        if (reply->isRunning()) {
            reply->abort();
        }
    });
#endif
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        if (reply->error() != QNetworkReply::NoError) {
            const QString errorMessage = reply->errorString();
            reply->deleteLater();
            retryOrFailBackground(errorMessage);
            return;
        }

        const QByteArray imageData = reply->readAll();
        reply->deleteLater();
        if (imageData.size() < 1024) {
            retryOrFailBackground(tr("Background server returned an empty response"));
            return;
        }

        m_backgroundReply.clear();
        decodeBackground(imageData, false);
    });
}

void ImageUtils::retryOrFailBackground(const QString &errorMessage)
{
    qWarning() << "fetchAndSaveRandomBackground: attempt" << m_backgroundAttempt
               << "failed:" << errorMessage;
    if (m_backgroundAttempt < 3) {
        const quint64 generation = m_backgroundGeneration;
        QTimer::singleShot(500 * m_backgroundAttempt, this, [this, generation]() {
            if (generation == m_backgroundGeneration)
                startBackgroundRequest();
        });
        return;
    }

    m_backgroundFetchInProgress = false;
    emit backgroundError(errorMessage);
}

void ImageUtils::decodeBackground(const QByteArray &imageData, bool reviewed)
{
    const quint64 generation = m_backgroundGeneration;
    const bool phone = QUrlQuery(m_backgroundApiUrl).queryItemValue("phone") == "true";
    struct Result
    {
        QString preview;
        QString original;
        QByteArray previewDigest;
        bool retained = false;
        ~Result()
        {
            if (!retained) {
                QFile::remove(preview);
                QFile::remove(original);
            }
        }
    };
    const auto result = std::make_shared<Result>();
    QThread *worker = QThread::create([result, imageData, reviewed] {
        result->preview = decodeAndSaveBackground(imageData, reviewed);
        if (reviewed && !result->preview.isEmpty()) {
            const QString extension = reviewedImageExtension(imageData);
            if (extension.isEmpty())
                return;
            const QString directory = QFileInfo(result->preview).absolutePath();
            QTemporaryFile original(directory + "/pipw-original-XXXXXX." + extension);
            if (original.open() && original.write(imageData) == imageData.size() &&
                original.flush()) {
                original.setAutoRemove(false);
                result->original = original.fileName();
                QFile preview(result->preview);
                QCryptographicHash hash(QCryptographicHash::Md5);
                if (preview.open(QIODevice::ReadOnly) && hash.addData(&preview)) {
                    result->previewDigest = hash.result();
                }
            }
        }
    });
    connect(
        worker, &QThread::finished, this, [this, generation, result, imageData, reviewed, phone] {
            if (generation != m_backgroundGeneration) {
                QFile::remove(result->preview);
                QFile::remove(result->original);
                return;
            }
            if (result->preview.isEmpty() ||
                (reviewed && (result->original.isEmpty() || result->previewDigest.isEmpty()))) {
                QFile::remove(result->preview);
                QFile::remove(result->original);
                if (reviewed) {
                    if (m_pipwDownloader)
                        m_pipwDownloader->rejectDecodedImage();
                    else {
                        m_backgroundFetchInProgress = false;
                        emit backgroundError(tr("Unable to decode the reviewed background image"));
                    }
                } else {
                    retryOrFailBackground(tr("Unable to decode background image"));
                }
                return;
            }
            // Pipw files are owned by this instance. Never prune another instance's
            // verified original, or its save dialog may lose the selected image.
            for (auto it = m_reviewedBackgrounds.cbegin(); it != m_reviewedBackgrounds.cend();
                 ++it) {
                QFile::remove(it.key());
                QFile::remove(it.value().originalPath);
            }
            m_reviewedBackgrounds.clear();
            if (reviewed) {
                m_reviewedBackgrounds.insert(
                    result->preview, { result->original,
                                       QCryptographicHash::hash(imageData, QCryptographicHash::Md5),
                                       result->previewDigest, phone });
            }
            result->retained = true;
            // Only application-owned cache files in the new directory are pruned.
            // User exports in Pictures/vplus and the legacy cache are untouched.
            QDir cache(QFileInfo(result->preview).absolutePath());
            const auto previous = cache.entryInfoList({ "background_*.*" }, QDir::Files);
            for (const auto &file : previous) {
                if (file.absoluteFilePath() != result->preview &&
                    file.absoluteFilePath() != result->original) {
                    QFile::remove(file.absoluteFilePath());
                }
            }
            m_backgroundFetchInProgress = false;
            emit backgroundReady(result->preview);
        });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

QString ImageUtils::decodeAndSaveBackground(const QByteArray &imageData, bool reviewed)
{
    QByteArray decodableData = imageData;
    QImage image;
    if (!image.loadFromData(decodableData)) {
        decodableData = convertToJpeg(decodableData);
        if (decodableData.isEmpty() || !image.loadFromData(decodableData)) {
            return QString();
        }
    }

    const QString cacheDir =
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/vplus-backgrounds";
    QDir().mkpath(cacheDir);
    QTemporaryFile output(cacheDir +
                          (reviewed ? "/pipw-preview-XXXXXX.jpg" : "/background_XXXXXX.jpg"));
    if (!output.open() || !image.save(&output, "JPEG", 90) || !output.flush()) {
        return QString();
    }
    output.setAutoRemove(false);
    return output.fileName();
}

QByteArray ImageUtils::convertToJpeg(const QByteArray &imageData)
{
    QImage image;
    if (image.loadFromData(imageData)) {
        QByteArray result;
        QBuffer buffer(&result);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "JPEG", 90);
        return result;
    }

#ifdef Q_OS_WIN
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool shouldUninitialize = SUCCEEDED(comResult);

    QByteArray result;
    IWICImagingFactory *factory = nullptr;
    IWICStream *stream = nullptr;
    IWICBitmapDecoder *decoder = nullptr;
    IWICBitmapFrameDecode *frame = nullptr;

    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (FAILED(hr)) goto cleanup;

    hr = factory->CreateStream(&stream);
    if (FAILED(hr)) goto cleanup;

    hr = stream->InitializeFromMemory(reinterpret_cast<BYTE *>(const_cast<char *>(imageData.data())),
                                      imageData.size());
    if (FAILED(hr)) goto cleanup;

    hr = factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr)) goto cleanup;

    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) goto cleanup;

    {
        UINT width = 0;
        UINT height = 0;
        frame->GetSize(&width, &height);

        const qint64 stride = static_cast<qint64>(width) * 4;
        const qint64 bufferSize = stride * static_cast<qint64>(height);
        static constexpr qint64 kMaximumDecodedBytes = 512LL * 1024 * 1024;
        if (width == 0 || height == 0 ||
                width > static_cast<UINT>((std::numeric_limits<int>::max)()) ||
                height > static_cast<UINT>((std::numeric_limits<int>::max)()) ||
                stride > (std::numeric_limits<UINT>::max)() ||
                bufferSize <= 0 || bufferSize > kMaximumDecodedBytes ||
                bufferSize > (std::numeric_limits<UINT>::max)()) {
            goto cleanup;
        }

        IWICFormatConverter *converter = nullptr;
        hr = factory->CreateFormatConverter(&converter);
        if (FAILED(hr)) goto cleanup;

        hr = converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom);
        if (FAILED(hr)) {
            converter->Release();
            goto cleanup;
        }

        QByteArray pixels(static_cast<qsizetype>(bufferSize), 0);
        hr = converter->CopyPixels(nullptr, static_cast<UINT>(stride),
                                   static_cast<UINT>(bufferSize),
                                   reinterpret_cast<BYTE *>(pixels.data()));
        converter->Release();
        if (FAILED(hr)) goto cleanup;

        QImage wicImage(reinterpret_cast<const uchar *>(pixels.constData()),
                        static_cast<int>(width), static_cast<int>(height),
                        static_cast<qsizetype>(stride), QImage::Format_ARGB32);
        QBuffer buffer(&result);
        buffer.open(QIODevice::WriteOnly);
        wicImage.save(&buffer, "JPEG", 90);
    }

cleanup:
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();
    if (shouldUninitialize) CoUninitialize();
    return result;
#else
    qWarning() << "convertToJpeg: unsupported image format. Install qtimageformats for WebP support.";
    return QByteArray();
#endif
}

bool ImageUtils::fileExists(const QString &path)
{
    const QString localPath = localPathFromUrlOrPath(path);
    return !localPath.isEmpty() && QFileInfo::exists(localPath);
}

bool ImageUtils::isValidCache(const QString &cachePath)
{
    if (!fileExists(cachePath)) {
        return false;
    }

    const QFileInfo fileInfo(cachePath);
    const qint64 twentyFourHours = 24 * 60 * 60;
    return fileInfo.lastModified().secsTo(QDateTime::currentDateTime()) < twentyFourHours &&
           fileInfo.size() > 1024 &&
           fileInfo.suffix().toLower() == "jpg";
}

bool ImageUtils::validateExtension(const QString &filePath)
{
    const QString localPath = localPathFromUrlOrPath(filePath);
    return !localPath.isEmpty() && hasSupportedBackgroundExtension(localPath);
}

QString ImageUtils::validateLocalBackgroundImage(const QString &fileUrl)
{
    const QString localPath = localPathFromUrlOrPath(fileUrl);
    if (localPath.isEmpty()) {
        return tr("Only local image files can be used as backgrounds");
    }

    const QFileInfo fileInfo(localPath);
    if (!fileInfo.exists() || !fileInfo.isFile() || !fileInfo.isReadable()) {
        return tr("The selected image file is unavailable");
    }

    if (!hasSupportedBackgroundExtension(localPath)) {
        return tr("Unsupported image format");
    }

    // Reading the header is enough to reject renamed or damaged non-images
    // without decoding a potentially large image on the UI thread.
    QImageReader reader(localPath);
    if (!reader.canRead()) {
        return tr("Unable to decode the selected image");
    }

    return QString();
}
