#pragma once

#include <QNetworkAccessManager>
#include <QObject>
#include <QString>

class NetworkClipboardClient : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString serverUrl READ serverUrl WRITE setServerUrl NOTIFY serverUrlChanged)
    Q_PROPERTY(QString token READ token WRITE setToken NOTIFY tokenChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)

public:
    explicit NetworkClipboardClient(QObject *parent = nullptr);

    QString serverUrl() const;
    void setServerUrl(const QString &serverUrl);

    QString token() const;
    void setToken(const QString &token);

    QString status() const;

    Q_INVOKABLE void sendText(const QString &text, const QString &deviceName);
    Q_INVOKABLE void getLatest();

signals:
    void serverUrlChanged();
    void tokenChanged();
    void statusChanged();
    void latestReceived(const QString &text);

private:
    QNetworkRequest request(const QString &path) const;
    void setStatus(const QString &status);

    QNetworkAccessManager m_network;
    QString m_serverUrl = QStringLiteral("http://192.168.178.1:8787");
    QString m_token;
    QString m_status = QStringLiteral("Ready");
};
