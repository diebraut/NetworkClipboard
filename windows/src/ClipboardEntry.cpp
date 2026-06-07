#include "ClipboardEntry.h"

#include <QDateTime>
#include <QUuid>

namespace {
constexpr qsizetype MaxContentSize = 1024 * 1024;
}

ClipboardEntry ClipboardEntry::fromJson(const QJsonObject &object)
{
    ClipboardEntry entry;
    entry.id = object.value(QStringLiteral("id")).toString();
    entry.deviceId = object.value(QStringLiteral("deviceId")).toString();
    entry.deviceName = object.value(QStringLiteral("deviceName")).toString();
    entry.type = object.value(QStringLiteral("type")).toString(QStringLiteral("text"));
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

    if (type != QStringLiteral("text") && type != QStringLiteral("url"))
        return fail(QStringLiteral("Unsupported clipboard type."));
    if (content.isEmpty())
        return fail(QStringLiteral("Content must not be empty."));
    if (content.size() > MaxContentSize)
        return fail(QStringLiteral("Content exceeds the 1 MB limit."));

    return true;
}
