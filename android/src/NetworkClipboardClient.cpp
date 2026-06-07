#include "NetworkClipboardClient.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUuid>

NetworkClipboardClient::NetworkClipboardClient(QObject *parent)
    : QObject(parent)
{
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

    QNetworkReply *reply = m_network.post(request(QStringLiteral("/api/clipboard")), QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        setStatus(reply->error() == QNetworkReply::NoError ? QStringLiteral("Sent.") : reply->errorString());
        reply->deleteLater();
    });
}

void NetworkClipboardClient::getLatest()
{
    QNetworkReply *reply = m_network.get(request(QStringLiteral("/api/clipboard/latest")));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        if (reply->error() != QNetworkReply::NoError) {
            setStatus(reply->errorString());
            reply->deleteLater();
            return;
        }

        const QJsonObject object = QJsonDocument::fromJson(reply->readAll()).object();
        emit latestReceived(object.value(QStringLiteral("content")).toString());
        setStatus(QStringLiteral("Received."));
        reply->deleteLater();
    });
}

QNetworkRequest NetworkClipboardClient::request(const QString &path) const
{
    QNetworkRequest request(QUrl(m_serverUrl + path));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Authorization", QByteArray("Bearer ") + m_token.toUtf8());
    return request;
}

void NetworkClipboardClient::setStatus(const QString &status)
{
    if (m_status == status)
        return;
    m_status = status;
    emit statusChanged();
}
