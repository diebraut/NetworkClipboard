#include "ClipboardBridge.h"

#include <QBuffer>
#include <QByteArrayView>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMimeData>
#include <QPointer>
#include <QStandardPaths>
#include <QTextDocument>
#include <QThreadPool>
#include <QTimer>
#include <QVariantMap>
#include <QVector>

#include <utility>

#ifdef Q_OS_ANDROID
#include <QJniObject>
#include <QtCore/qcoreapplication_platform.h>
#endif

namespace {
QString withUnixLineEndings(QString text)
{
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    return text;
}

QString clipboardPlainText()
{
    const QMimeData *mimeData = QGuiApplication::clipboard()->mimeData();
    if (!mimeData)
        return {};

    if (mimeData->hasHtml()) {
        QTextDocument document;
        document.setHtml(mimeData->html());
        const QString text = document.toPlainText();
        if (!text.trimmed().isEmpty())
            return text;
    }

    return mimeData->text();
}

QByteArray imageFingerprintBytes(const QImage &image)
{
    if (image.isNull())
        return {};

    const QImage normalized = image.convertToFormat(QImage::Format_RGBA8888);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArray::number(normalized.width()));
    hash.addData("x");
    hash.addData(QByteArray::number(normalized.height()));
    for (int row = 0; row < normalized.height(); ++row) {
        hash.addData(QByteArrayView(
            reinterpret_cast<const char *>(normalized.constScanLine(row)),
            normalized.width() * 4));
    }
    return hash.result();
}

QString imageVisualFingerprint(const QImage &image)
{
    if (image.isNull())
        return {};

    const QImage sample = image.scaled(16,
                                       16,
                              Qt::IgnoreAspectRatio,
                              Qt::SmoothTransformation)
                              .convertToFormat(QImage::Format_ARGB32);
    if (sample.isNull())
        return {};

    QVector<int> luminanceValues;
    luminanceValues.reserve(sample.width() * sample.height());
    int luminanceSum = 0;
    for (int y = 0; y < sample.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(sample.constScanLine(y));
        for (int x = 0; x < sample.width(); ++x) {
            const QRgb pixel = line[x];
            const int alpha = qAlpha(pixel);
            const int red = (qRed(pixel) * alpha + 255 * (255 - alpha)) / 255;
            const int green = (qGreen(pixel) * alpha + 255 * (255 - alpha)) / 255;
            const int blue = (qBlue(pixel) * alpha + 255 * (255 - alpha)) / 255;
            const int luminance = (red * 299 + green * 587 + blue * 114) / 1000;
            luminanceValues.push_back(luminance);
            luminanceSum += luminance;
        }
    }

    const int average = luminanceValues.isEmpty() ? 0 : luminanceSum / luminanceValues.size();
    QByteArray bits;
    bits.reserve((luminanceValues.size() + 7) / 8);
    for (int i = 0; i < luminanceValues.size(); i += 8) {
        uchar byte = 0;
        for (int bit = 0; bit < 8 && i + bit < luminanceValues.size(); ++bit) {
            if (luminanceValues.at(i + bit) >= average)
                byte |= uchar(1u << bit);
        }
        bits.append(char(byte));
    }
    return QString::fromLatin1(bits.toHex());
}

QByteArray networkPngData(const QImage &sourceImage, qsizetype maxImageBytes = 10 * 1024 * 1024)
{
    if (sourceImage.isNull())
        return {};

    QImage image = sourceImage;
    for (int attempt = 0; attempt < 10; ++attempt) {
        QByteArray pngData;
        QBuffer buffer(&pngData);
        if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG"))
            return {};
        if (pngData.size() <= maxImageBytes)
            return pngData;

        const int nextWidth = qMax(1, qRound(image.width() * 0.8));
        const int nextHeight = qMax(1, qRound(image.height() * 0.8));
        if (nextWidth == image.width() && nextHeight == image.height())
            break;
        image = image.scaled(nextWidth,
                             nextHeight,
                             Qt::KeepAspectRatio,
                             Qt::SmoothTransformation);
    }
    return {};
}

QString historyImageDirPath()
{
    const QString basePath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir dir(basePath);
    dir.mkpath(QStringLiteral("history-images"));
    return dir.filePath(QStringLiteral("history-images"));
}

#ifdef Q_OS_ANDROID
QJniObject androidContext()
{
    return QNativeInterface::QAndroidApplication::context();
}

QString androidClipboardImageBase64()
{
    const QJniObject context = androidContext();
    if (!context.isValid())
        return {};

    const QJniObject result = QJniObject::callStaticObjectMethod(
        "org/qtproject/example/NetworkClipboardAndroid/ImageClipboardHelper",
        "imageBase64",
        "(Landroid/content/Context;)Ljava/lang/String;",
        context.object());
    return result.toString();
}

#endif
}

ClipboardBridge::ClipboardBridge(QObject *parent)
    : QObject(parent)
{
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, &ClipboardBridge::textChanged);
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, &ClipboardBridge::clipboardChanged);
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, &ClipboardBridge::invalidateImageCache);
}

QString ClipboardBridge::text() const
{
    return withUnixLineEndings(clipboardPlainText());
}

void ClipboardBridge::setText(const QString &text)
{
    QGuiApplication::clipboard()->setText(withUnixLineEndings(text));
}

bool ClipboardBridge::hasImage() const
{
#ifdef Q_OS_ANDROID
    const QJniObject context = androidContext();
    if (!context.isValid())
        return false;

    const QJniObject result = QJniObject::callStaticObjectMethod(
        "org/qtproject/example/NetworkClipboardAndroid/ImageClipboardHelper",
        "imageKey",
        "(Landroid/content/Context;)Ljava/lang/String;",
        context.object());
    const QString key = result.toString();
    if (key != m_cachedImageKey) {
        m_imageCacheValid = false;
        m_cachedImage = {};
        m_cachedImageKey = key;
        m_cachedImageFingerprint.clear();
        m_cachedImageBase64.clear();
    }
    return !key.isEmpty();
#else
    updateImageCache();
    return !m_cachedImage.isNull();
#endif
}

QString ClipboardBridge::imageFingerprint() const
{
    updateImageCache();
    return m_cachedImageFingerprint;
}

QString ClipboardBridge::imageFingerprintFromBase64(const QString &base64) const
{
    const QImage image = QImage::fromData(QByteArray::fromBase64(base64.toLatin1()), "PNG");
    if (image.isNull())
        return {};
    return QString::fromLatin1(imageFingerprintBytes(image).toHex());
}

QString ClipboardBridge::imageVisualFingerprintFromBase64(const QString &base64) const
{
    const QImage image = QImage::fromData(QByteArray::fromBase64(base64.toLatin1()), "PNG");
    return imageVisualFingerprint(image);
}

QString ClipboardBridge::thumbnailBase64FromBase64(const QString &base64, int maxSize) const
{
    const QImage image = QImage::fromData(QByteArray::fromBase64(base64.toLatin1()), "PNG");
    if (image.isNull())
        return {};

    const int boundedMaxSize = qBound(24, maxSize, 1024);
    const QImage thumbnail = image.scaled(boundedMaxSize,
                                          boundedMaxSize,
                                          Qt::KeepAspectRatio,
                                          Qt::SmoothTransformation);
    QByteArray pngData;
    QBuffer buffer(&pngData);
    if (!buffer.open(QIODevice::WriteOnly) || !thumbnail.save(&buffer, "PNG"))
        return {};
    return QString::fromLatin1(pngData.toBase64());
}

bool ClipboardBridge::imageHasMeaningfulContentBase64(const QString &base64) const
{
    const QImage image = QImage::fromData(QByteArray::fromBase64(base64.toLatin1()), "PNG");
    if (image.isNull())
        return false;

    const QImage sample = image.scaled(32,
                                       32,
                                       Qt::KeepAspectRatio,
                                       Qt::SmoothTransformation)
                              .convertToFormat(QImage::Format_RGBA8888);
    if (sample.isNull())
        return false;

    int visiblePixels = 0;
    int minR = 255;
    int minG = 255;
    int minB = 255;
    int maxR = 0;
    int maxG = 0;
    int maxB = 0;
    qint64 luminanceSum = 0;
    qint64 luminanceSquaredSum = 0;

    for (int y = 0; y < sample.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(sample.constScanLine(y));
        for (int x = 0; x < sample.width(); ++x) {
            const QRgb pixel = line[x];
            if (qAlpha(pixel) <= 8)
                continue;

            const int red = qRed(pixel);
            const int green = qGreen(pixel);
            const int blue = qBlue(pixel);
            const int luminance = (red * 299 + green * 587 + blue * 114) / 1000;

            minR = qMin(minR, red);
            minG = qMin(minG, green);
            minB = qMin(minB, blue);
            maxR = qMax(maxR, red);
            maxG = qMax(maxG, green);
            maxB = qMax(maxB, blue);
            luminanceSum += luminance;
            luminanceSquaredSum += qint64(luminance) * luminance;
            ++visiblePixels;
        }
    }

    if (visiblePixels < qMax(1, sample.width() * sample.height() / 20))
        return false;

    const int colorRange = qMax(qMax(maxR - minR, maxG - minG), maxB - minB);
    const qint64 varianceNumerator = luminanceSquaredSum * visiblePixels
                                     - luminanceSum * luminanceSum;
    const qint64 lowVarianceLimit = qint64(12) * 12 * visiblePixels * visiblePixels;
    return !(varianceNumerator < lowVarianceLimit && colorRange < 80);
}

QString ClipboardBridge::saveHistoryImageBase64(const QString &base64) const
{
    if (base64.isEmpty())
        return {};

    const QString imageId = QString::fromLatin1(
        QCryptographicHash::hash(base64.toLatin1(), QCryptographicHash::Sha256).toHex());
    const QString path = QDir(historyImageDirPath()).filePath(imageId + QStringLiteral(".b64"));
    if (QFile::exists(path))
        return imageId;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return {};
    file.write(base64.toLatin1());
    return imageId;
}

QString ClipboardBridge::loadHistoryImageBase64(const QString &imageId) const
{
    if (imageId.isEmpty())
        return {};

    QFile file(QDir(historyImageDirPath()).filePath(imageId + QStringLiteral(".b64")));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromLatin1(file.readAll());
}

QString ClipboardBridge::imageBase64() const
{
    updateImageCache();
    return m_cachedImageBase64;
}

bool ClipboardBridge::imageFromNetworkClipboard() const
{
#ifdef Q_OS_ANDROID
    const QJniObject context = androidContext();
    if (!context.isValid())
        return false;

    return QJniObject::callStaticMethod<jboolean>(
        "org/qtproject/example/NetworkClipboardAndroid/ImageClipboardHelper",
        "isNetworkClipboardImage",
        "(Landroid/content/Context;)Z",
        context.object());
#else
    return false;
#endif
}

bool ClipboardBridge::setImageBase64(const QString &base64)
{
#ifdef Q_OS_ANDROID
    const QJniObject context = androidContext();
    const QJniObject encoded = QJniObject::fromString(base64);
    if (!context.isValid()
        || !QJniObject::callStaticMethod<jboolean>(
            "org/qtproject/example/NetworkClipboardAndroid/ImageClipboardHelper",
            "setImageBase64",
            "(Landroid/content/Context;Ljava/lang/String;)Z",
            context.object(),
            encoded.object<jstring>())) {
        return false;
    }

    m_imageCacheValid = true;
    m_cachedImage = {};
    m_cachedImageKey.clear();
    m_cachedImageFingerprint.clear();
    m_cachedImageBase64 = base64;
#else
    const QImage image = QImage::fromData(QByteArray::fromBase64(base64.toLatin1()), "PNG");
    if (image.isNull())
        return false;
    QGuiApplication::clipboard()->setImage(image);
#endif
    return true;
}

void ClipboardBridge::loadRecentPhotos(int maxCount)
{
#ifdef Q_OS_ANDROID
    const QJniObject context = androidContext();
    if (!context.isValid()) {
        emit recentPhotosLoaded(QVariantList{});
        return;
    }

    const QJniObject json = QJniObject::callStaticObjectMethod(
        "org/qtproject/example/NetworkClipboardAndroid/ImageClipboardHelper",
        "recentPhotosJson",
        "(Landroid/content/Context;I)Ljava/lang/String;",
        context.object(),
        jint(qBound(1, maxCount, 80)));
    const QJsonDocument document = QJsonDocument::fromJson(json.toString().toUtf8());
    QVariantList photos;
    if (document.isArray()) {
        const QJsonArray array = document.array();
        photos.reserve(array.size());
        for (const QJsonValue &value : array) {
            if (!value.isObject())
                continue;
            const QJsonObject object = value.toObject();
            QVariantMap item;
            item.insert(QStringLiteral("id"), object.value(QStringLiteral("id")).toString());
            item.insert(QStringLiteral("thumbnail"), object.value(QStringLiteral("thumbnail")).toString());
            item.insert(QStringLiteral("createdAtMs"),
                        qint64(object.value(QStringLiteral("createdAtMs")).toDouble()));
            photos.push_back(item);
        }
    }
    emit recentPhotosLoaded(photos);
#else
    Q_UNUSED(maxCount);
    emit recentPhotosLoaded(QVariantList{});
#endif
}

void ClipboardBridge::loadPhotoContent(const QString &assetId)
{
#ifdef Q_OS_ANDROID
    const QPointer<ClipboardBridge> bridge(this);
    QThreadPool::globalInstance()->start([bridge, assetId]() {
        const QJniObject context = androidContext();
        const QJniObject uri = QJniObject::fromString(assetId);
        QString content;
        if (context.isValid()) {
            const QJniObject base64 = QJniObject::callStaticObjectMethod(
                "org/qtproject/example/NetworkClipboardAndroid/ImageClipboardHelper",
                "photoContentBase64",
                "(Landroid/content/Context;Ljava/lang/String;)Ljava/lang/String;",
                context.object(),
                uri.object<jstring>());
            content = base64.toString();
        }

        if (!bridge)
            return;
        QMetaObject::invokeMethod(bridge, [bridge, assetId, content]() {
            if (bridge)
                emit bridge->photoContentLoaded(assetId, content);
        }, Qt::QueuedConnection);
    });
#else
    emit photoContentLoaded(assetId, QString{});
#endif
}

void ClipboardBridge::openCamera()
{
#ifdef Q_OS_ANDROID
    const QJniObject context = androidContext();
    if (!context.isValid()) {
        emit cameraCaptureFailed(QStringLiteral("Kamera konnte nicht geöffnet werden."));
        return;
    }

    const bool hasCameraApp = QJniObject::callStaticMethod<jboolean>(
        "org/qtproject/example/NetworkClipboardAndroid/ImageClipboardHelper",
        "hasCameraApp",
        "(Landroid/content/Context;)Z",
        context.object());
    if (!hasCameraApp) {
        emit cameraCaptureFailed(QStringLiteral("Kamera ist auf diesem Gerät nicht verfügbar."));
        return;
    }

    const QJniObject captureUri = QJniObject::callStaticObjectMethod(
        "org/qtproject/example/NetworkClipboardAndroid/ImageClipboardHelper",
        "createCameraCaptureUri",
        "(Landroid/content/Context;)Ljava/lang/String;",
        context.object());
    const QString uriText = captureUri.toString();
    if (uriText.isEmpty()) {
        emit cameraCaptureFailed(QStringLiteral("Kamera konnte nicht geöffnet werden."));
        return;
    }

    m_cameraCapturePending = true;
    context.callMethod<void>("startCameraCapture", "(Ljava/lang/String;)V",
                             QJniObject::fromString(uriText).object<jstring>());
#else
    emit cameraCaptureFailed(QStringLiteral("Kamera ist auf diesem Gerät nicht verfügbar."));
#endif
}

void ClipboardBridge::finishPendingCameraCapture()
{
#ifdef Q_OS_ANDROID
    if (!m_cameraCapturePending)
        return;

    const QString result = QJniObject::callStaticObjectMethod(
        "org/qtproject/example/NetworkClipboardAndroid/NetworkClipboardActivity",
        "takeCameraCaptureResult",
        "()Ljava/lang/String;").toString();
    if (result.isEmpty() || result == QStringLiteral("pending")) {
        QTimer::singleShot(100, this, &ClipboardBridge::finishPendingCameraCapture);
        return;
    }

    m_cameraCapturePending = false;
    if (result == QStringLiteral("cancel")) {
        emit cameraCaptureFailed(QString{});
        return;
    }

    const QString prefix = QStringLiteral("ok:");
    if (!result.startsWith(prefix)) {
        emit cameraCaptureFailed(QStringLiteral("Kamerabild konnte nicht verarbeitet werden."));
        return;
    }

    emit cameraImageProcessingStarted();
    const QString uriText = result.mid(prefix.size());
    const QPointer<ClipboardBridge> bridge(this);
    QThreadPool::globalInstance()->start([bridge, uriText]() {
        const QJniObject context = androidContext();
        const QJniObject uri = QJniObject::fromString(uriText);
        QString content;
        if (context.isValid()) {
            const QJniObject base64 = QJniObject::callStaticObjectMethod(
                "org/qtproject/example/NetworkClipboardAndroid/ImageClipboardHelper",
                "cameraContentBase64",
                "(Landroid/content/Context;Ljava/lang/String;)Ljava/lang/String;",
                context.object(),
                uri.object<jstring>());
            content = base64.toString();
        }

        if (!bridge)
            return;
        QMetaObject::invokeMethod(bridge, [bridge, content]() {
            if (!bridge)
                return;
            if (content.isEmpty())
                emit bridge->cameraCaptureFailed(
                    QStringLiteral("Kamerabild konnte nicht verarbeitet werden."));
            else
                emit bridge->cameraImageCaptured(content);
        }, Qt::QueuedConnection);
    });
#endif
}

void ClipboardBridge::invalidateImageCache()
{
    m_imageCacheValid = false;
    m_cachedImage = {};
    m_cachedImageKey.clear();
    m_cachedImageFingerprint.clear();
    m_cachedImageBase64.clear();
}

void ClipboardBridge::updateImageCache() const
{
    if (m_imageCacheValid)
        return;

#ifdef Q_OS_ANDROID
    const QString sourceBase64 = androidClipboardImageBase64();
    if (sourceBase64.isEmpty())
        return;
    m_cachedImage = QImage::fromData(QByteArray::fromBase64(sourceBase64.toLatin1()));
#else
    m_cachedImage = QGuiApplication::clipboard()->image();
#endif
    if (m_cachedImage.isNull())
        return;

    m_imageCacheValid = true;
    m_cachedImageFingerprint = QString::fromLatin1(imageFingerprintBytes(m_cachedImage).toHex());
    const QByteArray pngData = networkPngData(m_cachedImage);
    if (!pngData.isEmpty())
        m_cachedImageBase64 = QString::fromLatin1(pngData.toBase64());
}
