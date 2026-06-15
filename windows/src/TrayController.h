#pragma once

#include "ClipboardEntry.h"

#include <QDateTime>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QUrl>

class QAction;
class QClipboard;
class QMenu;

class TrayController : public QObject
{
    Q_OBJECT

public:
    TrayController(const QString &deviceId, const QString &deviceName, QObject *parent = nullptr);

    void show();
    void setServerInfo(const QUrl &serverUrl, quint16 port, const QString &token);

private:
    void onClipboardChanged();
    void pollLatestFromServer();
    void sendAgentHeartbeat();
    void sendCurrentClipboard();
    void pasteFromNetwork();
    void copyServerInfo();
    void toggleServerService();
    void updateServiceStatus();
    void setAutoSendEnabled(bool enabled);
    void sendEntryToServer(const ClipboardEntry &entry, bool showSuccessMessage);
    void publishClipboardText(const QString &text, bool showSuccessMessage, bool force = false);
    void publishCurrentClipboardIfAvailable(bool force);
    void scheduleCurrentClipboardPublish(bool force);
    void applyNetworkEntryToClipboard(const ClipboardEntry &entry, bool showMessage, bool allowOwnEntry);
    QNetworkRequest apiRequest(const QString &path) const;

    QClipboard *m_clipboard = nullptr;
    QNetworkAccessManager m_network;
    QTimer m_pollTimer;
    QSystemTrayIcon m_tray;
    QMenu *m_menu = nullptr;
    QAction *m_autoSendAction = nullptr;
    QAction *m_serviceAction = nullptr;
    QString m_deviceId;
    QString m_deviceName;
    QString m_token;
    QUrl m_serverUrl;
    quint16 m_port = 8787;
    bool m_autoSendEnabled = true;
    bool m_serviceRunning = false;
    qint64 m_ignoreClipboardChangesUntil = 0;
    QString m_ignoredClipboardContent;
    QString m_lastSeenNetworkEntryId;
    QString m_lastSeenNetworkContent;
    QString m_lastPublishedContent;
};
