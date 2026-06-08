#include "NetworkClipboardClient.h"

#include <QAbstractSocket>
#include <QDateTime>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QUuid>

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

    return hosts;
}
}

NetworkClipboardClient::NetworkClipboardClient(QObject *parent)
    : QObject(parent)
{
    connect(&m_discoverySocket, &QUdpSocket::readyRead, this, &NetworkClipboardClient::handleDiscoveryResponse);
}

QString NetworkClipboardClient::serverUrl() const { return m_serverUrl; }

void NetworkClipboardClient::setServerUrl(const QString &serverUrl)
{
    if (m_serverUrl == serverUrl)
        return;
    m_serverUrl = serverUrl;
    emit serverUrlChanged();
}

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
    if (m_serverUrl.trimmed().isEmpty()) {
        setStatus(QStringLiteral("Server URL fehlt."));
        return;
    }

    const QString content = text.trimmed();
    if (content.isEmpty()) {
        setStatus(QStringLiteral("Clipboard is empty."));
        return;
    }

    setStatus(QStringLiteral("Sending to %1").arg(m_serverUrl));

    QJsonObject body{
        {QStringLiteral("deviceId"), QStringLiteral("mobile-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces))},
        {QStringLiteral("deviceName"), deviceName},
        {QStringLiteral("type"), content.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive) || content.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive) ? QStringLiteral("url") : QStringLiteral("text")},
        {QStringLiteral("content"), content},
        {QStringLiteral("timestamp"), QDateTime::currentSecsSinceEpoch()}
    };

    QNetworkReply *reply = m_network.post(request(QStringLiteral("/api/clipboard")), QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        setStatus(reply->error() == QNetworkReply::NoError
            ? QStringLiteral("Sent.")
            : QStringLiteral("%1: %2").arg(reply->errorString(), reply->url().toString()));
        reply->deleteLater();
    });
}

void NetworkClipboardClient::getLatest()
{
    if (m_serverUrl.trimmed().isEmpty()) {
        setStatus(QStringLiteral("Server URL fehlt."));
        return;
    }

    setStatus(QStringLiteral("Getting from %1").arg(m_serverUrl));

    QNetworkReply *reply = m_network.get(request(QStringLiteral("/api/clipboard/latest")));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        if (reply->error() != QNetworkReply::NoError) {
            setStatus(QStringLiteral("%1: %2").arg(reply->errorString(), reply->url().toString()));
            reply->deleteLater();
            return;
        }

        const QJsonObject object = QJsonDocument::fromJson(reply->readAll()).object();
        emit latestReceived(object.value(QStringLiteral("content")).toString());
        setStatus(QStringLiteral("Received."));
        reply->deleteLater();
    });
}

void NetworkClipboardClient::discoverServer()
{
    setStatus(QStringLiteral("Suche Clipboard-Server..."));
    const qint64 bytesWritten = m_discoverySocket.writeDatagram(DiscoveryRequest, QHostAddress::Broadcast, DiscoveryPort);
    if (bytesWritten < 0) {
        setStatus(QStringLiteral("UDP-Suche fehlgeschlagen, versuche HTTP..."));
    }

    startHttpDiscovery();

    QTimer::singleShot(4000, this, [this]() {
        if (m_status == QStringLiteral("Suche Clipboard-Server..."))
            setStatus(QStringLiteral("Kein Clipboard-Server gefunden."));
    });
}

QNetworkRequest NetworkClipboardClient::request(const QString &path) const
{
    QNetworkRequest request(QUrl(m_serverUrl + path));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Authorization", QByteArray("Bearer ") + m_token.toUtf8());
    return request;
}

void NetworkClipboardClient::handleDiscoveryResponse()
{
    while (m_discoverySocket.hasPendingDatagrams()) {
        const QNetworkDatagram datagram = m_discoverySocket.receiveDatagram();
        const QJsonObject object = QJsonDocument::fromJson(datagram.data()).object();
        if (object.value(QStringLiteral("service")).toString() != QStringLiteral("NetworkClipboard"))
            continue;

        const QString url = object.value(QStringLiteral("url")).toString();
        const QString discoveredToken = object.value(QStringLiteral("token")).toString();
        if (url.isEmpty())
            continue;

        setServerUrl(url);
        if (!discoveredToken.isEmpty())
            setToken(discoveredToken);

        setStatus(QStringLiteral("Server gefunden: %1").arg(url));
        return;
    }
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
                const QString url = object.value(QStringLiteral("url")).toString();
                const QString discoveredToken = object.value(QStringLiteral("token")).toString();
                if (!url.isEmpty()) {
                    setServerUrl(url);
                    if (!discoveredToken.isEmpty())
                        setToken(discoveredToken);
                    setStatus(QStringLiteral("Server gefunden: %1").arg(url));
                }
            }
        }

        if (m_pendingDiscoveryUrls.isEmpty() && m_status == QStringLiteral("Suche Clipboard-Server..."))
            setStatus(QStringLiteral("Kein Clipboard-Server gefunden."));

        reply->deleteLater();
    });
}

void NetworkClipboardClient::startHttpDiscovery()
{
    m_pendingDiscoveryUrls.clear();

    const QSet<QString> hosts = localSubnetHosts();
    for (const QString &host : hosts) {
        probeDiscoveryUrl(QUrl(QStringLiteral("http://%1:%2/api/discovery").arg(host).arg(ApiPort)));
    }
}

void NetworkClipboardClient::setStatus(const QString &status)
{
    if (m_status == status)
        return;
    m_status = status;
    emit statusChanged();
}
