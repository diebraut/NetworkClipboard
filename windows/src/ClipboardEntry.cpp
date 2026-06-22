#include "ClipboardEntry.h"

#include <QDateTime>
#include <QByteArray>
#include <QUuid>

namespace {
constexpr qsizetype MaxTextSize = 1024 * 1024;
constexpr qsizetype MaxImageSize = 10 * 1024 * 1024;
}

ClipboardEntry ClipboardEntry::fromJson(const QJsonObject &object)
{
    ClipboardEntry entry;
    entry.id = object.value(QStringLiteral("id")).toString();
    entry.deviceId = object.value(QStringLiteral("deviceId")).toString();
    entry.deviceName = object.value(QStringLiteral("deviceName")).toString();
    entry.type = object.value(QStringLiteral("type")).toString(QStringLiteral("text"));
    entry.mimeType = object.value(QStringLiteral("mimeType")).toString();
    entry.content = object.value(QStringLiteral("content")).toString();
    entry.timestamp = static_cast<qint64>(object.value(QStringLiteral("timestamp")).toDouble(0));

    if (entry.id.isEmpty())
        entry.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (entry.timestamp <= 0)
        entry.timestamp = QDateTime::currentSecsSinceEpoch();

    return entry;
}

QJsonObject ClipboardEntry::toJson() const
{
    return {{QStringLiteral("id"), id},
            {QStringLiteral("deviceId"), deviceId},
            {QStringLiteral("deviceName"), deviceName},
            {QStringLiteral("type"), type},
            {QStringLiteral("mimeType"), mimeType},
            {QStringLiteral("content"), content},
            {QStringLiteral("timestamp"), timestamp}};
}

bool ClipboardEntry::isValid(QString *errorMessage) const
{
    auto fail = [errorMessage](const QString &message) {
        if (errorMessage)
            *errorMessage = message;
        return false;
    };

    if (type != QStringLiteral("text")
        && type != QStringLiteral("url")
        && type != QStringLiteral("image")) {
        return fail(QStringLiteral("Unsupported clipboard type."));
    }
    if (content.isEmpty())
        return fail(QStringLiteral("Content must not be empty."));

    if (type == QStringLiteral("image")) {
        if (mimeType != QStringLiteral("image/png"))
            return fail(QStringLiteral("Only PNG images are supported."));

        const QByteArray encoded = content.toLatin1();
        if (encoded.size() % 4 != 0)
            return fail(QStringLiteral("Image content is not valid Base64."));

        const QByteArray imageData = QByteArray::fromBase64(encoded);
        constexpr char PngSignature[] = "\x89PNG\r\n\x1a\n";
        if (imageData.isEmpty()
            || imageData.size() > MaxImageSize
            || imageData.toBase64() != encoded
            || !imageData.startsWith(QByteArray(PngSignature, 8))) {
            return fail(QStringLiteral("Image exceeds the 10 MB limit or is invalid."));
        }
    } else if (content.size() > MaxTextSize) {
        return fail(QStringLiteral("Content exceeds the 1 MB limit."));
    }

    return true;
}
