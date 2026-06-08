#include "ClipboardBridge.h"

#include <QClipboard>
#include <QGuiApplication>

namespace {
QString withUnixLineEndings(QString text)
{
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    return text;
}
}

ClipboardBridge::ClipboardBridge(QObject *parent)
    : QObject(parent)
{
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, &ClipboardBridge::textChanged);
}

QString ClipboardBridge::text() const
{
    return QGuiApplication::clipboard()->text();
}

void ClipboardBridge::setText(const QString &text)
{
    QGuiApplication::clipboard()->setText(withUnixLineEndings(text));
}
