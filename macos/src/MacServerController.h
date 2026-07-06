#pragma once

#include "ApiServer.h"
#include "ClipboardContentWindow.h"
#include "ClipboardEntry.h"
#include "ClipboardStore.h"

#include <QByteArray>
#include <QObject>
#include <QSystemTrayIcon>
#include <QTimer>

class QAction;
class QClipboard;
class QImage;
class QMenu;
class QMimeData;

class MacServerController : public QObject
{
    Q_OBJECT

public:
    explicit MacServerController(QObject *parent = nullptr);

    void show();

private:
    void startServer();
    void buildMenu();
    void onClipboardChanged();
    void processClipboardChange();
    void showContent();
    void updateContentWindow();
    void pasteFromNetwork();
    void publishClipboardNow();
    void copyServerInfo();
    void clearHistory();
    void setAutoPublish(bool enabled);
    void setMasterServer(bool enabled);
    void setStartAtLogin(bool enabled);
    void updateStartAtLoginAction();
    void applyEntryToClipboard(const ClipboardEntry &entry, bool showMessage, bool allowOwnEntry);
    void publishClipboardText(const QString &text, bool showMessage, bool force = false);
    void publishClipboardImage(const QImage &image, bool showMessage, bool force = false);
    void showStatus(const QString &message);
    QString deviceName() const;
    QString deviceId();
    QString serverInfo() const;

    QClipboard *m_clipboard = nullptr;
    ClipboardStore m_store;
    ApiServer m_server;
    QSystemTrayIcon m_tray;
    QMenu *m_menu = nullptr;
    ClipboardContentWindow *m_contentWindow = nullptr;
    QAction *m_autoPublishAction = nullptr;
    QAction *m_masterAction = nullptr;
    QAction *m_startAtLoginAction = nullptr;
    QTimer m_clipboardChangeTimer;
    QTimer m_clipboardPollTimer;
    QString m_token;
    QString m_deviceId;
    bool m_autoPublish = true;
    bool m_masterServer = true;
    qint64 m_ignoreClipboardChangesUntil = 0;
    QString m_ignoredClipboardContent;
    QByteArray m_ignoredClipboardImageHash;
    QString m_lastPublishedContent;
    QByteArray m_lastPublishedImageHash;
    QString m_lastSeenEntryId;
};
