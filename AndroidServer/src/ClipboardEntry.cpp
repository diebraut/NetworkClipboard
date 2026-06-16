#include "ClipboardEntry.h"

ClipboardEntry ClipboardEntry::fromJson(const QJsonObject &object)
{
    ClipboardEntry entry;
    entry.id = object.value(QStringLiteral("id")).toString();
    entry.deviceId = object.value(QStringLiteral("deviceId")).toString();
    entry.deviceName = object.value(QStringLiteral("deviceName")).toString();
    entry.type = object.value(QStringLiteral("type")).toString(QStringLiteral("text"));
    entry.content = object.value(QStringLiteral("content")).toString();
    entry.timestamp = static_cast<qint64>(object.value(QStringLiteral("timestamp")).toDouble());
    return entry;
}

QJsonObject ClipboardEntry::toJson() const
{
    return {
        {QStringLiteral("id"), id},
        {QStringLiteral("deviceId"), deviceId},
        {QStringLiteral("deviceName"), deviceName},
        {QStringLiteral("type"), type},
        {QStringLiteral("content"), content},
        {QStringLiteral("timestamp"), timestamp}
    };
}

bool ClipboardEntry::isValid(QString *errorMessage) const
{
    if (deviceId.trimmed().isEmpty()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("deviceId is required.");
        return false;
    }
    if (type != QStringLiteral("text") && type != QStringLiteral("url")) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Only text and url clipboard entries are supported.");
        return false;
    }
    if (content.trimmed().isEmpty()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("content is required.");
        return false;
    }
    return true;
}
