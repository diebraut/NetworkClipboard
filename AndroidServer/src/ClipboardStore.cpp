#include "ClipboardStore.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QIODevice>
#include <QReadLocker>
#include <QSaveFile>
#include <QStandardPaths>
#include <QWriteLocker>

#include <utility>

ClipboardStore::ClipboardStore(QObject *parent)
    : QObject(parent)
{
    loadHistory();
}

void ClipboardStore::setLatest(const ClipboardEntry &entry)
{
    bool changed = false;
    {
        QWriteLocker locker(&m_lock);
        if (!m_history.isEmpty()) {
            const ClipboardEntry &latest = m_history.first();
            if (latest.type == entry.type
                && latest.mimeType == entry.mimeType
                && latest.content == entry.content) {
                return;
            }
        }

        m_history.prepend(entry);
        pruneHistoryLocked();
        changed = true;
    }

    if (changed) {
        saveHistory();
        emit latestChanged(entry);
    }
}

std::optional<ClipboardEntry> ClipboardStore::latest() const
{
    QReadLocker locker(&m_lock);
    if (m_history.isEmpty())
        return std::nullopt;
    return m_history.first();
}

QList<ClipboardEntry> ClipboardStore::history() const
{
    QReadLocker locker(&m_lock);
    return m_history;
}

void ClipboardStore::clear()
{
    {
        QWriteLocker locker(&m_lock);
        m_history.clear();
    }
    saveHistory();
}

QString ClipboardStore::storagePath() const
{
    const QString directoryPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(directoryPath);
#ifdef Q_OS_ANDROID
    return QDir(directoryPath).filePath(QStringLiteral("NetworkClipboardQtHistory.json"));
#else
    return QDir(directoryPath).filePath(QStringLiteral("NetworkClipboardHistory.json"));
#endif
}

void ClipboardStore::loadHistory()
{
    QFile file(storagePath());
    if (!file.open(QIODevice::ReadOnly))
        return;

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return;

    QList<ClipboardEntry> entries;
    const QJsonArray items = document.object().value(QStringLiteral("items")).toArray();
    for (const QJsonValue &item : items) {
        if (!item.isObject())
            continue;
        ClipboardEntry entry = ClipboardEntry::fromJson(item.toObject());
        QString validationError;
        if (entry.isValid(&validationError))
            entries.append(std::move(entry));
    }

    QWriteLocker locker(&m_lock);
    m_history = std::move(entries);
    pruneHistoryLocked();
}

void ClipboardStore::saveHistory() const
{
    QJsonArray items;
    {
        QReadLocker locker(&m_lock);
        for (const ClipboardEntry &entry : m_history)
            items.append(entry.toJson());
    }

    QSaveFile file(storagePath());
    if (!file.open(QIODevice::WriteOnly))
        return;

    file.write(QJsonDocument(QJsonObject{{QStringLiteral("items"), items}})
                   .toJson(QJsonDocument::Compact));
    file.commit();
}

void ClipboardStore::pruneHistoryLocked()
{
    qsizetype storedContentSize = 0;
    int retainedItems = 0;
    constexpr qsizetype MaxStoredContentSize = 32 * 1024 * 1024;
    for (const ClipboardEntry &item : std::as_const(m_history)) {
        storedContentSize += item.content.size();
        ++retainedItems;
        if (retainedItems >= m_maxHistoryItems || storedContentSize > MaxStoredContentSize)
            break;
    }
    while (m_history.size() > retainedItems)
        m_history.removeLast();
}
