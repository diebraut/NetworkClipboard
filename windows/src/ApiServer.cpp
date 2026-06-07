#include "ApiServer.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpSocket>

namespace {
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
}

ApiServer::ApiServer(ClipboardStore *store, QObject *parent)
    : QObject(parent), m_store(store)
{
    connect(&m_server, &QTcpServer::newConnection, this, &ApiServer::handleConnection);
}

bool ApiServer::start(quint16 port, const QString &token, QString *errorMessage)
{
    m_token = token;
    if (m_server.isListening())
        m_server.close();

    if (!m_server.listen(QHostAddress::AnyIPv4, port)) {
        if (errorMessage)
            *errorMessage = m_server.errorString();
        return false;
    }

    return true;
}

quint16 ApiServer::port() const
{
    return m_server.serverPort();
}

void ApiServer::handleConnection()
{
    while (QTcpSocket *socket = m_server.nextPendingConnection()) {
        socket->setParent(this);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { handleReadyRead(socket); });
        connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
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
        if (separator <= 0)
            continue;
        request->headers.insert(QString::fromLatin1(line.left(separator)).toLower(), QString::fromLatin1(line.mid(separator + 1).trimmed()));
    }

    request->body = raw.mid(headerEnd + 4);
    return true;
}

bool ApiServer::isAuthorized(const HttpRequest &request) const
{
    return request.headers.value(QStringLiteral("authorization")) == QStringLiteral("Bearer %1").arg(m_token);
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
