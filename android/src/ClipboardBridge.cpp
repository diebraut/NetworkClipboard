#include "ClipboardBridge.h"

#include <QBuffer>
#include <QByteArrayView>
#include <QClipboard>
#include <QCryptographicHash>
#include <QGuiApplication>
#include <QImage>
#include <QMimeData>
#include <QTextDocument>

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

QString ClipboardBridge::imageBase64() const
{
    updateImageCache();
    return m_cachedImageBase64;
}

bool ClipboardBridge::setImageBase64(const QString &base64)
{
    const QImage image = QImage::fromData(QByteArray::fromBase64(base64.toLatin1()), "PNG");
    if (image.isNull())
        return false;

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
    m_cachedImage = image;
    m_cachedImageKey.clear();
    m_cachedImageFingerprint = QString::fromLatin1(imageFingerprintBytes(image).toHex());
    m_cachedImageBase64 = base64;
#else
    QGuiApplication::clipboard()->setImage(image);
#endif
    return true;
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

    m_imageCacheValid = true;
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

    m_cachedImageFingerprint = QString::fromLatin1(imageFingerprintBytes(m_cachedImage).toHex());
    QByteArray pngData;
    QBuffer buffer(&pngData);
    if (buffer.open(QIODevice::WriteOnly) && m_cachedImage.save(&buffer, "PNG"))
        m_cachedImageBase64 = QString::fromLatin1(pngData.toBase64());
}
