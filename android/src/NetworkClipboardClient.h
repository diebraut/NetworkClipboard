#pragma once

#include <QNetworkAccessManager>
#include <QObject>
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

    QString token() const;
    void setToken(const QString &token);

    QString status() const;

    Q_INVOKABLE void sendText(const QString &text, const QString &deviceName);
    Q_INVOKABLE void getLatest();
    Q_INVOKABLE void discoverServer();
    Q_INVOKABLE void selectServer(int index);

signals:
    void serverUrlChanged();
    void serverNameChanged();
    void serversChanged();
    void selectedServerIndexChanged();
    void serverActiveChanged();
    void tokenChanged();
    void statusChanged();
    void latestReceived(const QString &text);

private:
    QString normalizedServerUrl(QString *errorMessage = nullptr) const;
    QNetworkRequest request(const QString &path) const;
    QNetworkRequest discoveryRequest(const QString &serverUrl) const;
    void withAvailableServer(const QString &actionStatus, const std::function<void(const QString &serverUrl)> &action);
    void handleDiscoveryResponse();
    void probeDiscoveryUrl(const QUrl &url);
    void startHttpDiscovery();
    void clearDiscoveredServers(bool keepSelectedServer);
    void addDiscoveredServer(const QJsonObject &object);
    void resolveServerName(int index, const QString &host);
    void checkSelectedServer();
    void loadSavedServer();
    void saveSelectedServer();
    void setServerActive(bool active);
    void updateServerName(const QString &serverName, const QString &serverUrl);
    void setStatus(const QString &status);

    QNetworkAccessManager m_network;
    QTimer m_serverCheckTimer;
    QUdpSocket m_discoverySocket;
    QSet<QString> m_pendingDiscoveryUrls;
    QVariantList m_servers;
    int m_selectedServerIndex = -1;
    int m_missedServerChecks = 0;
    bool m_serverCheckInFlight = false;
    bool m_serverActive = false;
    QString m_serverUrl;
    QString m_serverName;
    QString m_token;
    QString m_status = QStringLiteral("Ready");
};
