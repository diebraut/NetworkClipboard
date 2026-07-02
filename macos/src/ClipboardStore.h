#pragma once

#include "ClipboardEntry.h"

#include <QList>
#include <QObject>
#include <QReadWriteLock>
#include <optional>

class ClipboardStore : public QObject
{
    Q_OBJECT

public:
    explicit ClipboardStore(QObject *parent = nullptr);

    void setLatest(const ClipboardEntry &entry);
    std::optional<ClipboardEntry> latest() const;
    QList<ClipboardEntry> history() const;
    void clear();

signals:
    void latestChanged(const ClipboardEntry &entry);

private:
    mutable QReadWriteLock m_lock;
    QList<ClipboardEntry> m_history;
    int m_maxHistoryItems = 100;
};
