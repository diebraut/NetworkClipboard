#pragma once

#include "ClipboardStore.h"

#include <QDateTime>
#include <QObject>
#include <QSystemTrayIcon>

class QAction;
class QClipboard;
class QMenu;

class TrayController : public QObject
{
    Q_OBJECT

public:
    TrayController(ClipboardStore *store, const QString &deviceId, const QString &deviceName, QObject *parent = nullptr);

    void show();
    void setServerInfo(quint16 port, const QString &token);

private:
    void onClipboardChanged();
    void pasteFromNetwork();
    void copyServerInfo();
    void setAutoSendEnabled(bool enabled);

    ClipboardStore *m_store = nullptr;
    QClipboard *m_clipboard = nullptr;
    QSystemTrayIcon m_tray;
    QMenu *m_menu = nullptr;
    QAction *m_autoSendAction = nullptr;
    QString m_deviceId;
    QString m_deviceName;
    QString m_token;
    quint16 m_port = 8787;
    bool m_autoSendEnabled = true;
    qint64 m_ignoreClipboardChangesUntil = 0;
    QString m_ignoredClipboardContent;
    QString m_lastPublishedContent;
};
