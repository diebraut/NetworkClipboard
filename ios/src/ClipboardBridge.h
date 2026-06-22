#pragma once

#include <QImage>
#include <QObject>
#include <QString>

class ClipboardBridge : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool shouldOfferPasteSettings READ shouldOfferPasteSettings NOTIFY pasteSettingsOfferChanged)

public:
    explicit ClipboardBridge(QObject *parent = nullptr);

    bool shouldOfferPasteSettings() const;

    Q_INVOKABLE QString text() const;
    Q_INVOKABLE void setText(const QString &text);
    Q_INVOKABLE bool hasImage() const;
    Q_INVOKABLE QString imageFingerprint() const;
    Q_INVOKABLE QString imageBase64() const;
    Q_INVOKABLE bool setImageBase64(const QString &base64);
    Q_INVOKABLE void markPasteSettingsOfferSeen();
    Q_INVOKABLE void openAppSettings() const;

signals:
    void textChanged();
    void clipboardChanged();
    void pasteSettingsOfferChanged();

private:
    void updateImageCache() const;

    mutable qint64 m_cachedPasteboardChangeCount = -1;
    mutable QImage m_cachedImage;
    mutable QString m_cachedImageFingerprint;
    mutable QString m_cachedImageBase64;
};
