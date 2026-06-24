#include "TrayController.h"

#include <QAction>
#include <QApplication>
#include <QAbstractSocket>
#include <QBuffer>
#include <QByteArrayView>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDateTime>
#include <QHostAddress>
#include <QJsonDocument>
#include <QImage>
#include <QMenu>
#include <QMimeData>
#include <QNetworkInterface>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QStyle>
#include <QSettings>
#include <QUuid>

namespace {
constexpr qint64 ClipboardIgnoreWindowMs = 1500;

QByteArray imagePngData(const QImage &sourceImage)
{
    constexpr qsizetype MaxImageBytes = 10 * 1024 * 1024;
    if (sourceImage.isNull())
        return {};

    QImage image = sourceImage;
    for (int attempt = 0; attempt < 10; ++attempt) {
        QByteArray data;
        QBuffer buffer(&data);
        if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG"))
            return {};
        if (data.size() <= MaxImageBytes)
            return data;

        const int nextWidth = qMax(1, qRound(image.width() * 0.8));
        const int nextHeight = qMax(1, qRound(image.height() * 0.8));
        if (nextWidth == image.width() && nextHeight == image.height())
            break;
        image = image.scaled(nextWidth,
                             nextHeight,
                             Qt::KeepAspectRatio,
                             Qt::SmoothTransformation);
    }
    return {};
}

QByteArray imageHash(const QImage &image)
{
    if (image.isNull())
        return {};

    const QImage normalized = image.convertToFormat(QImage::Format_RGBA8888);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArray::number(normalized.width()));
    hash.addData("x");
    hash.addData(QByteArray::number(normalized.height()));
    for (int row = 0; row < normalized.height(); ++row) {
        hash.addData(QByteArrayView(
            reinterpret_cast<const char *>(normalized.constScanLine(row)),
            normalized.width() * 4));
    }
    return hash.result();
}

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

bool isLikelyImageUrl(const QUrl &url)
{
    if (!url.isValid())
        return false;

    const QString scheme = url.scheme().toLower();
    if (scheme != QStringLiteral("http") && scheme != QStringLiteral("https"))
        return false;

    const QString path = url.path().toLower();
    if (path.endsWith(QStringLiteral(".png"))
        || path.endsWith(QStringLiteral(".jpg"))
        || path.endsWith(QStringLiteral(".jpeg"))
        || path.endsWith(QStringLiteral(".webp"))
        || path.endsWith(QStringLiteral(".gif"))
        || path.endsWith(QStringLiteral(".bmp"))) {
        return true;
    }

    const QString host = url.host().toLower();
    if (host.endsWith(QStringLiteral("bing.com")) && path == QStringLiteral("/th"))
        return true;

    return false;
}

QString htmlDecoded(QString text)
{
    text.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
    text.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
    text.replace(QStringLiteral("&#39;"), QStringLiteral("'"));
    text.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
    text.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
    return text;
}

QString decodedClipboardData(const QByteArray &data)
{
    if (data.isEmpty())
        return {};

    QString utf16;
    if (data.size() >= 2 && data.size() % 2 == 0) {
        utf16 = QString::fromUtf16(reinterpret_cast<const char16_t *>(data.constData()), data.size() / 2);
        utf16.remove(QLatin1Char('\0'));
    }

    QString utf8 = QString::fromUtf8(data);
    utf8.remove(QLatin1Char('\0'));

    return utf16.count(QStringLiteral("http"), Qt::CaseInsensitive) > utf8.count(QStringLiteral("http"), Qt::CaseInsensitive)
        ? utf16
        : utf8;
}

QUrl firstHttpUrlFromText(const QString &text)
{
    const QString trimmed = htmlDecoded(text.trimmed());
    if (!trimmed.contains(QLatin1Char('\n')) && !trimmed.contains(QLatin1Char('\r'))) {
        const QUrl directUrl(trimmed);
        const QString scheme = directUrl.scheme().toLower();
        if (scheme == QStringLiteral("http") || scheme == QStringLiteral("https"))
            return directUrl;
    }

    static const QRegularExpression urlPattern(QStringLiteral(R"(https?://[^\s"'<>]+)"));
    QRegularExpressionMatchIterator iterator = urlPattern.globalMatch(trimmed);
    while (iterator.hasNext()) {
        QString rawUrl = htmlDecoded(iterator.next().captured(0));
        while (!rawUrl.isEmpty()
               && QStringLiteral(".,;:!?)]}").contains(rawUrl.back())) {
            rawUrl.chop(1);
        }

        const QUrl url(rawUrl);
        const QString scheme = url.scheme().toLower();
        if (scheme == QStringLiteral("http") || scheme == QStringLiteral("https"))
            return url;
    }

    return {};
}

QUrl imageUrlFromText(const QString &text)
{
    const QUrl url = firstHttpUrlFromText(text);
    return isLikelyImageUrl(url) ? url : QUrl{};
}

QUrl clipboardImageUrlCandidate(const QMimeData *mimeData, const QString &plainText)
{
    QUrl url = imageUrlFromText(plainText);
    if (url.isValid() || !mimeData)
        return url;

    if (mimeData->hasUrls()) {
        const QList<QUrl> urls = mimeData->urls();
        for (const QUrl &candidate : urls) {
            if (isLikelyImageUrl(candidate))
                return candidate;
        }
    }

    if (mimeData->hasHtml()) {
        url = imageUrlFromText(mimeData->html());
        if (url.isValid())
            return url;
    }

    const QStringList formats = mimeData->formats();
    for (const QString &format : formats) {
        const QString lowerFormat = format.toLower();
        if (!lowerFormat.contains(QStringLiteral("url"))
            && !lowerFormat.contains(QStringLiteral("html"))
            && !lowerFormat.contains(QStringLiteral("chromium"))
            && !lowerFormat.contains(QStringLiteral("text"))) {
            continue;
        }

        url = imageUrlFromText(decodedClipboardData(mimeData->data(format)));
        if (url.isValid())
            return url;
    }

    return {};
}
}

TrayController::TrayController(const QString &deviceId, const QString &deviceName, QObject *parent)
    : QObject(parent), m_clipboard(QApplication::clipboard()), m_deviceId(deviceId), m_deviceName(deviceName)
{
    QSettings settings;
    m_isMaster = settings.value(QStringLiteral("server/isMaster"), true).toBool();

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

    m_masterAction = m_menu->addAction(QStringLiteral("Is Master"));
    m_masterAction->setCheckable(true);
    m_masterAction->setChecked(m_isMaster);
    connect(m_masterAction, &QAction::toggled, this, &TrayController::setMasterServer);

    m_menu->addSeparator();
    m_serviceAction = m_menu->addAction(QStringLiteral("Start Server Service"));
    connect(m_serviceAction, &QAction::triggered, this, &TrayController::toggleServerService);

    QAction *quitAction = m_menu->addAction(QStringLiteral("Quit Tray Agent"));
    connect(quitAction, &QAction::triggered, qApp, &QApplication::quit);

    m_tray.setContextMenu(m_menu);
    m_tray.setToolTip(QStringLiteral("Network Clipboard"));
    m_tray.setIcon(QApplication::style()->standardIcon(QStyle::SP_DriveNetIcon));

    connect(m_clipboard, &QClipboard::dataChanged, this, &TrayController::onClipboardChanged);
    m_clipboardChangeTimer.setSingleShot(true);
    m_clipboardChangeTimer.setInterval(250);
    connect(&m_clipboardChangeTimer, &QTimer::timeout, this, &TrayController::processClipboardChange);
    connect(&m_pollTimer, &QTimer::timeout, this, &TrayController::pollLatestFromServer);
    connect(&m_pollTimer, &QTimer::timeout, this, &TrayController::sendAgentHeartbeat);
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
    scheduleCurrentClipboardPublish(false);
}

void TrayController::onClipboardChanged()
{
    const quint64 generation = ++m_clipboardChangeGeneration;
    m_clipboardRetriesRemaining = 12;
    m_clipboardChangeTimer.start();
    QTimer::singleShot(900, this, [this, generation]() {
        if (generation == m_clipboardChangeGeneration)
            processClipboardChange();
    });
}

void TrayController::processClipboardChange()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QMimeData *mimeData = m_clipboard->mimeData();

    if (mimeData && mimeData->hasImage()) {
        const QImage image = m_clipboard->image();
        const QByteArray pngData = imagePngData(image);
        if (!pngData.isEmpty()) {
            const QByteArray hash = imageHash(QImage::fromData(pngData, "PNG"));
            if (now < m_ignoreClipboardChangesUntil && hash == m_ignoredClipboardImageHash)
                return;
            if (!m_autoSendEnabled)
                return;
            if (hash == m_lastPublishedImageHash) {
                const QString text = m_clipboard->text().trimmed();
                tryPublishImageUrl(mimeData, text, false, false, false);
                return;
            }

            publishClipboardImage(image, false);
            return;
        }

        // Browsers may advertise an image before delayed clipboard rendering
        // has produced usable pixels. Retry before falling back to its URL/text.
        if (m_clipboardRetriesRemaining > 0) {
            --m_clipboardRetriesRemaining;
            m_clipboardChangeTimer.start(350);
            return;
        }
    }

    const QString text = m_clipboard->text().trimmed();
    if (now < m_ignoreClipboardChangesUntil && text == m_ignoredClipboardContent)
        return;

    if (!m_autoSendEnabled)
        return;

    if (text.isEmpty())
        return;

    if (tryPublishImageUrl(mimeData, text, false, false, true))
        return;

    if (text == m_lastPublishedContent)
        return;

    publishClipboardText(text, false);
}

void TrayController::pollLatestFromServer()
{
    if (m_token.isEmpty() || !m_serverUrl.isValid())
        return;

    QNetworkRequest request = apiRequest(QStringLiteral("/api/clipboard/latest"));
    if (!m_lastSeenNetworkEntryId.isEmpty())
        request.setRawHeader("If-None-Match", m_lastSeenNetworkEntryId.toUtf8());
    QNetworkReply *reply = m_network.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 304) {
            reply->deleteLater();
            return;
        }
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

void TrayController::sendAgentHeartbeat()
{
    if (m_token.isEmpty() || !m_serverUrl.isValid())
        return;

    const QJsonObject heartbeat{{QStringLiteral("isMaster"), m_isMaster}};
    QNetworkReply *reply = m_network.post(
        apiRequest(QStringLiteral("/api/agent/heartbeat")),
        QJsonDocument(heartbeat).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, reply, &QNetworkReply::deleteLater);
}

void TrayController::sendCurrentClipboard()
{
    const QMimeData *mimeData = m_clipboard->mimeData();
    if (mimeData && mimeData->hasImage()) {
        const QImage image = m_clipboard->image();
        if (!imagePngData(image).isEmpty()) {
            publishClipboardImage(image, true, true);
            return;
        }
    }

    const QString text = m_clipboard->text().trimmed();
    if (tryPublishImageUrl(mimeData, text, true, true, true))
        return;

    publishClipboardText(text, true, true);
}

void TrayController::publishClipboardText(const QString &text, bool showSuccessMessage, bool force)
{
    if (text.isEmpty()) {
        if (showSuccessMessage)
            m_tray.showMessage(QStringLiteral("Network Clipboard"), QStringLiteral("Windows clipboard is empty."));
        return;
    }

    if (!force && text == m_lastPublishedContent)
        return;

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

    sendEntryToServer(entry, showSuccessMessage);
}

void TrayController::publishClipboardImage(const QImage &image, bool showSuccessMessage, bool force)
{
    const QByteArray pngData = imagePngData(image);
    if (pngData.isEmpty()) {
        if (showSuccessMessage)
            m_tray.showMessage(QStringLiteral("Network Clipboard"), QStringLiteral("Windows clipboard contains no usable image."));
        return;
    }

    const QByteArray hash = imageHash(QImage::fromData(pngData, "PNG"));
    if (!force && hash == m_lastPublishedImageHash)
        return;

    ClipboardEntry entry;
    entry.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    entry.deviceId = m_deviceId;
    entry.deviceName = m_deviceName;
    entry.type = QStringLiteral("image");
    entry.mimeType = QStringLiteral("image/png");
    entry.content = QString::fromLatin1(pngData.toBase64());
    entry.timestamp = QDateTime::currentSecsSinceEpoch();

    QString error;
    if (!entry.isValid(&error)) {
        if (showSuccessMessage)
            m_tray.showMessage(QStringLiteral("Network Clipboard"), error);
        return;
    }

    sendEntryToServer(entry, showSuccessMessage);
}

bool TrayController::tryPublishImageUrl(const QMimeData *mimeData,
                                        const QString &text,
                                        bool showSuccessMessage,
                                        bool force,
                                        bool publishTextOnFailure)
{
    const QUrl imageUrl = clipboardImageUrlCandidate(mimeData, text);
    if (imageUrl.isValid()) {
        publishDownloadedImageUrl(imageUrl, text, showSuccessMessage, force, publishTextOnFailure);
        return true;
    }

    return false;
}

void TrayController::publishDownloadedImageUrl(const QUrl &imageUrl,
                                               const QString &fallbackText,
                                               bool showSuccessMessage,
                                               bool force,
                                               bool publishTextOnFailure)
{
    const QString imageKey = QStringLiteral("image:") + imageUrl.toString(QUrl::FullyEncoded);
    if (m_pendingImageUrlDownload == imageKey)
        return;

    m_pendingImageUrlDownload = imageKey;

    QNetworkRequest request(imageUrl);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setRawHeader("Accept", "image/avif,image/webp,image/apng,image/svg+xml,image/*,*/*;q=0.8");
    request.setRawHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) NetworkClipboard/1.0");

    QNetworkReply *reply = m_network.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, imageKey, fallbackText, showSuccessMessage, force, publishTextOnFailure]() {
        if (m_pendingImageUrlDownload == imageKey)
            m_pendingImageUrlDownload.clear();

        QImage image;
        if (reply->error() == QNetworkReply::NoError)
            image.loadFromData(reply->readAll());

        reply->deleteLater();

        if (!image.isNull()) {
            const QByteArray pngData = imagePngData(image);
            const QByteArray hash = imageHash(QImage::fromData(pngData, "PNG"));
            if (!hash.isEmpty()) {
                m_ignoreClipboardChangesUntil = QDateTime::currentMSecsSinceEpoch() + ClipboardIgnoreWindowMs;
                m_ignoredClipboardContent.clear();
                m_ignoredClipboardImageHash = hash;
                m_clipboard->setImage(image);
            }
            publishClipboardImage(image, showSuccessMessage, force);
            return;
        }

        if (showSuccessMessage) {
            m_tray.showMessage(QStringLiteral("Network Clipboard"),
                               QStringLiteral("Image URL could not be loaded; sending URL instead."));
        }

        if (publishTextOnFailure)
            publishClipboardText(fallbackText, showSuccessMessage, force);
    });
}

void TrayController::publishCurrentClipboardIfAvailable(bool force)
{
    if (!m_autoSendEnabled || !m_serviceRunning || m_token.isEmpty() || !m_serverUrl.isValid())
        return;

    const QMimeData *mimeData = m_clipboard->mimeData();
    if (mimeData && mimeData->hasImage()) {
        const QImage image = m_clipboard->image();
        if (!imagePngData(image).isEmpty()) {
            publishClipboardImage(image, false, force);
            return;
        }
    }

    const QString text = m_clipboard->text().trimmed();
    if (tryPublishImageUrl(mimeData, text, false, force, true))
        return;

    publishClipboardText(text, false, force);
}

void TrayController::scheduleCurrentClipboardPublish(bool force)
{
    QTimer::singleShot(500, this, [this, force]() {
        publishCurrentClipboardIfAvailable(force);
    });
    QTimer::singleShot(2000, this, [this, force]() {
        publishCurrentClipboardIfAvailable(force);
    });
    QTimer::singleShot(5000, this, [this, force]() {
        publishCurrentClipboardIfAvailable(force);
    });
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
    const bool serviceStarted = !m_serviceRunning && running;
    if (m_serviceRunning == running && m_serviceAction)
        return;

    m_serviceRunning = running;
    if (!m_serviceAction)
        return;

    m_serviceAction->setText(m_serviceRunning
                                 ? QStringLiteral("Stop Server Service")
                                 : QStringLiteral("Start Server Service"));

    if (serviceStarted)
        scheduleCurrentClipboardPublish(true);
}

void TrayController::setAutoSendEnabled(bool enabled)
{
    m_autoSendEnabled = enabled;
}

void TrayController::setMasterServer(bool isMaster)
{
    m_isMaster = isMaster;
    QSettings settings;
    settings.setValue(QStringLiteral("server/isMaster"), m_isMaster);
    sendAgentHeartbeat();
}

void TrayController::sendEntryToServer(const ClipboardEntry &entry, bool showSuccessMessage)
{
    if (m_sendInFlight) {
        m_pendingEntry = entry;
        m_pendingShowSuccessMessage = showSuccessMessage;
        return;
    }

    m_sendInFlight = true;
    QNetworkReply *reply = m_network.post(apiRequest(QStringLiteral("/api/clipboard")), QJsonDocument(entry.toJson()).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, entry, showSuccessMessage]() {
        if (reply->error() != QNetworkReply::NoError) {
            m_tray.showMessage(QStringLiteral("Network Clipboard"), QStringLiteral("Could not send to server: %1").arg(reply->errorString()));
        } else {
            m_lastSeenNetworkEntryId = entry.id;
            if (entry.type == QStringLiteral("image")) {
                m_lastPublishedImageHash = imageHash(
                    QImage::fromData(QByteArray::fromBase64(entry.content.toLatin1()), "PNG"));
                m_lastPublishedContent.clear();
            } else {
                m_lastPublishedContent = entry.content;
                m_lastPublishedImageHash.clear();
            }
            if (showSuccessMessage)
                m_tray.showMessage(QStringLiteral("Network Clipboard"), QStringLiteral("Sent Windows clipboard to server."));
        }
        reply->deleteLater();
        m_sendInFlight = false;

        if (m_pendingEntry) {
            const ClipboardEntry pendingEntry = *m_pendingEntry;
            const bool pendingShowSuccessMessage = m_pendingShowSuccessMessage;
            m_pendingEntry.reset();
            m_pendingShowSuccessMessage = false;
            QTimer::singleShot(0, this, [this, pendingEntry, pendingShowSuccessMessage]() {
                sendEntryToServer(pendingEntry, pendingShowSuccessMessage);
            });
        }
    });
}

void TrayController::applyNetworkEntryToClipboard(const ClipboardEntry &entry, bool showMessage, bool allowOwnEntry)
{
    if (entry.content.isEmpty()) {
        if (showMessage)
            m_tray.showMessage(QStringLiteral("Network Clipboard"), QStringLiteral("No network clipboard entry available."));
        return;
    }

    if (!allowOwnEntry && entry.deviceId == m_deviceId) {
        m_lastSeenNetworkEntryId = entry.id;
        return;
    }

    if (!allowOwnEntry && !entry.id.isEmpty() && entry.id == m_lastSeenNetworkEntryId)
        return;

    if (!allowOwnEntry && entry.id.isEmpty() && entry.content == m_lastSeenNetworkContent)
        return;

    if (entry.type == QStringLiteral("image")) {
        const QByteArray pngData = QByteArray::fromBase64(entry.content.toLatin1());
        const QImage image = QImage::fromData(pngData, "PNG");
        if (image.isNull()) {
            if (showMessage)
                m_tray.showMessage(QStringLiteral("Network Clipboard"), QStringLiteral("Network clipboard image is invalid."));
            return;
        }
        const QByteArray hash = imageHash(image);
        if (!allowOwnEntry && entry.id.isEmpty() && hash == m_lastSeenNetworkImageHash)
            return;

        m_lastSeenNetworkEntryId = entry.id;
        m_lastSeenNetworkContent.clear();
        m_lastSeenNetworkImageHash = hash;
        m_ignoreClipboardChangesUntil = QDateTime::currentMSecsSinceEpoch() + ClipboardIgnoreWindowMs;
        m_ignoredClipboardContent.clear();
        m_ignoredClipboardImageHash = hash;
        m_lastPublishedContent.clear();
        m_lastPublishedImageHash = hash;
        m_clipboard->setImage(image);

        if (showMessage)
            m_tray.showMessage(QStringLiteral("Network Clipboard"), QStringLiteral("Copied latest network image to Windows clipboard."));
        return;
    }

    const QString content = withWindowsLineEndings(entry.content);
    m_lastSeenNetworkEntryId = entry.id;
    m_lastSeenNetworkContent = entry.content;
    m_lastSeenNetworkImageHash.clear();
    m_ignoreClipboardChangesUntil = QDateTime::currentMSecsSinceEpoch() + ClipboardIgnoreWindowMs;
    m_ignoredClipboardContent = content;
    m_ignoredClipboardImageHash.clear();
    m_lastPublishedContent = content;
    m_lastPublishedImageHash.clear();
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
