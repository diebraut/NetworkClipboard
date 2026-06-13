#pragma once

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
    Q_INVOKABLE void markPasteSettingsOfferSeen();
    Q_INVOKABLE void openAppSettings() const;

signals:
    void textChanged();
    void pasteSettingsOfferChanged();
};
