#pragma once

#include "ApiServer.h"
#include "ClipboardStore.h"

#include <QClipboard>
#include <QObject>
#include <QTimer>

#ifdef Q_OS_ANDROID
#include <QJniObject>
#endif

class AndroidServerController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QString token READ token NOTIFY tokenChanged)
    Q_PROPERTY(QString serverInfo READ serverInfo NOTIFY serverInfoChanged)
    Q_PROPERTY(QString serverUrlsText READ serverUrlsText NOTIFY serverInfoChanged)
    Q_PROPERTY(QString latestContent READ latestContent NOTIFY latestContentChanged)
    Q_PROPERTY(bool autoPublish READ autoPublish WRITE setAutoPublish NOTIFY autoPublishChanged)

public:
    explicit AndroidServerController(QObject *parent = nullptr);

    QString status() const;
    QString token() const;
    QString serverInfo() const;
    QString serverUrlsText() const;
    QString latestContent() const;
    bool autoPublish() const;
    void setAutoPublish(bool enabled);

    Q_INVOKABLE void publishClipboardNow();
    Q_INVOKABLE void copyServerInfo();

signals:
    void statusChanged();
    void tokenChanged();
    void serverInfoChanged();
    void latestContentChanged();
    void autoPublishChanged();

private:
    void start();
    void acquireMulticastLock();
    void onClipboardChanged();
    void publishClipboardText(const QString &text, bool force);
    void applyEntryToClipboard(const ClipboardEntry &entry);
    void setStatus(const QString &status);
    void setLatestContent(const QString &content);
    QString deviceName() const;
    QString deviceId();
    QString currentServerInfo() const;
    QStringList currentServerUrls() const;

    ClipboardStore m_store;
    ApiServer m_server;
    QClipboard *m_clipboard = nullptr;
    QString m_status;
    QString m_token;
    QString m_deviceId;
    QString m_latestContent;
    bool m_autoPublish = true;
    bool m_started = false;
    qint64 m_ignoreClipboardChangesUntil = 0;
    QString m_ignoredClipboardContent;
    QString m_lastPublishedContent;
#ifdef Q_OS_ANDROID
    QJniObject m_multicastLock;
#endif
};
