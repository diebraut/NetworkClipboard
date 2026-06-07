#include "ClipboardBridge.h"

#include <QClipboard>
#include <QGuiApplication>

ClipboardBridge::ClipboardBridge(QObject *parent)
    : QObject(parent)
{
}

QString ClipboardBridge::text() const
{
    return QGuiApplication::clipboard()->text();
}

void ClipboardBridge::setText(const QString &text)
{
    QGuiApplication::clipboard()->setText(text);
}
