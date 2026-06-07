#pragma once

#include <QObject>
#include <QString>

class ClipboardBridge : public QObject
{
    Q_OBJECT

public:
    explicit ClipboardBridge(QObject *parent = nullptr);

    Q_INVOKABLE QString text() const;
    Q_INVOKABLE void setText(const QString &text);
};
