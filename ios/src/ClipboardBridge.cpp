#include "ClipboardBridge.h"

#include <QClipboard>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QSettings>
#include <QUrl>

namespace {
QString withUnixLineEndings(QString text)
{
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    return text;
}

constexpr auto PasteSettingsOfferSeenKey = "pasteSettingsOfferSeen";
}

ClipboardBridge::ClipboardBridge(QObject *parent)
    : QObject(parent)
{
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, &ClipboardBridge::textChanged);
}

bool ClipboardBridge::shouldOfferPasteSettings() const
{
    QSettings settings;
    return !settings.value(QLatin1String(PasteSettingsOfferSeenKey), false).toBool();
}

QString ClipboardBridge::text() const
{
    return withUnixLineEndings(QGuiApplication::clipboard()->text());
}

void ClipboardBridge::setText(const QString &text)
{
    QGuiApplication::clipboard()->setText(withUnixLineEndings(text));
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
