#pragma once

#include "ClipboardContentWindow.h"
#include "ClipboardEntry.h"

#include <QDateTime>
#include <QByteArray>
#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSize>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QUrl>

#include <optional>

class QAction;
class QClipboard;
class QImage;
class QMenu;
class QMimeData;

class TrayController : public QObject
{
    Q_OBJECT

public:
    TrayController(const QString &deviceId, const QString &deviceName, QObject *parent = nullptr);

    void show();
    void setServerInfo(const QUrl &serverUrl, quint16 port, const QString &token);

private:
    void onClipboardChanged();
    void processClipboardChange();
    void pollLatestFromServer();
    void fetchClipboardHistory();
    void sendAgentHeartbeat();
    void showContent();
    void updateContentWindow();
    bool updateContentWindowFromImageUrl(const QMimeData *mimeData, const QString &fallbackText);
    bool updateContentWindowFromLatestNetworkImage();
    void sendCurrentClipboard();
    void pasteFromNetwork();
    void copyServerInfo();
    void startServerServiceIfNeeded();
    void toggleServerService();
    void updateServiceStatus();
    void setAutoSendEnabled(bool enabled);
    void setMasterServer(bool isMaster);
    void sendEntryToServer(const ClipboardEntry &entry, bool showSuccessMessage);
    void postEntryToServer(const ClipboardEntry &entry, bool showSuccessMessage);
    void finishSendEntryToServer();
    void rehydrateServerFromCachedEntry();
    void publishClipboardText(const QString &text, bool showSuccessMessage, bool force = false);
    void publishClipboardImage(const QImage &image, bool showSuccessMessage, bool force = false);
    bool tryPublishImageUrl(const QMimeData *mimeData, const QString &text, bool showSuccessMessage, bool force, bool publishTextOnFailure);
    void publishDownloadedImageUrl(const QUrl &imageUrl, const QString &fallbackText, bool showSuccessMessage, bool force, bool publishTextOnFailure);
    void publishCurrentClipboardIfAvailable(bool force);
    void scheduleCurrentClipboardPublish(bool force);
    void applyNetworkEntryToClipboard(const ClipboardEntry &entry, bool showMessage, bool allowOwnEntry);
    QNetworkRequest apiRequest(const QString &path) const;

    QClipboard *m_clipboard = nullptr;
    QNetworkAccessManager m_network;
    QTimer m_pollTimer;
    QTimer m_clipboardChangeTimer;
    QSystemTrayIcon m_tray;
    QMenu *m_menu = nullptr;
    ClipboardContentWindow *m_contentWindow = nullptr;
    QAction *m_autoSendAction = nullptr;
    QAction *m_masterAction = nullptr;
    QAction *m_serviceAction = nullptr;
    QString m_deviceId;
    QString m_deviceName;
    QString m_token;
    QUrl m_serverUrl;
    quint16 m_port = 8787;
    bool m_autoSendEnabled = true;
    bool m_isMaster = true;
    bool m_serviceRunning = false;
    qint64 m_ignoreClipboardChangesUntil = 0;
    QString m_ignoredClipboardContent;
    QByteArray m_ignoredClipboardImageHash;
    QString m_lastSeenNetworkEntryId;
    QString m_lastSeenNetworkContent;
    QByteArray m_lastSeenNetworkImageHash;
    QString m_lastPublishedContent;
    QByteArray m_lastPublishedImageHash;
    qint64 m_recentImagePublishUntil = 0;
    qint64 m_lastLocalPostCompletedAt = 0;
    QSize m_recentPublishedImageSize;
    std::optional<ClipboardEntry> m_latestNetworkEntry;
    QList<ClipboardEntry> m_networkHistory;
    bool m_sendInFlight = false;
    std::optional<ClipboardEntry> m_pendingEntry;
    bool m_pendingShowSuccessMessage = false;
    quint64 m_clipboardChangeGeneration = 0;
    int m_clipboardRetriesRemaining = 0;
    QString m_pendingImageUrlDownload;
    QString m_pendingContentImageUrlDownload;
};
