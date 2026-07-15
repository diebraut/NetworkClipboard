#pragma once

#include <QImage>
#include <QObject>
#include <QString>

class ClipboardBridge : public QObject
{
    Q_OBJECT

public:
    explicit ClipboardBridge(QObject *parent = nullptr);

    Q_INVOKABLE QString text() const;
    Q_INVOKABLE void setText(const QString &text);
    Q_INVOKABLE bool hasImage() const;
    Q_INVOKABLE QString imageFingerprint() const;
    Q_INVOKABLE QString imageFingerprintFromBase64(const QString &base64) const;
    Q_INVOKABLE QString imageVisualFingerprintFromBase64(const QString &base64) const;
    Q_INVOKABLE QString thumbnailBase64FromBase64(const QString &base64, int maxSize = 96) const;
    Q_INVOKABLE bool imageHasMeaningfulContentBase64(const QString &base64) const;
    Q_INVOKABLE QString saveHistoryImageBase64(const QString &base64) const;
    Q_INVOKABLE QString loadHistoryImageBase64(const QString &imageId) const;
    Q_INVOKABLE QString imageBase64() const;
    Q_INVOKABLE bool imageFromNetworkClipboard() const;
    Q_INVOKABLE bool setImageBase64(const QString &base64);

signals:
    void textChanged();
    void clipboardChanged();

private:
    void invalidateImageCache();
    void updateImageCache() const;

    mutable bool m_imageCacheValid = false;
    mutable QImage m_cachedImage;
    mutable QString m_cachedImageKey;
    mutable QString m_cachedImageFingerprint;
    mutable QString m_cachedImageBase64;
};
