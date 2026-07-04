#include "NetworkClipboardClient.h"

#include <QAbstractSocket>
#include <QByteArray>
#include <QDateTime>
#include <QDebug>
#include <QHostInfo>
#include <QHostAddress>
#include <QJsonArray>
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

#include <utility>

namespace {
constexpr quint16 DiscoveryPort = 8788;
constexpr quint16 ApiPort = 8787;
constexpr auto DiscoveryRequest = "NETWORK_CLIPBOARD_DISCOVER_V1";
constexpr qsizetype MaxImageBytes = 10 * 1024 * 1024;
constexpr int LatestRequestTimeoutMs = 90000;

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

bool isLoopbackOrWildcardUrl(const QString &serverUrl)
{
    const QHostAddress address(QUrl(serverUrl).host());
    return !address.isNull()
        && (address.isLoopback() || address == QHostAddress::Any || address == QHostAddress::AnyIPv4);
}

QString endpointServerUrl(QUrl url)
{
    url.setPath({});
    url.setQuery({});
    url.setFragment({});

    QString value = url.toString();
    while (value.endsWith(QLatin1Char('/')))
        value.chop(1);
    return value;
}

QString discoveredServerUrl(const QJsonObject &object, const QString &fallbackUrl)
{
    if (!fallbackUrl.isEmpty())
        return fallbackUrl;

    QString url = object.value(QStringLiteral("url")).toString().trimmed();
    const QJsonArray urls = object.value(QStringLiteral("urls")).toArray();
    for (const QJsonValue &value : urls) {
        const QString candidate = value.toString().trimmed();
        if (!candidate.isEmpty() && !isLoopbackOrWildcardUrl(candidate)) {
            url = candidate;
            break;
        }
    }

    if (!fallbackUrl.isEmpty() && (url.isEmpty() || isLoopbackOrWildcardUrl(url)))
        url = fallbackUrl;

    return url;
}
}

NetworkClipboardClient::NetworkClipboardClient(QObject *parent)
    : QObject(parent)
{
    connect(&m_discoverySocket, &QUdpSocket::readyRead, this, &NetworkClipboardClient::handleDiscoveryResponse);
    loadSavedServer();
    m_serverCheckTimer.setInterval(5000);
    connect(&m_serverCheckTimer, &QTimer::timeout, this, &NetworkClipboardClient::checkKnownServers);
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
bool NetworkClipboardClient::selectedServerMain() const
{
    return m_selectedServerIndex >= 0
        && m_selectedServerIndex < m_servers.size()
        && m_servers.at(m_selectedServerIndex).toMap().value(QStringLiteral("main")).toBool();
}

bool NetworkClipboardClient::manualServerSelection() const { return m_manualServerSelection; }

void NetworkClipboardClient::setManualServerSelection(bool enabled)
{
    if (m_manualServerSelection == enabled)
        return;

    m_manualServerSelection = enabled;
    QSettings settings;
    settings.setValue(QStringLiteral("server/manualSelection"), enabled);
    emit manualServerSelectionChanged();

    if (!enabled)
        QTimer::singleShot(0, this, &NetworkClipboardClient::checkKnownServers);
}

bool NetworkClipboardClient::discoveryInProgress() const { return m_discoveryInProgress; }
int NetworkClipboardClient::discoveryCompleted() const { return m_networkScanCompleted; }
int NetworkClipboardClient::discoveryTotal() const { return m_networkScanTotal; }

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

void NetworkClipboardClient::sendImage(const QString &base64Png,
                                       const QString &fingerprint,
                                       const QString &deviceName)
{
    const QByteArray encoded = base64Png.toLatin1();
    const QByteArray pngData = QByteArray::fromBase64(encoded);
    constexpr char PngSignature[] = "\x89PNG\r\n\x1a\n";
    if (pngData.isEmpty()
        || pngData.size() > MaxImageBytes
        || pngData.toBase64() != encoded
        || !pngData.startsWith(QByteArray(PngSignature, 8))) {
        setStatus(QStringLiteral("Bild ist ungültig oder größer als 10 MB."));
        emit imageSendFailed(fingerprint);
        return;
    }

    QJsonObject body{
        {QStringLiteral("deviceId"), QStringLiteral("mobile-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces))},
        {QStringLiteral("deviceName"), deviceName},
        {QStringLiteral("type"), QStringLiteral("image")},
        {QStringLiteral("mimeType"), QStringLiteral("image/png")},
        {QStringLiteral("content"), base64Png},
        {QStringLiteral("timestamp"), QDateTime::currentSecsSinceEpoch()}
    };

    withAvailableServer(QStringLiteral("Prüfe Server vor dem Senden..."),
                        [this, body, fingerprint](const QString &) {
                            setStatus(QStringLiteral("Sende Bild an %1").arg(m_serverName));
                            QNetworkReply *reply = m_network.post(
                                request(QStringLiteral("/api/clipboard")),
                                QJsonDocument(body).toJson(QJsonDocument::Compact));
                            connect(reply, &QNetworkReply::finished, this, [this, reply, fingerprint]() {
                                if (reply->error() == QNetworkReply::NoError) {
                                    const QJsonObject stored = QJsonDocument::fromJson(reply->readAll()).object();
                                    m_latestEntryId = stored.value(QStringLiteral("id")).toString();
                                    setStatus(QStringLiteral("Bild gesendet."));
                                    emit imageSent(fingerprint);
                                } else {
                                    setStatus(replyErrorMessage(reply));
                                    emit imageSendFailed(fingerprint);
                                }
                                reply->deleteLater();
                            });
                        });
}

void NetworkClipboardClient::getLatest()
{
    withAvailableServer(QStringLiteral("Prüfe Server vor dem Empfangen..."), [this](const QString &) {
        setStatus(QStringLiteral("Empfange von %1").arg(m_serverName));

        QNetworkRequest latestRequest = request(QStringLiteral("/api/clipboard/latest"));
        latestRequest.setRawHeader("Accept", "application/vnd.networkclipboard.meta+json, application/json");
        QNetworkReply *reply = m_network.get(latestRequest);
        connect(reply, &QNetworkReply::finished, this, [this, reply]() {
            if (reply->error() != QNetworkReply::NoError) {
                setStatus(replyErrorMessage(reply));
                reply->deleteLater();
                return;
            }

            const QJsonObject object = QJsonDocument::fromJson(reply->readAll()).object();
            handleClipboardEntry(object);
            setStatus(QStringLiteral("Received."));
            reply->deleteLater();
        });
    });
}

void NetworkClipboardClient::pollLatest()
{
    pollLatest(false);
}

void NetworkClipboardClient::forcePollLatest()
{
    pollLatest(true);
}

void NetworkClipboardClient::pollLatest(bool force)
{
    if (m_latestRequestInFlight)
        return;

    const QString serverUrl = normalizedServerUrl();
    if (serverUrl.isEmpty()) {
        setStatus(QStringLiteral("Server URL fehlt."));
        return;
    }

    if (!m_serverActive) {
        setStatus(QStringLiteral("Kein aktiver Server."));
        return;
    }

    if (m_token.trimmed().isEmpty()) {
        setStatus(QStringLiteral("API token fehlt."));
        return;
    }

    m_latestRequestInFlight = true;

    QNetworkRequest latestRequest = request(QStringLiteral("/api/clipboard/latest"));
    latestRequest.setRawHeader("Accept", "application/vnd.networkclipboard.meta+json, application/json");
    if (!force && !m_latestEntryId.isEmpty())
        latestRequest.setRawHeader("If-None-Match", m_latestEntryId.toUtf8());
    QNetworkReply *reply = m_network.get(latestRequest);
    QTimer::singleShot(LatestRequestTimeoutMs, reply, [reply]() {
        if (reply->isRunning())
            reply->abort();
    });

    connect(reply, &QNetworkReply::finished, this, [this, reply, force]() {
        m_latestRequestInFlight = false;

        if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 304) {
            reply->deleteLater();
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            setStatus(replyErrorMessage(reply));
            reply->deleteLater();
            return;
        }

        const QJsonObject object = QJsonDocument::fromJson(reply->readAll()).object();
        handleClipboardEntry(object, force);
        reply->deleteLater();
    });
}

void NetworkClipboardClient::handleClipboardEntry(const QJsonObject &object, bool force)
{
    const QString id = object.value(QStringLiteral("id")).toString();
    if (!force && !id.isEmpty() && id == m_latestEntryId)
        return;

    const QString type = object.value(QStringLiteral("type")).toString(QStringLiteral("text"));
    if (type == QStringLiteral("image")) {
        const QString mimeType = object.value(QStringLiteral("mimeType")).toString();
        const QString content = object.value(QStringLiteral("content")).toString();
        const QString contentUrl = object.value(QStringLiteral("contentUrl")).toString();
        constexpr char PngSignature[] = "\x89PNG\r\n\x1a\n";

        if (mimeType != QStringLiteral("image/png")) {
            setStatus(QStringLiteral("Server lieferte ein ungueltiges Bild."));
            return;
        }

        if (content.isEmpty() && !contentUrl.isEmpty()) {
            if (!id.isEmpty() && id == m_pendingImageEntryId)
                return;
            m_pendingImageEntryId = id;
            QString imageUrl = contentUrl;
            if (!imageUrl.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive)
                && !imageUrl.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive)) {
                if (!imageUrl.startsWith(QLatin1Char('/')))
                    imageUrl.prepend(QLatin1Char('/'));
                imageUrl.prepend(normalizedServerUrl());
            }

            QNetworkRequest imageRequest{QUrl(imageUrl)};
            imageRequest.setRawHeader("Authorization", QByteArray("Bearer ") + m_token.toUtf8());
            imageRequest.setRawHeader("Accept", "image/png");
            QNetworkReply *reply = m_network.get(imageRequest);
            QTimer::singleShot(LatestRequestTimeoutMs, reply, [reply]() {
                if (reply->isRunning())
                    reply->abort();
            });
            connect(reply, &QNetworkReply::finished, this, [this, reply, id]() {
                if (m_pendingImageEntryId == id)
                    m_pendingImageEntryId.clear();
                if (reply->error() != QNetworkReply::NoError) {
                    setStatus(replyErrorMessage(reply));
                    reply->deleteLater();
                    return;
                }

                const QByteArray pngData = reply->readAll();
                constexpr char PngSignature[] = "\x89PNG\r\n\x1a\n";
                if (pngData.isEmpty()
                    || pngData.size() > MaxImageBytes
                    || !pngData.startsWith(QByteArray(PngSignature, 8))) {
                    setStatus(QStringLiteral("Server lieferte ein ungueltiges Bild."));
                    reply->deleteLater();
                    return;
                }

                if (!id.isEmpty())
                    m_latestEntryId = id;
                setStatus(QStringLiteral("Bild empfangen: %1 KB.").arg((pngData.size() + 1023) / 1024));
                emit latestImageReceived(QString::fromLatin1(pngData.toBase64()));
                reply->deleteLater();
            });
            return;
        }

        const QByteArray encoded = content.toLatin1();
        const QByteArray pngData = QByteArray::fromBase64(encoded);
        if (pngData.isEmpty()
            || pngData.size() > MaxImageBytes
            || !pngData.startsWith(QByteArray(PngSignature, 8))) {
            setStatus(QStringLiteral("Server lieferte ein ungueltiges Bild."));
            return;
        }

        setStatus(QStringLiteral("Bild empfangen: %1 KB.").arg((pngData.size() + 1023) / 1024));
        if (!id.isEmpty()) {
            m_latestEntryId = id;
            if (m_pendingImageEntryId == id)
                m_pendingImageEntryId.clear();
        }
        emit latestImageReceived(content);
        return;
    }

    if (!id.isEmpty())
        m_latestEntryId = id;
    emit latestReceived(object.value(QStringLiteral("content")).toString());
}

void NetworkClipboardClient::discoverServer()
{
    if (m_discoveryInProgress) {
        setStatus(QStringLiteral("Serversuche läuft bereits."));
        return;
    }
    m_discoveryInProgress = true;
    m_networkScanQueue.clear();
    m_networkScanPending = 0;
    m_networkScanCompleted = 0;
    m_networkScanTotal = 0;
    emit discoveryProgressChanged();
    setStatus(QStringLiteral("Suche Clipboard-Server..."));
    if (!sendDiscoveryDatagrams()) {
        setStatus(QStringLiteral("UDP-Suche fehlgeschlagen, versuche HTTP..."));
    }
    QTimer::singleShot(400, this, [this]() { sendDiscoveryDatagrams(); });
    QTimer::singleShot(900, this, [this]() { sendDiscoveryDatagrams(); });

    startHttpDiscovery();
    QTimer::singleShot(1200, this, &NetworkClipboardClient::startNetworkScan);
}

void NetworkClipboardClient::connectToServerUrl(const QString &serverUrl)
{
    QString value = serverUrl.trimmed();
    if (value.isEmpty()) {
        setStatus(QStringLiteral("Server URL fehlt."));
        return;
    }

    if (!value.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive)
        && !value.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive)) {
        value.prepend(QStringLiteral("http://"));
    }

    while (value.endsWith(QLatin1Char('/')))
        value.chop(1);

    const QUrl url(value);
    if (!url.isValid() || url.scheme().isEmpty() || url.host().isEmpty()
        || (url.scheme() != QStringLiteral("http") && url.scheme() != QStringLiteral("https"))) {
        setStatus(QStringLiteral("Ungültige Server URL."));
        return;
    }

    const QString name = displayNameForServer({}, value);
    int index = -1;
    for (int i = 0; i < m_servers.size(); ++i) {
        if (m_servers.at(i).toMap().value(QStringLiteral("url")).toString() == value) {
            index = i;
            break;
        }
    }

    if (index < 0) {
        m_servers.append(QVariantMap{
            {QStringLiteral("name"), name},
            {QStringLiteral("url"), value},
            {QStringLiteral("token"), m_token},
            {QStringLiteral("main"), false},
            {QStringLiteral("active"), false}
        });
        index = m_servers.size() - 1;
        emit serversChanged();
        saveKnownServers();
    }

    m_selectedServerIndex = index;
    emit selectedServerIndexChanged();

    if (m_serverUrl != value) {
        m_serverUrl = value;
        emit serverUrlChanged();
    }

    setServerActive(false);
    updateServerName(name, value);
    saveSelectedServer();
    setStatus(QStringLiteral("Prüfe Server: %1").arg(value));
    checkSelectedServer();
}

bool NetworkClipboardClient::sendDiscoveryDatagrams()
{
    if (m_discoverySocket.state() == QAbstractSocket::UnconnectedState
        && !m_discoverySocket.bind(QHostAddress::AnyIPv4, 0)) {
        qWarning() << "NetworkClipboardIOS: could not bind UDP discovery socket:"
                   << m_discoverySocket.errorString();
        return false;
    }

    bool wroteDatagram = m_discoverySocket.writeDatagram(DiscoveryRequest, QHostAddress::Broadcast, DiscoveryPort) >= 0;

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
            const QHostAddress broadcast = entry.broadcast();
            if (broadcast.protocol() != QAbstractSocket::IPv4Protocol || broadcast.isNull())
                continue;

            wroteDatagram = m_discoverySocket.writeDatagram(DiscoveryRequest, broadcast, DiscoveryPort) >= 0
                || wroteDatagram;
        }
    }

    if (!wroteDatagram) {
        qWarning() << "NetworkClipboardIOS: could not send UDP discovery datagram:"
                   << m_discoverySocket.errorString();
    }

    return wroteDatagram;
}

void NetworkClipboardClient::selectServer(int index)
{
    if (!m_manualServerSelection || index < 0 || index >= m_servers.size())
        return;

    const QVariantMap server = m_servers.at(index).toMap();
    if (!server.value(QStringLiteral("active")).toBool())
        return;
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

        if (object.contains(QStringLiteral("agentActive")) && !object.value(QStringLiteral("agentActive")).toBool(false)) {
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

        const QString fallbackUrl = QStringLiteral("http://%1:%2").arg(datagram.senderAddress().toString()).arg(ApiPort);
        addDiscoveredServer(object, fallbackUrl);
    }
}

void NetworkClipboardClient::startInitialServerDiscovery()
{
    if (m_servers.isEmpty())
        setStatus(QStringLiteral("Suche Clipboard-Server..."));

    sendDiscoveryDatagrams();
    QTimer::singleShot(400, this, [this]() { sendDiscoveryDatagrams(); });
    QTimer::singleShot(900, this, [this]() { sendDiscoveryDatagrams(); });

    if (!m_servers.isEmpty()) {
        checkKnownServers();
    } else {
        setStatus(QStringLiteral("Kein gespeicherter Server. Warte auf UDP-Antwort."));
    }
}

void NetworkClipboardClient::probeDiscoveryUrl(const QUrl &url, bool networkScan)
{
    const QString urlString = url.toString();
    m_pendingDiscoveryUrls.insert(urlString);

    QNetworkReply *reply = m_network.get(QNetworkRequest(url));
    QTimer::singleShot(networkScan ? 700 : 1500, reply, [reply]() {
        if (reply->isRunning())
            reply->abort();
    });

    connect(reply, &QNetworkReply::finished, this, [this, reply, urlString, networkScan]() {
        m_pendingDiscoveryUrls.remove(urlString);

        if (reply->error() == QNetworkReply::NoError) {
            const QJsonObject object = QJsonDocument::fromJson(reply->readAll()).object();
            if (object.value(QStringLiteral("service")).toString() == QStringLiteral("NetworkClipboard")) {
                addDiscoveredServer(object, endpointServerUrl(reply->url()));
            }
        }

        if (networkScan) {
            --m_networkScanPending;
            ++m_networkScanCompleted;
            emit discoveryProgressChanged();
            launchNextNetworkScanProbe();
        } else if (m_pendingDiscoveryUrls.isEmpty() && m_status.startsWith(QStringLiteral("Suche Clipboard-Server"))) {
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
    for (const QVariant &serverValue : std::as_const(m_servers)) {
        const QString serverUrl = serverValue.toMap().value(QStringLiteral("url")).toString();
        if (!serverUrl.isEmpty())
            probeDiscoveryUrl(QUrl(serverUrl + QStringLiteral("/api/discovery")));
    }
}

void NetworkClipboardClient::startNetworkScan()
{
    QSet<QString> prefixes;
    const auto interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &networkInterface : interfaces) {
        const auto flags = networkInterface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp)
            || !flags.testFlag(QNetworkInterface::IsRunning)
            || flags.testFlag(QNetworkInterface::IsLoopBack)) {
            continue;
        }

        for (const QNetworkAddressEntry &entry : networkInterface.addressEntries()) {
            const QHostAddress address = entry.ip();
            if (address.protocol() != QAbstractSocket::IPv4Protocol || address.isLoopback())
                continue;

            const QStringList parts = address.toString().split(QLatin1Char('.'));
            if (parts.size() == 4) {
                prefixes.insert(parts.at(0) + QLatin1Char('.')
                                + parts.at(1) + QLatin1Char('.')
                                + parts.at(2) + QLatin1Char('.'));
            }
        }
    }

    QSet<QString> knownUrls;
    for (const QVariant &serverValue : std::as_const(m_servers))
        knownUrls.insert(serverValue.toMap().value(QStringLiteral("url")).toString());

    for (const QString &prefix : std::as_const(prefixes)) {
        for (int host = 1; host < 255; ++host) {
            const QUrl url(QStringLiteral("http://%1%2:%3/api/discovery")
                               .arg(prefix)
                               .arg(host)
                               .arg(ApiPort));
            if (!knownUrls.contains(endpointServerUrl(url)))
                m_networkScanQueue.enqueue(url);
        }
    }

    m_networkScanTotal = m_networkScanQueue.size();
    emit discoveryProgressChanged();
    if (m_networkScanTotal == 0) {
        m_discoveryInProgress = false;
        emit discoveryProgressChanged();
        setStatus(QStringLiteral("Serversuche abgeschlossen."));
        QTimer::singleShot(0, this, &NetworkClipboardClient::checkKnownServers);
        return;
    }

    setStatus(QStringLiteral("Suche Server im lokalen Netzwerk..."));
    launchNextNetworkScanProbe();
}

void NetworkClipboardClient::launchNextNetworkScanProbe()
{
    constexpr int MaxConcurrentProbes = 24;
    while (m_networkScanPending < MaxConcurrentProbes && !m_networkScanQueue.isEmpty()) {
        ++m_networkScanPending;
        probeDiscoveryUrl(m_networkScanQueue.dequeue(), true);
    }

    if (m_networkScanQueue.isEmpty() && m_networkScanPending == 0) {
        m_discoveryInProgress = false;
        emit discoveryProgressChanged();
        setStatus(QStringLiteral("Serversuche abgeschlossen: %1 Server bekannt.").arg(m_servers.size()));
        QTimer::singleShot(0, this, &NetworkClipboardClient::checkKnownServers);
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

void NetworkClipboardClient::addDiscoveredServer(const QJsonObject &object, const QString &fallbackUrl)
{
    const QString url = discoveredServerUrl(object, fallbackUrl);
    if (url.isEmpty())
        return;

    const QString discoveredName = object.value(QStringLiteral("serverName")).toString();
    const QString name = displayNameForServer(discoveredName, url);
    const QString discoveredToken = object.value(QStringLiteral("token")).toString();
    const bool discoveredMaster = object.value(QStringLiteral("isMaster")).toBool(false);
    const bool hasAgentStatus = object.contains(QStringLiteral("agentActive"));
    const bool serverAvailable = !hasAgentStatus || object.value(QStringLiteral("agentActive")).toBool(false);

    int inactiveServersWithSameName = 0;
    if (!name.isEmpty()) {
        for (const QVariant &serverValue : std::as_const(m_servers)) {
            const QVariantMap server = serverValue.toMap();
            if (!server.value(QStringLiteral("active")).toBool()
                && server.value(QStringLiteral("name")).toString() == name) {
                ++inactiveServersWithSameName;
            }
        }
    }

    for (int i = 0; i < m_servers.size(); ++i) {
        QVariantMap existing = m_servers.at(i).toMap();
        const QString existingUrl = existing.value(QStringLiteral("url")).toString();
        const QString existingToken = existing.value(QStringLiteral("token")).toString();
        const bool sameUrl = existingUrl == url;
        const bool sameTokenAndName = !discoveredToken.isEmpty()
            && !existingToken.isEmpty()
            && discoveredToken == existingToken
            && existing.value(QStringLiteral("name")).toString() == name;
        const bool tokensCompatible = existingToken.isEmpty()
            || discoveredToken.isEmpty()
            || discoveredToken == existingToken;
        const bool staleServerWithSameName = !existing.value(QStringLiteral("active")).toBool()
            && !name.isEmpty()
            && inactiveServersWithSameName == 1
            && tokensCompatible
            && existing.value(QStringLiteral("name")).toString() == name;
        if (sameUrl || sameTokenAndName || staleServerWithSameName) {
            const QString existingName = existing.value(QStringLiteral("name")).toString();
            const QString preferredUrl = sameUrl ? existingUrl : url;
            const bool shouldSelectServer = m_selectedServerIndex == -1
                || m_selectedServerIndex == i
                || !m_serverActive
                || (!m_manualServerSelection && discoveredMaster && serverAvailable)
                || (!m_serverUrl.isEmpty() && (m_serverUrl == url || m_serverUrl == existingUrl));
            bool changed = false;

            if (existingUrl != preferredUrl) {
                existing.insert(QStringLiteral("url"), preferredUrl);
                changed = true;
            }
            if (existingName != name && !discoveredName.trimmed().isEmpty()) {
                existing.insert(QStringLiteral("name"), name);
                changed = true;
            }
            if (!discoveredToken.isEmpty() && existingToken != discoveredToken) {
                existing.insert(QStringLiteral("token"), discoveredToken);
                changed = true;
            }
            if (existing.value(QStringLiteral("main")).toBool() != discoveredMaster) {
                existing.insert(QStringLiteral("main"), discoveredMaster);
                changed = true;
            }
            if (existing.value(QStringLiteral("active")).toBool() != serverAvailable) {
                existing.insert(QStringLiteral("active"), serverAvailable);
                changed = true;
            }

            if (changed) {
                m_servers[i] = existing;
                emit serversChanged();
                saveKnownServers();
                if (m_selectedServerIndex == i) {
                    updateServerName(existing.value(QStringLiteral("name")).toString(), preferredUrl);
                    if (!existing.value(QStringLiteral("token")).toString().isEmpty())
                        setToken(existing.value(QStringLiteral("token")).toString());
                }
            }
            if (shouldSelectServer) {
                if (m_selectedServerIndex != i) {
                    m_selectedServerIndex = i;
                    emit selectedServerIndexChanged();
                }
                if (m_serverUrl != preferredUrl) {
                    m_serverUrl = preferredUrl;
                    emit serverUrlChanged();
                }
                updateServerName(existing.value(QStringLiteral("name")).toString(), preferredUrl);
                if (!existing.value(QStringLiteral("token")).toString().isEmpty())
                    setToken(existing.value(QStringLiteral("token")).toString());
                setServerActive(serverAvailable);
                saveSelectedServer();
                setStatus(serverAvailable
                              ? QStringLiteral("Server aktiv: %1").arg(m_serverName)
                              : QStringLiteral("Server gefunden, Windows Tray-Agent nicht aktiv: %1").arg(m_serverName));
            }
            return;
        }
    }

    QVariantMap server{
        {QStringLiteral("name"), name},
        {QStringLiteral("url"), url},
        {QStringLiteral("token"), object.value(QStringLiteral("token")).toString()},
        {QStringLiteral("main"), discoveredMaster},
        {QStringLiteral("active"), serverAvailable}
    };
    m_servers.append(server);
    emit serversChanged();
    saveKnownServers();

    const int newServerIndex = static_cast<int>(m_servers.size() - 1);
    if (discoveredName.trimmed().isEmpty())
        resolveServerName(newServerIndex, QUrl(url).host());

    const bool shouldSelectServer = m_selectedServerIndex == -1
        || !m_serverActive
        || (!m_manualServerSelection && discoveredMaster && serverAvailable)
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

        setServerActive(serverAvailable);
        saveSelectedServer();
        setStatus(serverAvailable
                      ? QStringLiteral("Server aktiv: %1").arg(m_serverName)
                      : QStringLiteral("Server gefunden, Windows Tray-Agent nicht aktiv: %1").arg(m_serverName));
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
        saveKnownServers();

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
                updateServerName(object.value(QStringLiteral("serverName")).toString(), serverUrl);
                const QString discoveredToken = object.value(QStringLiteral("token")).toString();
                if (!discoveredToken.isEmpty())
                    setToken(discoveredToken);
                const bool available = !object.contains(QStringLiteral("agentActive"))
                    || object.value(QStringLiteral("agentActive")).toBool(false);
                if (available) {
                    m_missedServerChecks = 0;
                    addDiscoveredServer(object, serverUrl);
                    setServerActive(true);
                    saveSelectedServer();
                } else if (++m_missedServerChecks >= 3) {
                    setServerActive(false);
                    setStatus(QStringLiteral("Windows Tray-Agent ist nicht aktiv."));
                }
            } else {
                ++m_missedServerChecks;
                if (m_missedServerChecks >= 3) {
                    setServerActive(false);
                    setStatus(QStringLiteral("Server antwortet nicht."));
                }
            }
        } else {
            ++m_missedServerChecks;
            if (m_missedServerChecks >= 3) {
                setServerActive(false);
                setStatus(QStringLiteral("Server nicht erreichbar: %1").arg(reply->errorString()));
            }
        }

        m_serverCheckInFlight = false;
        reply->deleteLater();
    });
}

void NetworkClipboardClient::checkKnownServers()
{
    if (m_knownServerCheckInFlight || m_discoveryInProgress)
        return;
    if (m_servers.isEmpty()) {
        sendDiscoveryDatagrams();
        return;
    }

    m_knownServerCheckInFlight = true;
    m_knownServerCheckPending = m_servers.size();
    m_knownServerCheckFoundActive = false;
    m_knownServerListChanged = false;

    for (const QVariant &serverValue : std::as_const(m_servers)) {
        const QString serverUrl = serverValue.toMap().value(QStringLiteral("url")).toString();
        QNetworkReply *reply = m_network.get(discoveryRequest(serverUrl));
        QTimer::singleShot(900, reply, [reply]() {
            if (reply->isRunning())
                reply->abort();
        });

        connect(reply, &QNetworkReply::finished, this, [this, reply, serverUrl]() {
            bool available = false;
            QJsonObject object;
            if (reply->error() == QNetworkReply::NoError) {
                object = QJsonDocument::fromJson(reply->readAll()).object();
                available = object.value(QStringLiteral("service")).toString() == QStringLiteral("NetworkClipboard")
                    && (!object.contains(QStringLiteral("agentActive"))
                        || object.value(QStringLiteral("agentActive")).toBool(false));
            }

            for (int i = 0; i < m_servers.size(); ++i) {
                QVariantMap server = m_servers.at(i).toMap();
                if (server.value(QStringLiteral("url")).toString() != serverUrl)
                    continue;
                const QVariantMap originalServer = server;
                const bool wasActive = server.value(QStringLiteral("active")).toBool();
                const bool keepVisibleDuringGrace = !available && wasActive && m_knownServerMisses < 2;
                server.insert(QStringLiteral("active"), available || keepVisibleDuringGrace);
                if (available) {
                    m_knownServerCheckFoundActive = true;
                    server.insert(QStringLiteral("main"), object.value(QStringLiteral("isMaster")).toBool(false));
                    const QString name = object.value(QStringLiteral("serverName")).toString();
                    const QString token = object.value(QStringLiteral("token")).toString();
                    if (!name.isEmpty())
                        server.insert(QStringLiteral("name"), displayNameForServer(name, serverUrl));
                    if (!token.isEmpty())
                        server.insert(QStringLiteral("token"), token);
                }
                if (server != originalServer) {
                    m_servers[i] = server;
                    m_knownServerListChanged = true;
                }

                const bool shouldActivateNow = available
                    && (!m_manualServerSelection || i == m_selectedServerIndex)
                    && (server.value(QStringLiteral("main")).toBool()
                        || !m_serverActive);
                if (shouldActivateNow)
                    activateServer(i);
                break;
            }

            --m_knownServerCheckPending;
            reply->deleteLater();
            if (m_knownServerCheckPending == 0)
                finishKnownServerCheck();
        });
    }
}

void NetworkClipboardClient::finishKnownServerCheck()
{
    m_knownServerCheckInFlight = false;
    if (m_knownServerCheckFoundActive) {
        m_knownServerMisses = 0;
    } else {
        ++m_knownServerMisses;
    }
    sendDiscoveryDatagrams();

    int mainServerIndex = -1;
    int firstActiveIndex = -1;
    for (int i = 0; i < m_servers.size(); ++i) {
        const QVariantMap server = m_servers.at(i).toMap();
        if (!server.value(QStringLiteral("active")).toBool())
            continue;
        if (firstActiveIndex < 0)
            firstActiveIndex = i;
        if (server.value(QStringLiteral("main")).toBool()) {
            mainServerIndex = i;
            break;
        }
    }

    const int targetIndex = mainServerIndex >= 0 ? mainServerIndex : firstActiveIndex;
    if (targetIndex < 0 && m_knownServerMisses < 3) {
        setStatus(QStringLiteral("Prüfe bekannte Server..."));
        if (m_knownServerListChanged) {
            emit serversChanged();
            saveKnownServers();
        }
        return;
    }

    if (targetIndex < 0) {
        for (int i = 0; i < m_servers.size(); ++i) {
            QVariantMap server = m_servers.at(i).toMap();
            if (server.value(QStringLiteral("active")).toBool()) {
                server.insert(QStringLiteral("active"), false);
                m_servers[i] = server;
                m_knownServerListChanged = true;
            }
        }
    }

    if (m_knownServerListChanged) {
        emit serversChanged();
        saveKnownServers();
    }

    if (m_manualServerSelection
        && m_selectedServerIndex >= 0
        && m_selectedServerIndex < m_servers.size()
        && m_servers.at(m_selectedServerIndex).toMap().value(QStringLiteral("active")).toBool()) {
        activateServer(m_selectedServerIndex);
        return;
    }

    if (targetIndex >= 0) {
        activateServer(targetIndex);
    } else {
        setServerActive(false);
        setStatus(QStringLiteral("Kein bekannter Server erreichbar."));
    }
}

void NetworkClipboardClient::activateServer(int index)
{
    if (index < 0 || index >= m_servers.size())
        return;

    const QVariantMap server = m_servers.at(index).toMap();
    if (m_selectedServerIndex != index) {
        m_selectedServerIndex = index;
        emit selectedServerIndexChanged();
    }

    const QString url = server.value(QStringLiteral("url")).toString();
    if (m_serverUrl != url) {
        m_serverUrl = url;
        emit serverUrlChanged();
    }
    updateServerName(server.value(QStringLiteral("name")).toString(), url);
    setToken(server.value(QStringLiteral("token")).toString());
    setServerActive(true);
    saveSelectedServer();
    setStatus(QStringLiteral("%1 aktiv: %2")
                  .arg(server.value(QStringLiteral("main")).toBool()
                           ? QStringLiteral("Main-Server")
                           : QStringLiteral("Subserver"),
                       m_serverName));
}

void NetworkClipboardClient::loadSavedServer()
{
    QSettings settings;
    m_manualServerSelection = settings.value(QStringLiteral("server/manualSelection"), false).toBool();
    const QString selectedUrl = settings.value(QStringLiteral("server/url")).toString().trimmed();
    const int serverCount = settings.beginReadArray(QStringLiteral("knownServers"));
    for (int i = 0; i < serverCount; ++i) {
        settings.setArrayIndex(i);
        const QString url = settings.value(QStringLiteral("url")).toString().trimmed();
        if (url.isEmpty())
            continue;
        m_servers.append(QVariantMap{
            {QStringLiteral("name"), displayNameForServer(settings.value(QStringLiteral("name")).toString(), url)},
            {QStringLiteral("url"), url},
            {QStringLiteral("token"), settings.value(QStringLiteral("token")).toString()},
            {QStringLiteral("main"), settings.value(QStringLiteral("main"), false).toBool()},
            {QStringLiteral("active"), false}
        });
    }
    settings.endArray();

    if (m_servers.isEmpty() && !selectedUrl.isEmpty()) {
        m_servers.append(QVariantMap{
            {QStringLiteral("name"), displayNameForServer(settings.value(QStringLiteral("server/name")).toString(), selectedUrl)},
            {QStringLiteral("url"), selectedUrl},
            {QStringLiteral("token"), settings.value(QStringLiteral("server/token")).toString()},
            {QStringLiteral("main"), false},
            {QStringLiteral("active"), false}
        });
    }

    if (m_servers.isEmpty())
        return;

    m_selectedServerIndex = 0;
    for (int i = 0; i < m_servers.size(); ++i) {
        if (m_servers.at(i).toMap().value(QStringLiteral("url")).toString() == selectedUrl) {
            m_selectedServerIndex = i;
            break;
        }
    }

    const QVariantMap server = m_servers.at(m_selectedServerIndex).toMap();
    m_serverUrl = server.value(QStringLiteral("url")).toString();
    m_serverName = server.value(QStringLiteral("name")).toString();
    m_token = server.value(QStringLiteral("token")).toString();

    emit serversChanged();
    emit selectedServerIndexChanged();
    emit serverUrlChanged();
    emit serverNameChanged();
    emit tokenChanged();
}

void NetworkClipboardClient::saveKnownServers()
{
    QSettings settings;
    settings.beginWriteArray(QStringLiteral("knownServers"), m_servers.size());
    for (int i = 0; i < m_servers.size(); ++i) {
        settings.setArrayIndex(i);
        const QVariantMap server = m_servers.at(i).toMap();
        settings.setValue(QStringLiteral("name"), server.value(QStringLiteral("name")));
        settings.setValue(QStringLiteral("url"), server.value(QStringLiteral("url")));
        settings.setValue(QStringLiteral("token"), server.value(QStringLiteral("token")));
        settings.setValue(QStringLiteral("main"), server.value(QStringLiteral("main"), false));
    }
    settings.endArray();
}

void NetworkClipboardClient::saveSelectedServer()
{
    QSettings settings;
    settings.setValue(QStringLiteral("server/url"), m_serverUrl);
    settings.setValue(QStringLiteral("server/name"), m_serverName);
    settings.setValue(QStringLiteral("server/token"), m_token);
    saveKnownServers();
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
