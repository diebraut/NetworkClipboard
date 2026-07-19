#include "ClipboardBridge.h"

#include <QBuffer>
#include <QByteArrayView>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QDebug>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>
#include <QVariantMap>
#include <QVector>

#include <limits>

#import <Photos/Photos.h>
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
    for (int attempt = 0; attempt < 20; ++attempt) {
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

QByteArray photoClipboardPngData(const QImage &sourceImage)
{
    constexpr qsizetype MaxPhotoBytes = 500 * 1024;
    if (sourceImage.isNull()) {
        qWarning() << "NC photo encode skipped: source image is null";
        return {};
    }

    QImage image = sourceImage;
    const int longestSide = qMax(image.width(), image.height());
    if (longestSide > 1200) {
        image = image.scaled(1200,
                             1200,
                             Qt::KeepAspectRatio,
                             Qt::SmoothTransformation);
    }

    return networkPngData(image, MaxPhotoBytes);
}

QString historyImageDirPath()
{
    const QString basePath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir dir(basePath);
    dir.mkpath(QStringLiteral("history-images"));
    return dir.filePath(QStringLiteral("history-images"));
}

QString pngBase64FromImage(const QImage &image)
{
    if (image.isNull())
        return {};

    QByteArray pngData;
    QBuffer buffer(&pngData);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG"))
        return {};
    return QString::fromLatin1(pngData.toBase64());
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

QImage imageFromCgImage(CGImageRef cgImage)
{
    if (cgImage == nil)
        return {};

    const size_t width = CGImageGetWidth(cgImage);
    const size_t height = CGImageGetHeight(cgImage);
    if (width == 0 || height == 0
        || width > static_cast<size_t>(std::numeric_limits<int>::max())
        || height > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return {};
    }

    QImage image(static_cast<int>(width), static_cast<int>(height), QImage::Format_RGBA8888);
    if (image.isNull())
        return {};

    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    CGContextRef context = CGBitmapContextCreate(image.bits(),
                                                 width,
                                                 height,
                                                 8,
                                                 static_cast<size_t>(image.bytesPerLine()),
                                                 colorSpace,
                                                 kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(colorSpace);
    if (context == nil)
        return {};

    CGContextDrawImage(context, CGRectMake(0, 0, width, height), cgImage);
    CGContextRelease(context);
    return image;
}

QImage imageFromDrawnUiImage(UIImage *image)
{
    if (image == nil || image.size.width <= 0 || image.size.height <= 0)
        return {};

    UIGraphicsBeginImageContextWithOptions(image.size, NO, 1.0);
    [image drawInRect:CGRectMake(0, 0, image.size.width, image.size.height)];
    UIImage *drawnImage = UIGraphicsGetImageFromCurrentImageContext();
    CGImageRef cgImage = drawnImage.CGImage;
    const QImage qimage = imageFromCgImage(cgImage);
    UIGraphicsEndImageContext();
    return qimage;
}

UIViewController *topViewController()
{
    UIWindow *keyWindow = nil;
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
        if (scene.activationState != UISceneActivationStateForegroundActive
            || ![scene isKindOfClass:UIWindowScene.class]) {
            continue;
        }
        UIWindowScene *windowScene = static_cast<UIWindowScene *>(scene);
        for (UIWindow *window in windowScene.windows) {
            if (window.isKeyWindow) {
                keyWindow = window;
                break;
            }
        }
        if (keyWindow != nil)
            break;
    }
    if (keyWindow == nil)
        keyWindow = UIApplication.sharedApplication.keyWindow;

    UIViewController *controller = keyWindow.rootViewController;
    while (controller.presentedViewController != nil)
        controller = controller.presentedViewController;
    return controller;
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

@interface NetworkClipboardCameraDelegate : NSObject <UIImagePickerControllerDelegate, UINavigationControllerDelegate>
- (instancetype)initWithBridge:(ClipboardBridge *)bridge;
@end

@implementation NetworkClipboardCameraDelegate {
    ClipboardBridge *m_bridge;
}

- (instancetype)initWithBridge:(ClipboardBridge *)bridge
{
    self = [super init];
    if (self)
        m_bridge = bridge;
    return self;
}

- (void)imagePickerController:(UIImagePickerController *)picker
didFinishPickingMediaWithInfo:(NSDictionary<UIImagePickerControllerInfoKey, id> *)info
{
    UIImage *image = info[UIImagePickerControllerOriginalImage];
    [picker dismissViewControllerAnimated:YES completion:^{
        ClipboardBridge *bridge = m_bridge;
        QMetaObject::invokeMethod(bridge, [bridge]() {
            emit bridge->cameraImageProcessingStarted();
        }, Qt::QueuedConnection);

        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            const QImage qimage = imageFromDrawnUiImage(image);
            const QByteArray contentData = photoClipboardPngData(qimage);
            const QString contentBase64 = contentData.isEmpty()
                ? QString{}
                : QString::fromLatin1(contentData.toBase64());
            QMetaObject::invokeMethod(bridge, [bridge, contentBase64]() {
                if (contentBase64.isEmpty())
                    emit bridge->cameraCaptureFailed(QStringLiteral("Kamerabild konnte nicht verarbeitet werden."));
                else
                    emit bridge->cameraImageCaptured(contentBase64);
            }, Qt::QueuedConnection);
        });
    }];
}

- (void)imagePickerControllerDidCancel:(UIImagePickerController *)picker
{
    [picker dismissViewControllerAnimated:YES completion:^{
        QMetaObject::invokeMethod(m_bridge, [bridge = m_bridge]() {
            emit bridge->cameraCaptureFailed(QString{});
        }, Qt::QueuedConnection);
    }];
}

@end

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
    const QByteArray sourcePngData = QByteArray::fromBase64(base64.toLatin1());
    const QImage image = QImage::fromData(sourcePngData, "PNG");
    if (image.isNull()) {
        qWarning() << "NC image fingerprint failed"
                   << "base64Len" << base64.length()
                   << "bytes" << sourcePngData.size();
        return {};
    }
    return QString::fromLatin1(imageFingerprintBytes(image).toHex());
}

QString ClipboardBridge::imageVisualFingerprintFromBase64(const QString &base64) const
{
    const QImage image = QImage::fromData(QByteArray::fromBase64(base64.toLatin1()), "PNG");
    return imageVisualFingerprint(image);
}

QString ClipboardBridge::thumbnailBase64FromBase64(const QString &base64, int maxSize) const
{
    const QByteArray sourcePngData = QByteArray::fromBase64(base64.toLatin1());
    const QImage image = QImage::fromData(sourcePngData, "PNG");
    if (image.isNull()) {
        qWarning() << "NC thumbnail failed: decode"
                   << "base64Len" << base64.length()
                   << "bytes" << sourcePngData.size()
                   << "maxSize" << maxSize;
        return {};
    }

    const int boundedMaxSize = qBound(24, maxSize, 1024);
    const QImage thumbnail = image.scaled(boundedMaxSize,
                                          boundedMaxSize,
                                          Qt::KeepAspectRatio,
                                          Qt::SmoothTransformation);
    QByteArray thumbnailPngData;
    QBuffer buffer(&thumbnailPngData);
    if (!buffer.open(QIODevice::WriteOnly) || !thumbnail.save(&buffer, "PNG")) {
        qWarning() << "NC thumbnail failed: encode"
                   << "sourceSize" << image.size()
                   << "thumbSize" << thumbnail.size()
                   << "maxSize" << maxSize;
        return {};
    }
    return QString::fromLatin1(thumbnailPngData.toBase64());
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
        qWarning() << "NC preview file fallback failed: decode"
                   << "base64Len" << base64.length()
                   << "bytes" << pngData.size();
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
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "NC preview file fallback failed: open" << path << file.errorString();
        return {};
    }
    if (file.write(pngData) != pngData.size()) {
        qWarning() << "NC preview file fallback failed: write" << path << file.errorString();
        return {};
    }

    const QString url = QUrl::fromLocalFile(path).toString();
    return url;
}

void ClipboardBridge::clearPreviewImage()
{
    if (m_previewImage.isNull())
        return;

    m_previewImage = {};
    ++m_previewImageRevision;
}

void ClipboardBridge::loadRecentPhotos(int maxCount)
{
    const int boundedMaxCount = qBound(1, maxCount, 80);
    auto *self = this;

    dispatch_async(dispatch_get_main_queue(), ^{
        auto startLoadBlock = ^{
            dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
                QVariantList photos;

                PHFetchOptions *options = [[PHFetchOptions alloc] init];
                options.sortDescriptors = @[[NSSortDescriptor sortDescriptorWithKey:@"creationDate" ascending:NO]];
                options.fetchLimit = boundedMaxCount;

                PHFetchResult<PHAsset *> *assets =
                    [PHAsset fetchAssetsWithMediaType:PHAssetMediaTypeImage options:options];
                PHImageRequestOptions *thumbnailOptions = [[PHImageRequestOptions alloc] init];
                thumbnailOptions.synchronous = YES;
                thumbnailOptions.deliveryMode = PHImageRequestOptionsDeliveryModeOpportunistic;
                thumbnailOptions.resizeMode = PHImageRequestOptionsResizeModeExact;
                thumbnailOptions.networkAccessAllowed = YES;

                PHImageRequestOptions *fallbackOptions = [[PHImageRequestOptions alloc] init];
                fallbackOptions.synchronous = YES;
                fallbackOptions.deliveryMode = PHImageRequestOptionsDeliveryModeFastFormat;
                fallbackOptions.resizeMode = PHImageRequestOptionsResizeModeFast;
                fallbackOptions.networkAccessAllowed = YES;

                PHImageManager *manager = [PHImageManager defaultManager];
                const NSUInteger count = qMin<NSUInteger>(assets.count, static_cast<NSUInteger>(boundedMaxCount));

                for (NSUInteger i = 0; i < count; ++i) {
                    PHAsset *asset = [assets objectAtIndex:i];
                    __block QImage thumbnailImage;
                    [manager requestImageForAsset:asset
                                       targetSize:CGSizeMake(220, 220)
                                      contentMode:PHImageContentModeAspectFill
                                          options:thumbnailOptions
                                    resultHandler:^(UIImage *result, NSDictionary *) {
                        thumbnailImage = imageFromCgImage(result.CGImage);
                    }];

                    if (thumbnailImage.isNull()) {
                        __block QByteArray imageBytes;
                        [manager requestImageDataAndOrientationForAsset:asset
                                                                options:fallbackOptions
                                                          resultHandler:^(NSData *data,
                                                                          NSString *,
                                                                          CGImagePropertyOrientation,
                                                                          NSDictionary *) {
                            if (data != nil && data.length > 0) {
                                imageBytes = QByteArray(
                                    reinterpret_cast<const char *>(data.bytes),
                                    static_cast<qsizetype>(data.length));
                            }
                        }];

                        QImage fallbackImage;
                        if (!imageBytes.isEmpty())
                            fallbackImage = QImage::fromData(imageBytes);
                        if (!fallbackImage.isNull())
                            thumbnailImage = fallbackImage.scaled(220,
                                                                  220,
                                                                  Qt::KeepAspectRatioByExpanding,
                                                                  Qt::FastTransformation);
                    }

                    if (thumbnailImage.isNull())
                        continue;

                    const QString thumbnailBase64 = pngBase64FromImage(thumbnailImage);
                    if (thumbnailBase64.isEmpty())
                        continue;

                    QVariantMap item;
                    item.insert(QStringLiteral("thumbnail"), thumbnailBase64);
                    item.insert(QStringLiteral("id"), QString::fromUtf8(asset.localIdentifier.UTF8String));
                    if (asset.creationDate)
                        item.insert(QStringLiteral("createdAtMs"),
                                    qint64([asset.creationDate timeIntervalSince1970] * 1000.0));
                    photos.push_back(item);
                }

                QMetaObject::invokeMethod(self, [self, photos]() {
                    emit self->recentPhotosLoaded(photos);
                }, Qt::QueuedConnection);
            });
        };

        const PHAuthorizationStatus status =
            [PHPhotoLibrary authorizationStatusForAccessLevel:PHAccessLevelReadWrite];
        if (status == PHAuthorizationStatusAuthorized || status == PHAuthorizationStatusLimited) {
            startLoadBlock();
            return;
        }

        if (status == PHAuthorizationStatusNotDetermined) {
            [PHPhotoLibrary requestAuthorizationForAccessLevel:PHAccessLevelReadWrite
                                                       handler:^(PHAuthorizationStatus newStatus) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    if (newStatus == PHAuthorizationStatusAuthorized
                        || newStatus == PHAuthorizationStatusLimited) {
                        startLoadBlock();
                    } else {
                        QMetaObject::invokeMethod(self, [self]() {
                            emit self->recentPhotosLoaded(QVariantList{});
                        }, Qt::QueuedConnection);
                    }
                });
            }];
            return;
        }

        QMetaObject::invokeMethod(self, [self]() {
            emit self->recentPhotosLoaded(QVariantList{});
        }, Qt::QueuedConnection);
    });
}

void ClipboardBridge::loadPhotoContent(const QString &assetId)
{
    if (assetId.isEmpty()) {
        emit photoContentLoaded(assetId, QString{});
        return;
    }

    auto *self = this;
    const QString requestedAssetId = assetId;
    dispatch_async(dispatch_get_main_queue(), ^{
        auto loadBlock = ^{
            PHFetchOptions *options = [[PHFetchOptions alloc] init];
            PHFetchResult<PHAsset *> *assets =
                [PHAsset fetchAssetsWithLocalIdentifiers:@[requestedAssetId.toNSString()] options:options];
            PHAsset *asset = assets.count > 0 ? [assets objectAtIndex:0] : nil;
            if (asset == nil) {
                QMetaObject::invokeMethod(self, [self, requestedAssetId]() {
                    emit self->photoContentLoaded(requestedAssetId, QString{});
                }, Qt::QueuedConnection);
                return;
            }

            dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
                PHImageRequestOptions *dataOptions = [[PHImageRequestOptions alloc] init];
                dataOptions.synchronous = YES;
                dataOptions.deliveryMode = PHImageRequestOptionsDeliveryModeHighQualityFormat;
                dataOptions.networkAccessAllowed = YES;
                dataOptions.version = PHImageRequestOptionsVersionCurrent;

                __block QByteArray imageBytes;
                __block NSDictionary *dataInfo = nil;
                [[PHImageManager defaultManager] requestImageDataAndOrientationForAsset:asset
                                                                                options:dataOptions
                                                                          resultHandler:^(NSData *data,
                                                                                          NSString *,
                                                                                          CGImagePropertyOrientation,
                                                                                          NSDictionary *info) {
                    dataInfo = info;
                    if (data != nil && data.length > 0) {
                        imageBytes = QByteArray(
                            reinterpret_cast<const char *>(data.bytes),
                            static_cast<qsizetype>(data.length));
                    }
                }];

                QImage qimage;
                if (!imageBytes.isEmpty())
                    qimage = imageFromData([NSData dataWithBytes:imageBytes.constData()
                                                          length:static_cast<NSUInteger>(imageBytes.size())]);
                if (qimage.isNull()) {
                    PHImageRequestOptions *imageOptions = [[PHImageRequestOptions alloc] init];
                    imageOptions.synchronous = YES;
                    imageOptions.deliveryMode = PHImageRequestOptionsDeliveryModeHighQualityFormat;
                    imageOptions.resizeMode = PHImageRequestOptionsResizeModeNone;
                    imageOptions.networkAccessAllowed = YES;
                    imageOptions.version = PHImageRequestOptionsVersionCurrent;

                    __block UIImage *fallbackImage = nil;
                    __block NSDictionary *imageInfo = nil;
                    [[PHImageManager defaultManager] requestImageForAsset:asset
                                                               targetSize:PHImageManagerMaximumSize
                                                              contentMode:PHImageContentModeAspectFit
                                                                  options:imageOptions
                                                            resultHandler:^(UIImage *result, NSDictionary *info) {
                        imageInfo = info;
                        if (result != nil)
                            fallbackImage = result;
                    }];
                    qimage = imageFromDrawnUiImage(fallbackImage);
                }
                const QByteArray contentData = photoClipboardPngData(qimage);
                const QString contentBase64 = contentData.isEmpty()
                    ? QString{}
                    : QString::fromLatin1(contentData.toBase64());
                QMetaObject::invokeMethod(self, [self, requestedAssetId, contentBase64]() {
                    emit self->photoContentLoaded(requestedAssetId, contentBase64);
                }, Qt::QueuedConnection);
            });
        };

        const PHAuthorizationStatus status =
            [PHPhotoLibrary authorizationStatusForAccessLevel:PHAccessLevelReadWrite];
        if (status == PHAuthorizationStatusAuthorized || status == PHAuthorizationStatusLimited) {
            loadBlock();
            return;
        }

        if (status == PHAuthorizationStatusNotDetermined) {
            [PHPhotoLibrary requestAuthorizationForAccessLevel:PHAccessLevelReadWrite
                                                       handler:^(PHAuthorizationStatus newStatus) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    if (newStatus == PHAuthorizationStatusAuthorized
                        || newStatus == PHAuthorizationStatusLimited) {
                        loadBlock();
                    } else {
                        QMetaObject::invokeMethod(self, [self, requestedAssetId]() {
                            emit self->photoContentLoaded(requestedAssetId, QString{});
                        }, Qt::QueuedConnection);
                    }
                });
            }];
            return;
        }

        QMetaObject::invokeMethod(self, [self, requestedAssetId]() {
            emit self->photoContentLoaded(requestedAssetId, QString{});
        }, Qt::QueuedConnection);
    });
}

void ClipboardBridge::openCamera()
{
    auto *self = this;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (![UIImagePickerController isSourceTypeAvailable:UIImagePickerControllerSourceTypeCamera]) {
            QMetaObject::invokeMethod(self, [self]() {
                emit self->cameraCaptureFailed(QStringLiteral("Kamera ist auf diesem Gerät nicht verfügbar."));
            }, Qt::QueuedConnection);
            return;
        }

        UIViewController *presenter = topViewController();
        if (presenter == nil) {
            QMetaObject::invokeMethod(self, [self]() {
                emit self->cameraCaptureFailed(QStringLiteral("Kamera konnte nicht geöffnet werden."));
            }, Qt::QueuedConnection);
            return;
        }

        static NetworkClipboardCameraDelegate *cameraDelegate = nil;
        cameraDelegate = [[NetworkClipboardCameraDelegate alloc] initWithBridge:self];

        UIImagePickerController *picker = [[UIImagePickerController alloc] init];
        picker.sourceType = UIImagePickerControllerSourceTypeCamera;
        picker.mediaTypes = @[(NSString *)UTTypeImage.identifier];
        picker.allowsEditing = NO;
        picker.delegate = cameraDelegate;
        [presenter presentViewController:picker animated:YES completion:nil];
    });
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
