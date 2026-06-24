#include "ClipboardStore.h"

#include <QReadLocker>
#include <QWriteLocker>

#include <utility>

ClipboardStore::ClipboardStore(QObject *parent)
    : QObject(parent)
{
}

void ClipboardStore::setLatest(const ClipboardEntry &entry)
{
    {
        QWriteLocker locker(&m_lock);
        m_history.prepend(entry);
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
