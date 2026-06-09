#include "ClipboardBridge.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QMimeData>
#include <QTextDocument>

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
}

ClipboardBridge::ClipboardBridge(QObject *parent)
    : QObject(parent)
{
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, &ClipboardBridge::textChanged);
}

QString ClipboardBridge::text() const
{
    return withUnixLineEndings(clipboardPlainText());
}

void ClipboardBridge::setText(const QString &text)
{
    QGuiApplication::clipboard()->setText(withUnixLineEndings(text));
}
