#include "ClipboardBridge.h"

#include <QBuffer>
#include <QByteArrayView>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <UIKit/UIKit.h>

namespace {
QString withUnixLineEndings(QString text)
{
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    return text;
}

bool isPasteboardAuthorizationError(const QString &text)
{
    return text.contains(QStringLiteral("PBErrorDomain"))
        && text.contains(QStringLiteral("Operation not authorized"));
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

QByteArray networkPngData(const QImage &sourceImage)
{
    constexpr qsizetype MaxImageBytes = 10 * 1024 * 1024;
    if (sourceImage.isNull())
        return {};

    QImage image = sourceImage;
    for (int attempt = 0; attempt < 20; ++attempt) {
        QByteArray pngData;
        QBuffer buffer(&pngData);
        if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG"))
            return {};
        if (pngData.size() <= MaxImageBytes)
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

bool pasteboardMayStillProvideImage(UIPasteboard *pasteboard)
{
    if (pasteboard == nil)
        return false;
    if (pasteboard.hasImages)
        return true;

    NSMutableOrderedSet<NSString *> *types = [NSMutableOrderedSet orderedSet];
    for (NSString *type in pasteboard.pasteboardTypes)
        [types addObject:type];
    for (NSDictionary *item in pasteboard.items) {
        if (![item isKindOfClass:[NSDictionary class]])
            continue;
        for (NSString *type in item.allKeys)
            [types addObject:type];
    }

    for (NSString *type in types) {
        UTType *utType = [UTType typeWithIdentifier:type];
        if (utType == nil)
            continue;
        if ([utType conformsToType:UTTypeImage]
            || [utType conformsToType:UTTypePNG]
            || [utType conformsToType:UTTypeJPEG]
            || [utType conformsToType:UTTypeURL]
            || [utType conformsToType:UTTypeText]
            || [utType conformsToType:UTTypeData]) {
            return true;
        }
    }
    return false;
}

bool canAccessPasteboard()
{
    const UIApplicationState state = UIApplication.sharedApplication.applicationState;
    return state == UIApplicationStateActive;
}

UIPasteboard *availablePasteboard()
{
    if (!canAccessPasteboard())
        return nil;
    return UIPasteboard.generalPasteboard;
}

QImage imageFromUiImage(UIImage *image);
QImage imageFromData(NSData *data);

QImage imageFromUrl(NSURL *url)
{
    if (url == nil)
        return {};

    if (url.isFileURL)
        return imageFromData([NSData dataWithContentsOfURL:url]);
    return {};
}

QImage imageFromString(NSString *value)
{
    const QString text = value == nil ? QString{} : QString::fromUtf8(value.UTF8String).trimmed();
    if (text.isEmpty())
        return {};

    if (text.startsWith(QStringLiteral("data:image/"), Qt::CaseInsensitive)) {
        const qsizetype comma = text.indexOf(QLatin1Char(','));
        if (comma > 0 && text.left(comma).contains(QStringLiteral(";base64"), Qt::CaseInsensitive))
            return QImage::fromData(QByteArray::fromBase64(text.mid(comma + 1).toLatin1()));
    }

    return {};
}

QImage imageFromData(NSData *data)
{
    if (data == nil || data.length == 0)
        return {};

    const QByteArray bytes(
        reinterpret_cast<const char *>(data.bytes),
        static_cast<qsizetype>(data.length));
    QImage image = QImage::fromData(bytes);
    if (!image.isNull())
        return image;

    // Safari and Photos can place HEIC/WebKit-backed image data on the
    // pasteboard. Qt may not decode every native iOS image flavor, but UIKit can.
    return imageFromUiImage([UIImage imageWithData:data]);
}

QImage imageFromUiImage(UIImage *image)
{
    if (image == nil)
        return {};

    NSData *pngData = UIImagePNGRepresentation(image);
    if (pngData == nil || pngData.length == 0)
        return {};
    const QByteArray bytes(
        reinterpret_cast<const char *>(pngData.bytes),
        static_cast<qsizetype>(pngData.length));
    return QImage::fromData(bytes, "PNG");
}

QImage pasteboardImage()
{
    UIPasteboard *pasteboard = availablePasteboard();
    if (pasteboard == nil)
        return {};

    QImage image = imageFromUiImage(pasteboard.image);
    if (!image.isNull())
        return image;

    for (UIImage *uiImage in pasteboard.images) {
        image = imageFromUiImage(uiImage);
        if (!image.isNull())
            return image;
    }

    static NSArray<NSString *> *preferredTypes = @[
        @"public.png",
        @"public.jpeg",
        @"public.jpg",
        @"public.heic",
        @"public.heif",
        @"public.tiff",
        @"com.compuserve.gif",
        @"public.image"
    ];

    NSMutableOrderedSet<NSString *> *types = [NSMutableOrderedSet orderedSet];
    for (NSString *type in preferredTypes)
        [types addObject:type];
    for (NSString *type in pasteboard.pasteboardTypes)
        [types addObject:type];
    for (NSDictionary *item in pasteboard.items) {
        if (![item isKindOfClass:[NSDictionary class]])
            continue;
        for (NSString *type in item.allKeys)
            [types addObject:type];
    }

    for (NSDictionary *item in pasteboard.items) {
        if (![item isKindOfClass:[NSDictionary class]])
            continue;

        for (NSString *type in types) {
            id value = item[type];
            if ([value isKindOfClass:[UIImage class]]) {
                image = imageFromUiImage(static_cast<UIImage *>(value));
            } else if ([value isKindOfClass:[NSData class]]) {
                image = imageFromData(static_cast<NSData *>(value));
            } else if ([value isKindOfClass:[NSURL class]]) {
                image = imageFromUrl(static_cast<NSURL *>(value));
            } else if ([value isKindOfClass:[NSString class]]) {
                image = imageFromString(static_cast<NSString *>(value));
            } else {
                image = {};
            }

            if (!image.isNull())
                return image;
        }
    }

    for (NSString *type in types) {
        UTType *utType = [UTType typeWithIdentifier:type];
        const bool isImageType = utType == nil
            || [utType conformsToType:UTTypeImage]
            || [utType conformsToType:UTTypePNG]
            || [utType conformsToType:UTTypeJPEG];

        if (isImageType) {
            image = imageFromData([pasteboard dataForPasteboardType:type]);
            if (!image.isNull())
                return image;
        } else {
            continue;
        }

        NSArray *dataItems = [pasteboard dataForPasteboardType:type inItemSet:nil];
        for (NSData *data in dataItems) {
            if (isImageType) {
                image = imageFromData(data);
                if (!image.isNull())
                    return image;
            }
        }

        NSArray *values = [pasteboard valuesForPasteboardType:type inItemSet:nil];
        for (id value in values) {
            if ([value isKindOfClass:[UIImage class]]) {
                image = imageFromUiImage(static_cast<UIImage *>(value));
            } else if ([value isKindOfClass:[NSData class]]) {
                image = imageFromData(static_cast<NSData *>(value));
            } else if ([value isKindOfClass:[NSURL class]]) {
                image = imageFromUrl(static_cast<NSURL *>(value));
            } else if ([value isKindOfClass:[NSString class]]) {
                image = imageFromString(static_cast<NSString *>(value));
            } else {
                image = {};
            }

            if (!image.isNull())
                return image;
        }
    }

    return {};
}

constexpr auto PasteSettingsOfferSeenKey = "pasteSettingsOfferSeen";
}

ClipboardBridge::ClipboardBridge(QObject *parent)
    : QObject(parent)
{
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, [this]() {
        m_cachedPasteboardChangeCount = -1;
        emit textChanged();
        emit clipboardChanged();
    });
}

bool ClipboardBridge::shouldOfferPasteSettings() const
{
    QSettings settings;
    return !settings.value(QLatin1String(PasteSettingsOfferSeenKey), false).toBool();
}

qint64 ClipboardBridge::pasteboardChangeCount() const
{
    if (!canAccessPasteboard())
        return -1;

    return UIPasteboard.generalPasteboard.changeCount;
}

QString ClipboardBridge::text() const
{
    const QString text = withUnixLineEndings(QGuiApplication::clipboard()->text());
    return isPasteboardAuthorizationError(text) ? QString{} : text;
}

void ClipboardBridge::setText(const QString &text)
{
    QGuiApplication::clipboard()->setText(withUnixLineEndings(text));
}

bool ClipboardBridge::hasImage() const
{
    if (!canAccessPasteboard())
        return false;
    updateImageCache();
    return !m_cachedImage.isNull();
}

QString ClipboardBridge::imageFingerprint() const
{
    if (!canAccessPasteboard())
        return {};
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
    if (!canAccessPasteboard())
        return {};
    updateImageCache();
    return m_cachedImageBase64;
}

bool ClipboardBridge::setImageBase64(const QString &base64)
{
    UIPasteboard *pasteboard = availablePasteboard();
    if (pasteboard == nil)
        return false;

    const QByteArray pngData = QByteArray::fromBase64(base64.toLatin1());
    const QImage image = QImage::fromData(pngData, "PNG");
    if (image.isNull())
        return false;

    NSData *data = [NSData dataWithBytes:pngData.constData()
                                  length:static_cast<NSUInteger>(pngData.size())];
    if (data == nil || data.length == 0)
        return false;

    pasteboard.items = @[@{ UTTypePNG.identifier : data }];
    m_cachedPasteboardChangeCount = pasteboard.changeCount;
    m_cachedImage = image;
    m_cachedImageFingerprint = QString::fromLatin1(imageFingerprintBytes(image).toHex());
    m_cachedImageBase64 = base64;
    emit clipboardChanged();
    return true;
}

QString ClipboardBridge::setPreviewImageBase64(const QString &base64)
{
    const QByteArray pngData = QByteArray::fromBase64(base64.toLatin1());
    const QImage image = QImage::fromData(pngData, "PNG");
    if (image.isNull()) {
        m_previewImage = {};
        ++m_previewImageRevision;
        return {};
    }

    m_previewImage = image;
    ++m_previewImageRevision;

    QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (cacheDir.isEmpty())
        cacheDir = QDir::tempPath();
    QDir().mkpath(cacheDir);

    const QString path = QDir(cacheDir).filePath(
        QStringLiteral("network-clipboard-preview-%1.png").arg(m_previewImageRevision));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return {};
    if (file.write(pngData) != pngData.size())
        return {};

    return QUrl::fromLocalFile(path).toString();
}

void ClipboardBridge::clearPreviewImage()
{
    if (m_previewImage.isNull())
        return;

    m_previewImage = {};
    ++m_previewImageRevision;
}

QImage ClipboardBridge::previewImage() const
{
    return m_previewImage;
}

void ClipboardBridge::markPasteSettingsOfferSeen()
{
    if (!shouldOfferPasteSettings())
        return;

    QSettings settings;
    settings.setValue(QLatin1String(PasteSettingsOfferSeenKey), true);
    emit pasteSettingsOfferChanged();
}

void ClipboardBridge::openAppSettings() const
{
    QDesktopServices::openUrl(QUrl(QStringLiteral("app-settings:")));
}

void ClipboardBridge::updateImageCache() const
{
    UIPasteboard *pasteboard = availablePasteboard();
    if (pasteboard == nil)
        return;

    const qint64 changeCount = pasteboard.changeCount;
    if (m_cachedPasteboardChangeCount == changeCount)
        return;

    m_cachedImage = pasteboardImage();
    m_cachedImageFingerprint.clear();
    m_cachedImageBase64.clear();
    if (m_cachedImage.isNull()) {
        // Photos may publish an item provider before the actual image data is
        // available. Safari may also report only a transient text/provider
        // representation while the user is switching back to the app. Do not
        // cache those negative reads; let the QML retry timer ask again.
        const bool mayStillProvideImage = pasteboardMayStillProvideImage(pasteboard);
        if (!mayStillProvideImage)
            m_cachedPasteboardChangeCount = changeCount;
        return;
    }

    m_cachedPasteboardChangeCount = changeCount;
    m_cachedImageFingerprint = QString::fromLatin1(imageFingerprintBytes(m_cachedImage).toHex());
    const QByteArray pngData = networkPngData(m_cachedImage);
    if (!pngData.isEmpty())
        m_cachedImageBase64 = QString::fromLatin1(pngData.toBase64());
}
