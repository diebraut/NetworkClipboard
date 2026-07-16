#pragma once

#include <QNetworkAccessManager>
#include <QJsonObject>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QUdpSocket>
#include <QUrl>
#include <QVariantList>

#include <functional>

class NetworkClipboardClient : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString serverUrl READ serverUrl WRITE setServerUrl NOTIFY serverUrlChanged)
    Q_PROPERTY(QString serverName READ serverName NOTIFY serverNameChanged)
    Q_PROPERTY(QVariantList servers READ servers NOTIFY serversChanged)
    Q_PROPERTY(int selectedServerIndex READ selectedServerIndex NOTIFY selectedServerIndexChanged)
    Q_PROPERTY(bool serverActive READ serverActive NOTIFY serverActiveChanged)
    Q_PROPERTY(bool selectedServerMain READ selectedServerMain NOTIFY serversChanged)
    Q_PROPERTY(bool manualServerSelection READ manualServerSelection WRITE setManualServerSelection NOTIFY manualServerSelectionChanged)
    Q_PROPERTY(bool discoveryInProgress READ discoveryInProgress NOTIFY discoveryProgressChanged)
    Q_PROPERTY(int discoveryCompleted READ discoveryCompleted NOTIFY discoveryProgressChanged)
    Q_PROPERTY(int discoveryTotal READ discoveryTotal NOTIFY discoveryProgressChanged)
    Q_PROPERTY(QString token READ token WRITE setToken NOTIFY tokenChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)

public:
    explicit NetworkClipboardClient(QObject *parent = nullptr);

    QString serverUrl() const;
    void setServerUrl(const QString &serverUrl);

    QString serverName() const;
    QVariantList servers() const;
    int selectedServerIndex() const;
    bool serverActive() const;
    bool selectedServerMain() const;
    bool manualServerSelection() const;
    void setManualServerSelection(bool enabled);
    bool discoveryInProgress() const;
    int discoveryCompleted() const;
    int discoveryTotal() const;

    QString token() const;
    void setToken(const QString &token);

    QString status() const;

    Q_INVOKABLE void sendText(const QString &text, const QString &deviceName);
    Q_INVOKABLE void sendImage(const QString &base64Png,
                               const QString &fingerprint,
                               const QString &deviceName);
    Q_INVOKABLE void getLatest();
    Q_INVOKABLE void pollLatest();
    Q_INVOKABLE void forcePollLatest();
    Q_INVOKABLE void discoverServer();
    Q_INVOKABLE void connectToServerUrl(const QString &serverUrl);
    Q_INVOKABLE void selectServer(int index);

signals:
    void serverUrlChanged();
    void serverNameChanged();
    void serversChanged();
    void selectedServerIndexChanged();
    void serverActiveChanged();
    void manualServerSelectionChanged();
    void discoveryProgressChanged();
    void tokenChanged();
    void statusChanged();
    void latestReceived(const QString &text);
    void latestImageReceived(const QString &base64Png);
    void latestReadEmpty();
    void latestReadFailed();
    void imageSent(const QString &fingerprint);
    void imageSendFailed(const QString &fingerprint);

private:
    QString normalizedServerUrl(QString *errorMessage = nullptr) const;
    QNetworkRequest request(const QString &path) const;
    QNetworkRequest discoveryRequest(const QString &serverUrl) const;
    void withAvailableServer(const QString &actionStatus, const std::function<void(const QString &serverUrl)> &action);
    void handleDiscoveryResponse();
    void startInitialServerDiscovery();
    bool sendDiscoveryDatagrams();
    void probeDiscoveryUrl(const QUrl &url, bool networkScan = false);
    void startHttpDiscovery();
    void startNetworkScan();
    void launchNextNetworkScanProbe();
    void clearDiscoveredServers(bool keepSelectedServer);
    void addDiscoveredServer(const QJsonObject &object, const QString &fallbackUrl = {});
    void resolveServerName(int index, const QString &host);
    void checkSelectedServer();
    void checkKnownServers();
    void finishKnownServerCheck();
    void activateServer(int index);
    void loadSavedServer();
    void saveKnownServers();
    void saveSelectedServer();
    void setServerActive(bool active);
    void updateServerName(const QString &serverName, const QString &serverUrl);
    void setStatus(const QString &status);
    void pollLatest(bool force);
    void handleClipboardEntry(const QJsonObject &object, bool force = false);

    QNetworkAccessManager m_network;
    QTimer m_serverCheckTimer;
    QUdpSocket m_discoverySocket;
    QSet<QString> m_pendingDiscoveryUrls;
    QQueue<QUrl> m_networkScanQueue;
    QVariantList m_servers;
    int m_selectedServerIndex = -1;
    int m_missedServerChecks = 0;
    bool m_serverCheckInFlight = false;
    bool m_knownServerCheckInFlight = false;
    bool m_latestRequestInFlight = false;
    bool m_forceLatestAfterInFlight = false;
    bool m_serverActive = false;
    bool m_manualServerSelection = false;
    bool m_discoveryInProgress = false;
    int m_networkScanPending = 0;
    int m_networkScanCompleted = 0;
    int m_networkScanTotal = 0;
    int m_knownServerCheckPending = 0;
    int m_knownServerMisses = 0;
    bool m_knownServerCheckFoundActive = false;
    bool m_knownServerListChanged = false;
    QString m_serverUrl;
    QString m_serverName;
    QString m_token;
    QString m_status = QStringLiteral("Ready");
    QString m_latestEntryId;
    QString m_pendingImageEntryId;
};
