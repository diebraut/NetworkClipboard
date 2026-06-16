#include "ApiServer.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QTcpSocket>

#ifdef Q_OS_ANDROID
#include <QJniObject>
#include <QtCore/qcoreapplication_platform.h>
#endif

namespace {
constexpr quint16 DiscoveryPort = 8788;
constexpr auto DiscoveryRequest = "NETWORK_CLIPBOARD_DISCOVER_V1";

QByteArray reasonPhrase(int statusCode)
{
    switch (statusCode) {
    case 200: return "OK";
    case 201: return "Created";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 413: return "Payload Too Large";
    default: return "Internal Server Error";
    }
}

#ifdef Q_OS_ANDROID
QString androidWifiAddress()
{
    const QJniObject context = QNativeInterface::QAndroidApplication::context();
    if (!context.isValid())
        return {};

    const QJniObject serviceName = QJniObject::fromString(QStringLiteral("wifi"));
    const QJniObject wifiManager = context.callObjectMethod("getSystemService",
                                                            "(Ljava/lang/String;)Ljava/lang/Object;",
                                                            serviceName.object<jstring>());
    if (!wifiManager.isValid())
        return {};

    const QJniObject connectionInfo = wifiManager.callObjectMethod("getConnectionInfo",
                                                                   "()Landroid/net/wifi/WifiInfo;");
    if (!connectionInfo.isValid())
        return {};

    const jint ip = connectionInfo.callMethod<jint>("getIpAddress", "()I");
    if (ip == 0)
        return {};

    const QString address = QStringLiteral("%1.%2.%3.%4")
                                .arg(ip & 0xff)
                                .arg((ip >> 8) & 0xff)
                                .arg((ip >> 16) & 0xff)
                                .arg((ip >> 24) & 0xff);
    if (address == QStringLiteral("0.0.0.0") || address.startsWith(QStringLiteral("127.")))
        return {};
    return address;
}
#endif

QStringList localServerUrls(quint16 port)
{
    QStringList urls;
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
            const QString ip = address.toString();
            if (!ip.startsWith(QStringLiteral("169.254.")))
                urls.append(QStringLiteral("http://%1:%2").arg(ip).arg(port));
        }
    }

#ifdef Q_OS_ANDROID
    const QString wifiAddress = androidWifiAddress();
    if (!wifiAddress.isEmpty() && !wifiAddress.startsWith(QStringLiteral("169.254.")))
        urls.append(QStringLiteral("http://%1:%2").arg(wifiAddress).arg(port));
#endif

    urls.removeDuplicates();
    return urls;
}

QString subnetMatchedServerUrl(const QHostAddress &peerAddress, quint16 port)
{
    if (peerAddress.protocol() != QAbstractSocket::IPv4Protocol)
        return {};

    const quint32 peer = peerAddress.toIPv4Address();
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
            const QHostAddress netmask = entry.netmask();
            if (address.protocol() != QAbstractSocket::IPv4Protocol || netmask.protocol() != QAbstractSocket::IPv4Protocol)
                continue;

            const quint32 mask = netmask.toIPv4Address();
            if ((address.toIPv4Address() & mask) == (peer & mask))
                return QStringLiteral("http://%1:%2").arg(address.toString()).arg(port);
        }
    }
    return {};
}
}

ApiServer::ApiServer(ClipboardStore *store, QObject *parent)
    : QObject(parent), m_store(store)
{
    connect(&m_server, &QTcpServer::newConnection, this, &ApiServer::handleConnection);
    connect(&m_discoverySocket, &QUdpSocket::readyRead, this, &ApiServer::handleDiscoveryDatagram);
}

bool ApiServer::start(quint16 port, const QString &token, const QString &deviceName, QString *errorMessage)
{
    m_token = token;
    m_deviceName = deviceName;
    if (m_server.isListening())
        m_server.close();

    if (!m_server.listen(QHostAddress::AnyIPv4, port)) {
        if (errorMessage)
            *errorMessage = m_server.errorString();
        return false;
    }

    if (m_discoverySocket.state() != QAbstractSocket::BoundState)
        m_discoverySocket.bind(QHostAddress::AnyIPv4, DiscoveryPort, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);

    return true;
}

quint16 ApiServer::port() const
{
    return m_server.serverPort();
}

QStringList ApiServer::serverUrls() const
{
    return localServerUrls(m_server.serverPort());
}

void ApiServer::handleConnection()
{
    while (QTcpSocket *socket = m_server.nextPendingConnection()) {
        socket->setParent(this);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { handleReadyRead(socket); });
        connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
    }
}

void ApiServer::handleDiscoveryDatagram()
{
    while (m_discoverySocket.hasPendingDatagrams()) {
        const QNetworkDatagram datagram = m_discoverySocket.receiveDatagram();
        if (QString::fromUtf8(datagram.data()).trimmed() != QLatin1String(DiscoveryRequest))
            continue;

        QStringList urls = localServerUrls(m_server.serverPort());
        const QString matchedUrl = subnetMatchedServerUrl(datagram.senderAddress(), m_server.serverPort());
        if (!matchedUrl.isEmpty()) {
            urls.removeAll(matchedUrl);
            urls.prepend(matchedUrl);
        }

        m_discoverySocket.writeDatagram(QJsonDocument(discoveryResponse(urls)).toJson(QJsonDocument::Compact),
                                        datagram.senderAddress(),
                                        datagram.senderPort());
    }
}

void ApiServer::handleReadyRead(QTcpSocket *socket)
{
    const QByteArray raw = socket->readAll();
    HttpRequest request;
    QString error;
    if (!parseRequest(raw, &request, &error)) {
        sendError(socket, 400, error);
        return;
    }

    processRequest(socket, request);
}

void ApiServer::processRequest(QTcpSocket *socket, const HttpRequest &request)
{
    if (request.method == QStringLiteral("GET") && request.path == QStringLiteral("/api/discovery")) {
        sendJson(socket, 200, discoveryResponse(localServerUrls(m_server.serverPort())));
        return;
    }

    if (!isAuthorized(request)) {
        sendError(socket, 401, QStringLiteral("Missing or invalid bearer token."));
        return;
    }

    if (request.method == QStringLiteral("POST") && request.path == QStringLiteral("/api/clipboard")) {
        if (request.body.size() > 1024 * 1024) {
            sendError(socket, 413, QStringLiteral("Payload exceeds the 1 MB limit."));
            return;
        }

        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(request.body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            sendError(socket, 400, QStringLiteral("Request body must be a JSON object."));
            return;
        }

        ClipboardEntry entry = ClipboardEntry::fromJson(document.object());
        QString validationError;
        if (!entry.isValid(&validationError)) {
            sendError(socket, 400, validationError);
            return;
        }
        m_store->setLatest(entry);
        sendJson(socket, 201, entry.toJson());
        return;
    }

    if (request.method == QStringLiteral("GET") && request.path == QStringLiteral("/api/clipboard/latest")) {
        const auto latest = m_store->latest();
        if (!latest) {
            sendError(socket, 404, QStringLiteral("Network clipboard is empty."));
            return;
        }
        sendJson(socket, 200, latest->toJson());
        return;
    }

    if (request.method == QStringLiteral("GET") && request.path == QStringLiteral("/api/clipboard/history")) {
        QJsonArray items;
        for (const ClipboardEntry &entry : m_store->history())
            items.append(entry.toJson());
        sendJson(socket, 200, {{QStringLiteral("items"), items}});
        return;
    }

    if (request.method == QStringLiteral("DELETE") && request.path == QStringLiteral("/api/clipboard/history")) {
        m_store->clear();
        sendJson(socket, 200, {{QStringLiteral("ok"), true}});
        return;
    }

    sendError(socket, 404, QStringLiteral("Unknown endpoint."));
}

bool ApiServer::parseRequest(const QByteArray &raw, HttpRequest *request, QString *errorMessage) const
{
    const int headerEnd = raw.indexOf("\r\n\r\n");
    if (headerEnd < 0) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Incomplete HTTP request.");
        return false;
    }

    const QList<QByteArray> lines = raw.left(headerEnd).split('\n');
    if (lines.isEmpty())
        return false;

    const QList<QByteArray> requestLine = lines.first().trimmed().split(' ');
    if (requestLine.size() < 2) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Invalid HTTP request line.");
        return false;
    }

    request->method = QString::fromLatin1(requestLine.at(0)).toUpper();
    request->path = QString::fromLatin1(requestLine.at(1));
    const int queryStart = request->path.indexOf('?');
    if (queryStart >= 0)
        request->path = request->path.left(queryStart);

    for (int i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i).trimmed();
        const int separator = line.indexOf(':');
        if (separator > 0)
            request->headers.insert(QString::fromLatin1(line.left(separator)).toLower(), QString::fromLatin1(line.mid(separator + 1).trimmed()));
    }

    request->body = raw.mid(headerEnd + 4);
    return true;
}

bool ApiServer::isAuthorized(const HttpRequest &request) const
{
    return request.headers.value(QStringLiteral("authorization")) == QStringLiteral("Bearer %1").arg(m_token);
}

QJsonObject ApiServer::discoveryResponse(const QStringList &urls) const
{
    QJsonArray urlArray;
    for (const QString &url : urls)
        urlArray.append(url);

    return {
        {QStringLiteral("service"), QStringLiteral("NetworkClipboard")},
        {QStringLiteral("serverName"), m_deviceName},
        {QStringLiteral("url"), urls.value(0)},
        {QStringLiteral("urls"), urlArray},
        {QStringLiteral("token"), m_token},
        {QStringLiteral("agentActive"), true}
    };
}

void ApiServer::sendJson(QTcpSocket *socket, int statusCode, const QJsonObject &body) const
{
    sendBytes(socket, statusCode, "application/json; charset=utf-8", QJsonDocument(body).toJson(QJsonDocument::Compact));
}

void ApiServer::sendBytes(QTcpSocket *socket, int statusCode, const QByteArray &contentType, const QByteArray &body) const
{
    QByteArray response;
    response += "HTTP/1.1 " + QByteArray::number(statusCode) + " " + reasonPhrase(statusCode) + "\r\n";
    response += "Content-Type: " + contentType + "\r\n";
    response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    response += "Connection: close\r\n";
    response += "Access-Control-Allow-Origin: *\r\n\r\n";
    response += body;
    socket->write(response);
    socket->disconnectFromHost();
}

void ApiServer::sendError(QTcpSocket *socket, int statusCode, const QString &message) const
{
    sendJson(socket, statusCode, {{QStringLiteral("error"), message}});
}
