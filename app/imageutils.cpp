#include "imageutils.h"
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QFile>
#include <QDir>
#include <QStandardPaths>
#include <QEventLoop>
#include <QFileInfo>
#include <QDateTime>
#include <QImage>
#include <QBuffer>

#ifdef Q_OS_WIN
#include <wincodec.h>
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#endif

ImageUtils::ImageUtils(QObject *parent) : QObject(parent)
{
}

void ImageUtils::saveImageToFile(const QString &imageUrl, const QUrl &localPath)
{
    QNetworkAccessManager *manager = new QNetworkAccessManager(this);
    QNetworkRequest request((QUrl(imageUrl)));

    QNetworkReply *reply = manager->get(request);
    connect(reply, &QNetworkReply::finished, [=]()
            {
        if(reply->error() == QNetworkReply::NoError) {
            QString filePath = localPath.toLocalFile();
            QFile file(filePath);
            if(file.open(QIODevice::WriteOnly)) {
                file.write(reply->readAll());
                file.close();
                emit saveCompleted(true, filePath);
            } else {
                emit saveCompleted(false, "无法写入文件");
            }
        } else {
            emit saveCompleted(false, reply->errorString());
        }
        reply->deleteLater();
        manager->deleteLater(); });
}

QString ImageUtils::saveImageFromUrl(const QString &url)
{
    QNetworkAccessManager manager;
    QEventLoop loop;

    QNetworkRequest request(url);
    QNetworkReply *reply = manager.get(request);

    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    if (reply->error() != QNetworkReply::NoError)
    {
        reply->deleteLater();
        return QString();
    }

    QByteArray imageData = reply->readAll();
    reply->deleteLater();

    // 创建缓存目录
    QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/backgrounds";
    QDir().mkpath(cacheDir);

    // 生成文件名
    QString filePath = cacheDir + "/background.jpg";

    // 保存文件
    QFile file(filePath);
    if (file.open(QIODevice::WriteOnly))
    {
        file.write(imageData);
        file.close();
        return filePath;
    }

    return QString();
}

QString ImageUtils::fetchAndSaveRandomBackground(const QString &apiUrl)
{
    QNetworkAccessManager manager;
    
    // 最多重试3次（因为部分随机图片可能返回500）
    for (int attempt = 0; attempt < 3; attempt++) {
        QNetworkRequest request(apiUrl);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::NoLessSafeRedirectPolicy);
        
        QEventLoop loop;
        QNetworkReply *reply = manager.get(request);
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();

        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "fetchAndSaveRandomBackground: attempt" << (attempt + 1) 
                       << "failed:" << reply->errorString();
            reply->deleteLater();
            continue;
        }

        QByteArray imageData = reply->readAll();
        reply->deleteLater();

        if (imageData.isEmpty() || imageData.size() < 1024) {
            qWarning() << "fetchAndSaveRandomBackground: attempt" << (attempt + 1) 
                       << "got empty/tiny response";
            continue;
        }

        // 尝试用 QImage 加载（支持 jpg/png/gif 等 Qt 内置格式）
        QImage image;
        if (!image.loadFromData(imageData)) {
            // QImage 无法加载（可能是 WebP 等不支持的格式）
            // 尝试转换为 JPEG
            QByteArray jpegData = convertToJpeg(imageData);
            if (jpegData.isEmpty()) {
                qWarning() << "fetchAndSaveRandomBackground: attempt" << (attempt + 1) 
                           << "failed to decode image";
                continue;
            }
            imageData = jpegData;
            // 验证转换结果
            if (!image.loadFromData(imageData)) {
                qWarning() << "fetchAndSaveRandomBackground: converted data still unreadable";
                continue;
            }
        }

        // 创建缓存目录
        QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/backgrounds";
        QDir().mkpath(cacheDir);

        // 使用时间戳作为文件名
        QString filePath = cacheDir + "/background_" + QString::number(QDateTime::currentMSecsSinceEpoch()) + ".jpg";

        // 保存为 JPEG（确保 Qt Image 可以加载）
        if (image.save(filePath, "JPEG", 90)) {
            // 清理旧的背景图文件（保留最新的）
            QDir bgDir(cacheDir);
            QStringList filters;
            filters << "background_*.*";
            QFileInfoList oldFiles = bgDir.entryInfoList(filters, QDir::Files, QDir::Time);
            for (int i = 1; i < oldFiles.size(); i++) {
                QFile::remove(oldFiles[i].absoluteFilePath());
            }
            
            return filePath;
        }
    }
    
    qWarning() << "fetchAndSaveRandomBackground: all attempts failed";
    return QString();
}

QByteArray ImageUtils::convertToJpeg(const QByteArray &imageData)
{
    // 首先尝试 QImage（如果安装了 qtimageformats 模块，支持 WebP/TIFF等）
    QImage image;
    if (image.loadFromData(imageData)) {
        QByteArray result;
        QBuffer buffer(&result);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "JPEG", 90);
        return result;
    }

#ifdef Q_OS_WIN
    // Windows 后备方案：使用 WIC (Windows Imaging Component)
    // Windows 10+ 原生支持 WebP 解码
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    
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
    
    hr = stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(imageData.data())),
                                       imageData.size());
    if (FAILED(hr)) goto cleanup;
    
    hr = factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr)) goto cleanup;
    
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) goto cleanup;
    
    {
        UINT width, height;
        frame->GetSize(&width, &height);
        
        // 转换为 32bpp BGRA
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
        
        // 读取像素数据
        QByteArray pixels(width * height * 4, 0);
        hr = converter->CopyPixels(nullptr, width * 4, pixels.size(),
                                    reinterpret_cast<BYTE*>(pixels.data()));
        converter->Release();
        if (FAILED(hr)) goto cleanup;
        
        // 创建 QImage 并保存为 JPEG
        QImage wicImage(reinterpret_cast<const uchar*>(pixels.constData()),
                        width, height, width * 4, QImage::Format_ARGB32);
        
        QBuffer buffer(&result);
        buffer.open(QIODevice::WriteOnly);
        wicImage.save(&buffer, "JPEG", 90);
    }
    
cleanup:
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();
    
    return result;
#else
    // Linux/macOS: 需要安装 qtimageformats 模块以支持 WebP
    // Linux: sudo apt install qt6-image-formats-plugins (或对应包管理器)
    // macOS: brew install qt (通常已包含)
    qWarning() << "convertToJpeg: unsupported image format. Install qtimageformats for WebP support.";
    return QByteArray();
#endif
}

bool ImageUtils::fileExists(const QString &path)
{
    return QFileInfo::exists(path);
}

bool ImageUtils::isValidCache(const QString &cachePath)
{
    if (!fileExists(cachePath)) {
        return false;
    }

    QFileInfo fileInfo(cachePath);
    const qint64 twentyFourHours = 24 * 60 * 60;  // 24小时有效期
    
    // 检查文件时效性和有效性
    return fileInfo.lastModified().secsTo(QDateTime::currentDateTime()) < twentyFourHours &&
           fileInfo.size() > 1024 &&          // 文件至少1KB
           fileInfo.suffix().toLower() == "jpg"; // 验证文件格式
}

bool ImageUtils::validateExtension(const QString &filePath)
{
    static const QStringList allowedExtensions = {"jpg", "jpeg", "png", "bmp"};
    QFileInfo fileInfo(filePath);
    QString extension = fileInfo.suffix().toLower();
    
    return !extension.isEmpty() && allowedExtensions.contains(extension);
}