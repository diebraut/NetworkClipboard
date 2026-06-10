#include "TrayController.h"

#include <QAction>
#include <QApplication>
#include <QAbstractSocket>
#include <QClipboard>
#include <QDateTime>
#include <QHostAddress>
#include <QJsonDocument>
#include <QMenu>
#include <QNetworkInterface>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QStyle>
#include <QUuid>

namespace {
constexpr qint64 ClipboardIgnoreWindowMs = 1500;

QString withWindowsLineEndings(QString text)
{
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    text.replace(QLatin1Char('\n'), QStringLiteral("\r\n"));
    return text;
}

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
            if (ip.startsWith(QStringLiteral("169.254.")))
                continue;

            urls.append(QStringLiteral("http://%1:%2").arg(ip).arg(port));
        }
    }

    urls.removeDuplicates();
    return urls;
}

bool runElevatedServiceCommand(const QString &command)
{
#ifdef Q_OS_WIN
    const QStringList arguments{
        QStringLiteral("-NoProfile"),
        QStringLiteral("-WindowStyle"),
        QStringLiteral("Hidden"),
        QStringLiteral("-Command"),
        QStringLiteral("Start-Process -FilePath sc.exe -ArgumentList '%1 NetworkClipboardServer' -Verb RunAs -WindowStyle Hidden").arg(command)
    };

    return QProcess::startDetached(QStringLiteral("powershell.exe"), arguments);
#else
    Q_UNUSED(command)
    return false;
#endif
}

bool isServiceRunning()
{
#ifdef Q_OS_WIN
    QProcess process;
    process.start(QStringLiteral("sc.exe"), {QStringLiteral("query"), QStringLiteral("NetworkClipboardServer")});
    if (!process.waitForFinished(1500))
        return false;

    const QString output = QString::fromLocal8Bit(process.readAllStandardOutput());
    return output.contains(QStringLiteral("RUNNING"), Qt::CaseInsensitive);
#else
    return false;
#endif
}
}

TrayController::TrayController(const QString &deviceId, const QString &deviceName, QObject *parent)
    : QObject(parent), m_clipboard(QApplication::clipboard()), m_deviceId(deviceId), m_deviceName(deviceName)
{
    m_menu = new QMenu();

    QAction *pasteAction = m_menu->addAction(QStringLiteral("Paste from Network"));
    connect(pasteAction, &QAction::triggered, this, &TrayController::pasteFromNetwork);

    QAction *sendAction = m_menu->addAction(QStringLiteral("Send Clipboard Now"));
    connect(sendAction, &QAction::triggered, this, &TrayController::sendCurrentClipboard);

    QAction *copyInfoAction = m_menu->addAction(QStringLiteral("Copy Server Info"));
    connect(copyInfoAction, &QAction::triggered, this, &TrayController::copyServerInfo);

    m_menu->addSeparator();
    m_autoSendAction = m_menu->addAction(QStringLiteral("Auto-send Windows Clipboard"));
    m_autoSendAction->setCheckable(true);
    m_autoSendAction->setChecked(m_autoSendEnabled);
    connect(m_autoSendAction, &QAction::toggled, this, &TrayController::setAutoSendEnabled);

    m_menu->addSeparator();
    m_serviceAction = m_menu->addAction(QStringLiteral("Start Server Service"));
    connect(m_serviceAction, &QAction::triggered, this, &TrayController::toggleServerService);

    QAction *quitAction = m_menu->addAction(QStringLiteral("Quit Tray Agent"));
    connect(quitAction, &QAction::triggered, qApp, &QApplication::quit);

    m_tray.setContextMenu(m_menu);
    m_tray.setToolTip(QStringLiteral("Network Clipboard"));
    m_tray.setIcon(QApplication::style()->standardIcon(QStyle::SP_DriveNetIcon));

    connect(m_clipboard, &QClipboard::dataChanged, this, &TrayController::onClipboardChanged);
    connect(&m_pollTimer, &QTimer::timeout, this, &TrayController::pollLatestFromServer);
    connect(&m_pollTimer, &QTimer::timeout, this, &TrayController::updateServiceStatus);
    m_pollTimer.setInterval(2000);
    m_pollTimer.start();
    updateServiceStatus();
}

void TrayController::show()
{
    m_tray.show();
}

void TrayController::setServerInfo(const QUrl &serverUrl, quint16 port, const QString &token)
{
    m_serverUrl = serverUrl;
    m_port = port;
    m_token = token;
}

void TrayController::onClipboardChanged()
{
    const QString text = m_clipboard->text().trimmed();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now < m_ignoreClipboardChangesUntil && text == m_ignoredClipboardContent)
        return;

    if (!m_autoSendEnabled)
        return;

    if (text.isEmpty() || text == m_lastPublishedContent)
        return;

    publishClipboardText(text, false);
}

void TrayController::pollLatestFromServer()
{
    if (m_token.isEmpty() || !m_serverUrl.isValid())
        return;

    QNetworkReply *reply = m_network.get(apiRequest(QStringLiteral("/api/clipboard/latest")));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        if (reply->error() != QNetworkReply::NoError) {
            reply->deleteLater();
            return;
        }

        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll(), &parseError);
        if (parseError.error == QJsonParseError::NoError && document.isObject()) {
            applyNetworkEntryToClipboard(ClipboardEntry::fromJson(document.object()), false, false);
        }

        reply->deleteLater();
    });
}

void TrayController::sendCurrentClipboard()
{
    publishClipboardText(m_clipboard->text().trimmed(), true);
}

void TrayController::publishClipboardText(const QString &text, bool showSuccessMessage)
{
    if (text.isEmpty()) {
        if (showSuccessMessage)
            m_tray.showMessage(QStringLiteral("Network Clipboard"), QStringLiteral("Windows clipboard is empty."));
        return;
    }

    ClipboardEntry entry;
    entry.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    entry.deviceId = m_deviceId;
    entry.deviceName = m_deviceName;
    entry.type = text.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive) || text.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive)
        ? QStringLiteral("url")
        : QStringLiteral("text");
    entry.content = text;
    entry.timestamp = QDateTime::currentSecsSinceEpoch();

    QString error;
    if (!entry.isValid(&error)) {
        if (showSuccessMessage)
            m_tray.showMessage(QStringLiteral("Network Clipboard"), error);
        return;
    }

    m_lastPublishedContent = text;
    sendEntryToServer(entry, showSuccessMessage);
}

void TrayController::pasteFromNetwork()
{
    QNetworkReply *reply = m_network.get(apiRequest(QStringLiteral("/api/clipboard/latest")));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        if (reply->error() != QNetworkReply::NoError) {
            m_tray.showMessage(QStringLiteral("Network Clipboard"), QStringLiteral("Could not fetch from server: %1").arg(reply->errorString()));
            reply->deleteLater();
            return;
        }

        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            m_tray.showMessage(QStringLiteral("Network Clipboard"), QStringLiteral("Server returned invalid JSON."));
            reply->deleteLater();
            return;
        }

        const ClipboardEntry entry = ClipboardEntry::fromJson(document.object());
        applyNetworkEntryToClipboard(entry, true, true);
        reply->deleteLater();
    });
}

void TrayController::copyServerInfo()
{
    const QStringList urls = localServerUrls(m_port);
    const QString urlText = urls.isEmpty()
        ? QStringLiteral("http://127.0.0.1:%1").arg(m_port)
        : urls.join(QLatin1Char('\n'));
    const QString info = QStringLiteral("URL:\n%1\nAuthorization: Bearer %2").arg(urlText, m_token);
    m_ignoreClipboardChangesUntil = QDateTime::currentMSecsSinceEpoch() + ClipboardIgnoreWindowMs;
    m_ignoredClipboardContent = info.trimmed();
    m_clipboard->setText(info);
    m_tray.showMessage(QStringLiteral("Network Clipboard"), QStringLiteral("Server info copied to clipboard."));
}

void TrayController::toggleServerService()
{
    const QString command = m_serviceRunning ? QStringLiteral("stop") : QStringLiteral("start");
    if (!runElevatedServiceCommand(command)) {
        m_tray.showMessage(QStringLiteral("Network Clipboard"), QStringLiteral("Could not request service state change."));
        return;
    }
    m_tray.showMessage(QStringLiteral("Network Clipboard"),
                       m_serviceRunning
                           ? QStringLiteral("Requested Windows service stop.")
                           : QStringLiteral("Requested Windows service start."));
    QTimer::singleShot(2500, this, &TrayController::updateServiceStatus);
}

void TrayController::updateServiceStatus()
{
    const bool running = isServiceRunning();
    if (m_serviceRunning == running && m_serviceAction)
        return;

    m_serviceRunning = running;
    if (!m_serviceAction)
        return;

    m_serviceAction->setText(m_serviceRunning
                                 ? QStringLiteral("Stop Server Service")
                                 : QStringLiteral("Start Server Service"));
}

void TrayController::setAutoSendEnabled(bool enabled)
{
    m_autoSendEnabled = enabled;
}

void TrayController::sendEntryToServer(const ClipboardEntry &entry, bool showSuccessMessage)
{
    QNetworkReply *reply = m_network.post(apiRequest(QStringLiteral("/api/clipboard")), QJsonDocument(entry.toJson()).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, showSuccessMessage]() {
        if (reply->error() != QNetworkReply::NoError) {
            m_tray.showMessage(QStringLiteral("Network Clipboard"), QStringLiteral("Could not send to server: %1").arg(reply->errorString()));
        } else if (showSuccessMessage) {
            m_tray.showMessage(QStringLiteral("Network Clipboard"), QStringLiteral("Sent Windows clipboard to server."));
        }
        reply->deleteLater();
    });
}

void TrayController::applyNetworkEntryToClipboard(const ClipboardEntry &entry, bool showMessage, bool allowOwnEntry)
{
    if (entry.content.isEmpty()) {
        if (showMessage)
            m_tray.showMessage(QStringLiteral("Network Clipboard"), QStringLiteral("No network clipboard entry available."));
        return;
    }

    if (!allowOwnEntry && entry.deviceId == m_deviceId)
        return;

    if (!allowOwnEntry && !entry.id.isEmpty() && entry.id == m_lastSeenNetworkEntryId)
        return;

    if (!allowOwnEntry && entry.id.isEmpty() && entry.content == m_lastSeenNetworkContent)
        return;

    const QString content = withWindowsLineEndings(entry.content);
    m_lastSeenNetworkEntryId = entry.id;
    m_lastSeenNetworkContent = entry.content;
    m_ignoreClipboardChangesUntil = QDateTime::currentMSecsSinceEpoch() + ClipboardIgnoreWindowMs;
    m_ignoredClipboardContent = content;
    m_lastPublishedContent = content;
    m_clipboard->setText(content);

    if (showMessage)
        m_tray.showMessage(QStringLiteral("Network Clipboard"), QStringLiteral("Copied latest network entry to Windows clipboard."));
}

QNetworkRequest TrayController::apiRequest(const QString &path) const
{
    QUrl url = m_serverUrl;
    url.setPath(path);

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Authorization", QByteArray("Bearer ") + m_token.toUtf8());
    return request;
}
