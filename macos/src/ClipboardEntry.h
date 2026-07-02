#pragma once

#include <QJsonObject>
#include <QString>

struct ClipboardEntry
{
    QString id;
    QString deviceId;
    QString deviceName;
    QString type = QStringLiteral("text");
    QString mimeType;
    QString content;
    qint64 timestamp = 0;

    static ClipboardEntry fromJson(const QJsonObject &object);
    QJsonObject toJson() const;
    bool isValid(QString *errorMessage = nullptr) const;
};
