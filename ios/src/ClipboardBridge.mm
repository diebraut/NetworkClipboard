#include "ClipboardBridge.h"

#include <QBuffer>
#include <QByteArrayView>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QSettings>
#include <QUrl>

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
    for (int attempt = 0; attempt < 10; ++attempt) {
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

QImage pasteboardImage()
{
    UIPasteboard *pasteboard = UIPasteboard.generalPasteboard;
    UIImage *image = pasteboard.image;
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
    updateImageCache();
    return !m_cachedImage.isNull();
}

QString ClipboardBridge::imageFingerprint() const
{
    updateImageCache();
    return m_cachedImageFingerprint;
}

QString ClipboardBridge::imageBase64() const
{
    updateImageCache();
    return m_cachedImageBase64;
}

bool ClipboardBridge::setImageBase64(const QString &base64)
{
    const QByteArray pngData = QByteArray::fromBase64(base64.toLatin1());
    const QImage image = QImage::fromData(pngData, "PNG");
    if (image.isNull())
        return false;

    NSData *data = [NSData dataWithBytes:pngData.constData()
                                  length:static_cast<NSUInteger>(pngData.size())];
    UIImage *uiImage = [UIImage imageWithData:data];
    if (uiImage == nil)
        return false;

    UIPasteboard.generalPasteboard.image = uiImage;
    m_cachedPasteboardChangeCount = UIPasteboard.generalPasteboard.changeCount;
    m_cachedImage = image;
    m_cachedImageFingerprint = QString::fromLatin1(imageFingerprintBytes(image).toHex());
    m_cachedImageBase64 = QString::fromLatin1(pngData.toBase64());
    emit clipboardChanged();
    return true;
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
    UIPasteboard *pasteboard = UIPasteboard.generalPasteboard;
    const qint64 changeCount = pasteboard.changeCount;
    if (m_cachedPasteboardChangeCount == changeCount)
        return;

    m_cachedImage = pasteboardImage();
    m_cachedImageFingerprint.clear();
    m_cachedImageBase64.clear();
    if (m_cachedImage.isNull()) {
        // Photos may publish an item provider before the actual image data is
        // available. Do not cache that temporary empty result.
        if (!pasteboard.hasImages)
            m_cachedPasteboardChangeCount = changeCount;
        return;
    }

    m_cachedPasteboardChangeCount = changeCount;
    m_cachedImageFingerprint = QString::fromLatin1(imageFingerprintBytes(m_cachedImage).toHex());
    const QByteArray pngData = networkPngData(m_cachedImage);
    if (!pngData.isEmpty())
        m_cachedImageBase64 = QString::fromLatin1(pngData.toBase64());
}
