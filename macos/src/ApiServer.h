#pragma once

#include "ClipboardStore.h"

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <QTcpServer>
#include <QUdpSocket>

class QTcpSocket;

class ApiServer : public QObject
{
    Q_OBJECT

public:
    explicit ApiServer(ClipboardStore *store, QObject *parent = nullptr);

    bool start(quint16 port, const QString &token, const QString &serverName, QString *errorMessage = nullptr);
    void setMasterServer(bool masterServer);
    quint16 port() const;
    QStringList serverUrls() const;

private:
    struct HttpRequest
    {
        QString method;
        QString path;
        QHash<QString, QString> headers;
        QByteArray body;
    };

    void handleConnection();
    void handleDiscoveryDatagram();
    void handleReadyRead(QTcpSocket *socket);
    void processRequest(QTcpSocket *socket, const HttpRequest &request);
    bool parseRequest(const QByteArray &raw, HttpRequest *request, QString *errorMessage) const;
    bool isAuthorized(const HttpRequest &request) const;
    void sendJson(QTcpSocket *socket, int statusCode, const QJsonObject &body) const;
    void sendBytes(QTcpSocket *socket, int statusCode, const QByteArray &contentType, const QByteArray &body) const;
    void sendError(QTcpSocket *socket, int statusCode, const QString &message) const;
    QJsonObject discoveryResponse(const QStringList &urls) const;

    ClipboardStore *m_store = nullptr;
    QTcpServer m_server;
    QUdpSocket m_discoverySocket;
    QHash<QTcpSocket *, QByteArray> m_requestBuffers;
    QString m_token;
    QString m_serverName;
    bool m_isMaster = true;
};
