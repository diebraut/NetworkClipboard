#include "ClipboardStore.h"

#include <QReadLocker>
#include <QWriteLocker>

ClipboardStore::ClipboardStore(QObject *parent)
    : QObject(parent)
{
}

void ClipboardStore::setLatest(const ClipboardEntry &entry)
{
    {
        QWriteLocker locker(&m_lock);
        m_history.prepend(entry);
        while (m_history.size() > m_maxHistoryItems)
            m_history.removeLast();
    }

    emit latestChanged(entry);
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
    QWriteLocker locker(&m_lock);
    m_history.clear();
}
