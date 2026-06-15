#include "NetworkClipboardClient.h"

#include <QAbstractSocket>
#include <QDateTime>
#include <QHostInfo>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QVariant>
#include <QVariantMap>

namespace {
constexpr quint16 DiscoveryPort = 8788;
constexpr quint16 ApiPort = 8787;
constexpr auto DiscoveryRequest = "NETWORK_CLIPBOARD_DISCOVER_V1";

QSet<QString> localSubnetHosts()
{
    QSet<QString> hosts;

    const auto interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &networkInterface : interfaces) {
        const auto flags = networkInterface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp)
            || !flags.testFlag(QNetworkInterface::IsRunning)
            || flags.testFlag(QNetworkInterface::IsLoopBack)) {
            continue;
        }

        const auto entries = networkInterface.addressEntries();
        for (const QNetworkAddressEntry &entry : entries) {
            const QHostAddress address = entry.ip();
            if (address.protocol() != QAbstractSocket::IPv4Protocol || address.isLoopback())
                continue;

            const quint32 ip = address.toIPv4Address();
            const quint32 base = ip & 0xFFFFFF00u;
            for (quint32 host = 1; host < 255; ++host) {
                const quint32 candidate = base | host;
                if (candidate == ip)
                    continue;
                hosts.insert(QHostAddress(candidate).toString());
            }
        }
    }

    const QStringList commonLanPrefixes{
        QStringLiteral("192.168.68."),
        QStringLiteral("192.168.178."),
        QStringLiteral("192.168.1."),
        QStringLiteral("192.168.0.")
    };
    for (const QString &prefix : commonLanPrefixes) {
        for (int host = 1; host < 255; ++host)
            hosts.insert(QStringLiteral("%1%2").arg(prefix).arg(host));
    }

    return hosts;
}

QString replyErrorMessage(QNetworkReply *reply)
{
    const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QJsonObject object = QJsonDocument::fromJson(reply->readAll()).object();
    const QString serverMessage = object.value(QStringLiteral("error")).toString();

    if (!serverMessage.isEmpty() && statusCode > 0)
        return QStringLiteral("%1: %2").arg(statusCode).arg(serverMessage);
    if (!serverMessage.isEmpty())
        return serverMessage;
    if (statusCode > 0)
        return QStringLiteral("%1: %2").arg(statusCode).arg(reply->errorString());
    return QStringLiteral("%1: %2").arg(reply->errorString(), reply->url().toString());
}

QString displayNameForServer(const QString &serverName, const QString &serverUrl)
{
    QString displayName = serverName.trimmed();
    if (displayName.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive)
        || displayName.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive)) {
        displayName = QUrl(displayName).host();
    }

    if (displayName.isEmpty())
        displayName = QUrl(serverUrl).host();
    if (displayName.isEmpty())
        displayName = QStringLiteral("Clipboard-Server");

    return displayName;
}
}

NetworkClipboardClient::NetworkClipboardClient(QObject *parent)
    : QObject(parent)
{
    connect(&m_discoverySocket, &QUdpSocket::readyRead, this, &NetworkClipboardClient::handleDiscoveryResponse);
    loadSavedServer();
    m_serverCheckTimer.setInterval(2000);
    connect(&m_serverCheckTimer, &QTimer::timeout, this, &NetworkClipboardClient::checkSelectedServer);
    m_serverCheckTimer.start();
    QTimer::singleShot(0, this, &NetworkClipboardClient::startInitialServerDiscovery);
}

QString NetworkClipboardClient::serverUrl() const { return m_serverUrl; }

void NetworkClipboardClient::setServerUrl(const QString &serverUrl)
{
    if (m_serverUrl == serverUrl)
        return;
    m_serverUrl = serverUrl;
    updateServerName({}, m_serverUrl);
    emit serverUrlChanged();
    saveSelectedServer();
}

QString NetworkClipboardClient::serverName() const { return m_serverName; }

QVariantList NetworkClipboardClient::servers() const { return m_servers; }

int NetworkClipboardClient::selectedServerIndex() const { return m_selectedServerIndex; }

bool NetworkClipboardClient::serverActive() const { return m_serverActive; }

QString NetworkClipboardClient::token() const { return m_token; }

void NetworkClipboardClient::setToken(const QString &token)
{
    if (m_token == token)
        return;
    m_token = token;
    emit tokenChanged();
}

QString NetworkClipboardClient::status() const { return m_status; }

void NetworkClipboardClient::sendText(const QString &text, const QString &deviceName)
{
    const QString content = text.trimmed();
    if (content.isEmpty()) {
        setStatus(QStringLiteral("Clipboard is empty."));
        return;
    }

    QJsonObject body{
        {QStringLiteral("deviceId"), QStringLiteral("mobile-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces))},
        {QStringLiteral("deviceName"), deviceName},
        {QStringLiteral("type"), content.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive) || content.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive) ? QStringLiteral("url") : QStringLiteral("text")},
        {QStringLiteral("content"), content},
        {QStringLiteral("timestamp"), QDateTime::currentSecsSinceEpoch()}
    };

    withAvailableServer(QStringLiteral("Prüfe Server vor dem Senden..."), [this, body](const QString &) {
        setStatus(QStringLiteral("Sende an %1").arg(m_serverName));

        QNetworkReply *reply = m_network.post(request(QStringLiteral("/api/clipboard")), QJsonDocument(body).toJson(QJsonDocument::Compact));
        connect(reply, &QNetworkReply::finished, this, [this, reply]() {
            setStatus(reply->error() == QNetworkReply::NoError
                ? QStringLiteral("Sent.")
                : replyErrorMessage(reply));
            reply->deleteLater();
        });
    });
}

void NetworkClipboardClient::getLatest()
{
    withAvailableServer(QStringLiteral("Prüfe Server vor dem Empfangen..."), [this](const QString &) {
        setStatus(QStringLiteral("Empfange von %1").arg(m_serverName));

        QNetworkReply *reply = m_network.get(request(QStringLiteral("/api/clipboard/latest")));
        connect(reply, &QNetworkReply::finished, this, [this, reply]() {
            if (reply->error() != QNetworkReply::NoError) {
                setStatus(replyErrorMessage(reply));
                reply->deleteLater();
                return;
            }

            const QJsonObject object = QJsonDocument::fromJson(reply->readAll()).object();
            emit latestReceived(object.value(QStringLiteral("content")).toString());
            setStatus(QStringLiteral("Received."));
            reply->deleteLater();
        });
    });
}

void NetworkClipboardClient::pollLatest()
{
    if (m_latestRequestInFlight || !m_serverActive)
        return;

    const QString serverUrl = normalizedServerUrl();
    if (serverUrl.isEmpty() || m_token.trimmed().isEmpty())
        return;

    m_latestRequestInFlight = true;

    QNetworkReply *reply = m_network.get(request(QStringLiteral("/api/clipboard/latest")));
    QTimer::singleShot(2500, reply, [reply]() {
        if (reply->isRunning())
            reply->abort();
    });

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        m_latestRequestInFlight = false;

        if (reply->error() == QNetworkReply::NoError) {
            const QJsonObject object = QJsonDocument::fromJson(reply->readAll()).object();
            emit latestReceived(object.value(QStringLiteral("content")).toString());
        }

        reply->deleteLater();
    });
}

void NetworkClipboardClient::discoverServer()
{
    clearDiscoveredServers(true);
    setStatus(QStringLiteral("Suche Clipboard-Server..."));
    const qint64 bytesWritten = m_discoverySocket.writeDatagram(DiscoveryRequest, QHostAddress::Broadcast, DiscoveryPort);
    if (bytesWritten < 0) {
        setStatus(QStringLiteral("UDP-Suche fehlgeschlagen, versuche HTTP..."));
    }

    startHttpDiscovery();

    QTimer::singleShot(4000, this, [this]() {
        if (m_servers.isEmpty() && m_status.startsWith(QStringLiteral("Suche Clipboard-Server")))
            setStatus(QStringLiteral("Kein Clipboard-Server gefunden."));
    });
}

void NetworkClipboardClient::selectServer(int index)
{
    if (index < 0 || index >= m_servers.size())
        return;

    const QVariantMap server = m_servers.at(index).toMap();
    const QString url = server.value(QStringLiteral("url")).toString();
    if (url.isEmpty())
        return;

    const QString name = server.value(QStringLiteral("name")).toString();
    const QString serverToken = server.value(QStringLiteral("token")).toString();

    if (m_selectedServerIndex != index) {
        m_selectedServerIndex = index;
        emit selectedServerIndexChanged();
    }

    if (m_serverUrl != url) {
        m_serverUrl = url;
        emit serverUrlChanged();
    }
    setServerActive(false);
    updateServerName(name, url);
    if (!serverToken.isEmpty())
        setToken(serverToken);
    saveSelectedServer();
    checkSelectedServer();
}

QNetworkRequest NetworkClipboardClient::request(const QString &path) const
{
    QNetworkRequest request(QUrl(normalizedServerUrl() + path));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Authorization", QByteArray("Bearer ") + m_token.toUtf8());
    return request;
}

QString NetworkClipboardClient::normalizedServerUrl(QString *errorMessage) const
{
    QString value = m_serverUrl.trimmed();
    while (value.endsWith(QLatin1Char('/')))
        value.chop(1);

    if (value.isEmpty()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Server URL fehlt.");
        return {};
    }

    const QUrl url(value);
    if (!url.isValid() || url.scheme().isEmpty() || url.host().isEmpty()
        || (url.scheme() != QStringLiteral("http") && url.scheme() != QStringLiteral("https"))) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Ungültige Server URL.");
        return {};
    }

    return value;
}

QNetworkRequest NetworkClipboardClient::discoveryRequest(const QString &serverUrl) const
{
    QNetworkRequest request(QUrl(serverUrl + QStringLiteral("/api/discovery")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setRawHeader("Connection", "close");
    return request;
}

void NetworkClipboardClient::withAvailableServer(const QString &actionStatus, const std::function<void(const QString &serverUrl)> &action)
{
    QString errorMessage;
    const QString serverUrl = normalizedServerUrl(&errorMessage);
    if (serverUrl.isEmpty()) {
        setStatus(errorMessage);
        return;
    }

    setStatus(actionStatus);

    QNetworkReply *reply = m_network.get(discoveryRequest(serverUrl));
    QTimer::singleShot(2500, reply, [reply]() {
        if (reply->isRunning())
            reply->abort();
    });

    connect(reply, &QNetworkReply::finished, this, [this, reply, serverUrl, action]() {
        if (reply->error() != QNetworkReply::NoError) {
            setStatus(QStringLiteral("Server nicht erreichbar: %1").arg(reply->errorString()));
            reply->deleteLater();
            return;
        }

        const QJsonObject object = QJsonDocument::fromJson(reply->readAll()).object();
        if (object.value(QStringLiteral("service")).toString() != QStringLiteral("NetworkClipboard")) {
            setStatus(QStringLiteral("Kein gültiger Network-Clipboard-Server."));
            reply->deleteLater();
            return;
        }

        const QString discoveredToken = object.value(QStringLiteral("token")).toString();
        if (!discoveredToken.isEmpty())
            setToken(discoveredToken);
        updateServerName(object.value(QStringLiteral("serverName")).toString(), serverUrl);

        if (!object.contains(QStringLiteral("agentActive"))) {
            setServerActive(false);
            setStatus(QStringLiteral("Windows-Dienst muss aktualisiert werden."));
            reply->deleteLater();
            return;
        }

        if (!object.value(QStringLiteral("agentActive")).toBool(false)) {
            setServerActive(false);
            setStatus(QStringLiteral("Windows Tray-Agent ist nicht aktiv."));
            reply->deleteLater();
            return;
        }

        if (m_token.trimmed().isEmpty()) {
            setStatus(QStringLiteral("API token fehlt."));
            reply->deleteLater();
            return;
        }

        reply->deleteLater();
        action(serverUrl);
    });
}

void NetworkClipboardClient::handleDiscoveryResponse()
{
    while (m_discoverySocket.hasPendingDatagrams()) {
        const QNetworkDatagram datagram = m_discoverySocket.receiveDatagram();
        const QJsonObject object = QJsonDocument::fromJson(datagram.data()).object();
        if (object.value(QStringLiteral("service")).toString() != QStringLiteral("NetworkClipboard"))
            continue;

        addDiscoveredServer(object);
    }
}

void NetworkClipboardClient::startInitialServerDiscovery()
{
    if (m_serverUrl.isEmpty()) {
        discoverServer();
    } else {
        checkSelectedServer();
    }

    QTimer::singleShot(3000, this, [this]() {
        if (!m_serverActive)
            discoverServer();
    });
}

void NetworkClipboardClient::probeDiscoveryUrl(const QUrl &url)
{
    const QString urlString = url.toString();
    m_pendingDiscoveryUrls.insert(urlString);

    QNetworkReply *reply = m_network.get(QNetworkRequest(url));
    QTimer::singleShot(900, reply, [reply]() {
        if (reply->isRunning())
            reply->abort();
    });

    connect(reply, &QNetworkReply::finished, this, [this, reply, urlString]() {
        m_pendingDiscoveryUrls.remove(urlString);

        if (reply->error() == QNetworkReply::NoError) {
            const QJsonObject object = QJsonDocument::fromJson(reply->readAll()).object();
            if (object.value(QStringLiteral("service")).toString() == QStringLiteral("NetworkClipboard")) {
                addDiscoveredServer(object);
            }
        }

        if (m_pendingDiscoveryUrls.isEmpty() && m_status.startsWith(QStringLiteral("Suche Clipboard-Server"))) {
            if (m_servers.isEmpty()) {
                setStatus(QStringLiteral("Kein Clipboard-Server gefunden."));
            } else if (m_serverActive && !m_serverName.isEmpty()) {
                setStatus(QStringLiteral("Server aktiv: %1").arg(m_serverName));
            } else {
                setStatus(QStringLiteral("Serverliste aktualisiert."));
            }
        }

        reply->deleteLater();
    });
}

void NetworkClipboardClient::startHttpDiscovery()
{
    m_pendingDiscoveryUrls.clear();

    const QSet<QString> hosts = localSubnetHosts();
    setStatus(QStringLiteral("Suche Clipboard-Server (%1 Adressen)...").arg(hosts.size()));
    for (const QString &host : hosts) {
        probeDiscoveryUrl(QUrl(QStringLiteral("http://%1:%2/api/discovery").arg(host).arg(ApiPort)));
    }
}

void NetworkClipboardClient::clearDiscoveredServers(bool keepSelectedServer)
{
    if (m_servers.isEmpty() && m_selectedServerIndex == -1 && m_serverUrl.isEmpty() && m_serverName.isEmpty() && m_token.isEmpty())
        return;

    const QVariantMap selectedServer = (keepSelectedServer && m_selectedServerIndex >= 0 && m_selectedServerIndex < m_servers.size())
        ? m_servers.at(m_selectedServerIndex).toMap()
        : QVariantMap{};
    const QString selectedUrl = keepSelectedServer ? m_serverUrl : QString{};
    const QString selectedName = keepSelectedServer ? m_serverName : QString{};
    const QString selectedToken = keepSelectedServer ? m_token : QString{};

    m_servers.clear();
    m_selectedServerIndex = -1;

    if (keepSelectedServer && !selectedUrl.isEmpty()) {
        QVariantMap server = selectedServer;
        if (server.value(QStringLiteral("url")).toString().isEmpty())
            server.insert(QStringLiteral("url"), selectedUrl);
        if (server.value(QStringLiteral("name")).toString().isEmpty())
            server.insert(QStringLiteral("name"), displayNameForServer(selectedName, selectedUrl));
        if (server.value(QStringLiteral("token")).toString().isEmpty())
            server.insert(QStringLiteral("token"), selectedToken);
        m_servers.append(server);
        m_selectedServerIndex = 0;
    } else {
        m_serverUrl.clear();
        m_serverName.clear();
        m_token.clear();
        setServerActive(false);
    }

    emit serversChanged();
    emit selectedServerIndexChanged();
    if (!keepSelectedServer) {
        emit serverUrlChanged();
        emit serverNameChanged();
        emit tokenChanged();
    }
}

void NetworkClipboardClient::addDiscoveredServer(const QJsonObject &object)
{
    const QString url = object.value(QStringLiteral("url")).toString().trimmed();
    if (url.isEmpty())
        return;

    const QString discoveredName = object.value(QStringLiteral("serverName")).toString();
    const QString name = displayNameForServer(discoveredName, url);
    const bool hasAgentStatus = object.contains(QStringLiteral("agentActive"));
    const bool agentActive = object.value(QStringLiteral("agentActive")).toBool(false);

    for (int i = 0; i < m_servers.size(); ++i) {
        QVariantMap existing = m_servers.at(i).toMap();
        if (existing.value(QStringLiteral("url")).toString() == url) {
            const QString existingName = existing.value(QStringLiteral("name")).toString();
            const QString existingToken = existing.value(QStringLiteral("token")).toString();
            const QString discoveredToken = object.value(QStringLiteral("token")).toString();
            const bool shouldSelectServer = m_selectedServerIndex == -1
                || m_selectedServerIndex == i
                || !m_serverActive
                || (!m_serverUrl.isEmpty() && m_serverUrl == url);
            bool changed = false;

            if (existingName != name && !discoveredName.trimmed().isEmpty()) {
                existing.insert(QStringLiteral("name"), name);
                changed = true;
            }
            if (!discoveredToken.isEmpty() && existingToken != discoveredToken) {
                existing.insert(QStringLiteral("token"), discoveredToken);
                changed = true;
            }

            if (changed) {
                m_servers[i] = existing;
                emit serversChanged();
                if (m_selectedServerIndex == i) {
                    updateServerName(existing.value(QStringLiteral("name")).toString(), url);
                    if (!existing.value(QStringLiteral("token")).toString().isEmpty())
                        setToken(existing.value(QStringLiteral("token")).toString());
                }
            }
            if (shouldSelectServer) {
                if (m_selectedServerIndex != i) {
                    m_selectedServerIndex = i;
                    emit selectedServerIndexChanged();
                }
                if (m_serverUrl != url) {
                    m_serverUrl = url;
                    emit serverUrlChanged();
                }
                updateServerName(existing.value(QStringLiteral("name")).toString(), url);
                if (!existing.value(QStringLiteral("token")).toString().isEmpty())
                    setToken(existing.value(QStringLiteral("token")).toString());
                setServerActive(agentActive);
                saveSelectedServer();
                setStatus(agentActive
                              ? QStringLiteral("Server aktiv: %1").arg(m_serverName)
                              : (hasAgentStatus
                                     ? QStringLiteral("Server gefunden, Windows Tray-Agent nicht aktiv: %1").arg(m_serverName)
                                     : QStringLiteral("Server gefunden, Windows-Dienst muss aktualisiert werden: %1").arg(m_serverName)));
            }
            return;
        }
    }

    QVariantMap server{
        {QStringLiteral("name"), name},
        {QStringLiteral("url"), url},
        {QStringLiteral("token"), object.value(QStringLiteral("token")).toString()}
    };
    m_servers.append(server);
    emit serversChanged();

    const int newServerIndex = static_cast<int>(m_servers.size() - 1);
    if (discoveredName.trimmed().isEmpty())
        resolveServerName(newServerIndex, QUrl(url).host());

    const bool shouldSelectServer = m_selectedServerIndex == -1
        || !m_serverActive
        || (!m_serverUrl.isEmpty() && m_serverUrl == url);
    if (shouldSelectServer) {
        m_selectedServerIndex = newServerIndex;
        emit selectedServerIndexChanged();

        if (m_serverUrl != url) {
            m_serverUrl = url;
            emit serverUrlChanged();
        }
        updateServerName(name, url);

        const QString discoveredToken = object.value(QStringLiteral("token")).toString();
        if (!discoveredToken.isEmpty())
            setToken(discoveredToken);

        setServerActive(agentActive);
        saveSelectedServer();
        setStatus(agentActive
                      ? QStringLiteral("Server aktiv: %1").arg(m_serverName)
                      : (hasAgentStatus
                             ? QStringLiteral("Server gefunden, Windows Tray-Agent nicht aktiv: %1").arg(m_serverName)
                             : QStringLiteral("Server gefunden, Windows-Dienst muss aktualisiert werden: %1").arg(m_serverName)));
    } else {
        setStatus(QStringLiteral("Server gefunden: %1").arg(name));
    }
}

void NetworkClipboardClient::resolveServerName(int index, const QString &host)
{
    if (index < 0 || index >= m_servers.size() || host.isEmpty())
        return;

    const QString url = m_servers.at(index).toMap().value(QStringLiteral("url")).toString();
    QHostInfo::lookupHost(host, this, [this, index, url](const QHostInfo &hostInfo) {
        if (hostInfo.error() != QHostInfo::NoError || hostInfo.hostName().trimmed().isEmpty())
            return;
        if (index < 0 || index >= m_servers.size())
            return;

        QVariantMap server = m_servers.at(index).toMap();
        if (server.value(QStringLiteral("url")).toString() != url)
            return;

        const QString resolvedName = displayNameForServer(hostInfo.hostName(), url);
        if (server.value(QStringLiteral("name")).toString() == resolvedName)
            return;

        server.insert(QStringLiteral("name"), resolvedName);
        m_servers[index] = server;
        emit serversChanged();

        if (m_selectedServerIndex == index)
            updateServerName(resolvedName, url);
    });
}

void NetworkClipboardClient::checkSelectedServer()
{
    if (m_serverCheckInFlight)
        return;

    QString serverUrl;
    if (m_selectedServerIndex >= 0 && m_selectedServerIndex < m_servers.size())
        serverUrl = m_servers.at(m_selectedServerIndex).toMap().value(QStringLiteral("url")).toString();
    if (serverUrl.isEmpty())
        serverUrl = normalizedServerUrl();

    if (serverUrl.isEmpty()) {
        setServerActive(false);
        setStatus(QStringLiteral("Kein Server eingetragen."));
        return;
    }

    m_serverCheckInFlight = true;

    QNetworkReply *reply = m_network.get(discoveryRequest(serverUrl));
    QTimer::singleShot(1200, reply, [reply]() {
        if (reply->isRunning())
            reply->abort();
    });

    connect(reply, &QNetworkReply::finished, this, [this, reply, serverUrl]() {
        const bool active = reply->error() == QNetworkReply::NoError;
        if (active) {
            const QJsonObject object = QJsonDocument::fromJson(reply->readAll()).object();
            const bool validServer = object.value(QStringLiteral("service")).toString() == QStringLiteral("NetworkClipboard");
            if (validServer) {
                m_missedServerChecks = 0;
                addDiscoveredServer(object);
                updateServerName(object.value(QStringLiteral("serverName")).toString(), serverUrl);
                const QString discoveredToken = object.value(QStringLiteral("token")).toString();
                if (!discoveredToken.isEmpty())
                    setToken(discoveredToken);
                setServerActive(object.value(QStringLiteral("agentActive")).toBool(false));
                saveSelectedServer();
            } else {
                ++m_missedServerChecks;
                setServerActive(false);
            }
        } else {
            ++m_missedServerChecks;
            setServerActive(false);
            setStatus(QStringLiteral("Server nicht erreichbar: %1").arg(reply->errorString()));
        }

        m_serverCheckInFlight = false;
        reply->deleteLater();
    });
}

void NetworkClipboardClient::loadSavedServer()
{
    QSettings settings;
    const QString url = settings.value(QStringLiteral("server/url")).toString().trimmed();
    if (url.isEmpty())
        return;

    const QString name = settings.value(QStringLiteral("server/name")).toString();
    const QString token = settings.value(QStringLiteral("server/token")).toString();
    QVariantMap server{
        {QStringLiteral("name"), displayNameForServer(name, url)},
        {QStringLiteral("url"), url},
        {QStringLiteral("token"), token}
    };

    m_servers.append(server);
    m_selectedServerIndex = 0;
    m_serverUrl = url;
    m_serverName = server.value(QStringLiteral("name")).toString();
    m_token = token;

    emit serversChanged();
    emit selectedServerIndexChanged();
    emit serverUrlChanged();
    emit serverNameChanged();
    emit tokenChanged();
}

void NetworkClipboardClient::saveSelectedServer()
{
    QSettings settings;
    settings.setValue(QStringLiteral("server/url"), m_serverUrl);
    settings.setValue(QStringLiteral("server/name"), m_serverName);
    settings.setValue(QStringLiteral("server/token"), m_token);
}

void NetworkClipboardClient::setServerActive(bool active)
{
    if (m_serverActive == active)
        return;

    m_serverActive = active;
    emit serverActiveChanged();
}

void NetworkClipboardClient::updateServerName(const QString &serverName, const QString &serverUrl)
{
    const QString displayName = displayNameForServer(serverName, serverUrl);

    if (m_serverName == displayName)
        return;

    m_serverName = displayName;
    emit serverNameChanged();
}

void NetworkClipboardClient::setStatus(const QString &status)
{
    if (m_status == status)
        return;
    m_status = status;
    emit statusChanged();
}
