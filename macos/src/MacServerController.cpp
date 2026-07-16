#include "MacServerController.h"

#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QByteArrayView>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostInfo>
#include <QIcon>
#include <QImage>
#include <QPixmap>
#include <QMenu>
#include <QMimeData>
#include <QProcess>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QStyle>
#include <QUrl>
#include <QUuid>
#include <QVariant>

#include <optional>

namespace {
constexpr quint16 ApiPort = 8787;
constexpr qint64 ClipboardIgnoreWindowMs = 1500;
constexpr qint64 FreshRemoteEntryWindowSecs = 15;
constexpr qsizetype MaxImageBytes = 10 * 1024 * 1024;
constexpr int MaxNetworkImageEdge = 900;
constexpr auto LaunchAgentLabel = "de.localtools.NetworkClipboardMacServer";

bool isUrlText(const QString &text)
{
    return text.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive)
        || text.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive);
}

QByteArray imagePngData(const QImage &sourceImage)
{
    if (sourceImage.isNull())
        return {};

    QImage image = sourceImage.convertToFormat(QImage::Format_RGBA8888);
    if (qMax(image.width(), image.height()) > MaxNetworkImageEdge) {
        image = image.scaled(MaxNetworkImageEdge,
                             MaxNetworkImageEdge,
                             Qt::KeepAspectRatio,
                             Qt::FastTransformation);
    }

    for (int attempt = 0; attempt < 8; ++attempt) {
        QByteArray data;
        QBuffer buffer(&data);
        if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG"))
            return {};

        if (data.size() <= MaxImageBytes)
            return data;

        const int nextWidth = qMax(1, qRound(image.width() * 0.75));
        const int nextHeight = qMax(1, qRound(image.height() * 0.75));
        if (nextWidth == image.width() && nextHeight == image.height())
            break;
        image = image.scaled(nextWidth, nextHeight, Qt::KeepAspectRatio, Qt::SmoothTransformation);
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

QString normalizedTextPayload(QString text)
{
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    return text.trimmed();
}

bool sameClipboardPayload(const ClipboardEntry &left, const ClipboardEntry &right)
{
    if (left.type == QStringLiteral("image") || right.type == QStringLiteral("image")) {
        if (left.type != QStringLiteral("image") || right.type != QStringLiteral("image"))
            return false;

        const QImage leftImage = QImage::fromData(QByteArray::fromBase64(left.content.toLatin1()), "PNG");
        const QImage rightImage = QImage::fromData(QByteArray::fromBase64(right.content.toLatin1()), "PNG");
        if (leftImage.isNull() || rightImage.isNull())
            return left.content == right.content;

        return imageHash(leftImage) == imageHash(rightImage);
    }

    return normalizedTextPayload(left.content) == normalizedTextPayload(right.content);
}

QImage imageFromClipboard(QClipboard *clipboard, const QMimeData *mimeData)
{
    if (!clipboard || !mimeData)
        return {};

    if (mimeData->hasImage()) {
        const QImage image = clipboard->image();
        if (!image.isNull())
            return image;

        const QVariant imageData = mimeData->imageData();
        if (imageData.canConvert<QImage>()) {
            const QImage variantImage = imageData.value<QImage>();
            if (!variantImage.isNull())
                return variantImage;
        }
        if (imageData.canConvert<QPixmap>()) {
            const QImage pixmapImage = imageData.value<QPixmap>().toImage();
            if (!pixmapImage.isNull())
                return pixmapImage;
        }
    }

    static const QStringList imageFormats{
        QStringLiteral("image/png"),
        QStringLiteral("image/jpeg"),
        QStringLiteral("image/jpg"),
        QStringLiteral("image/tiff"),
        QStringLiteral("image/bmp"),
        QStringLiteral("public.png"),
        QStringLiteral("public.jpeg"),
        QStringLiteral("public.tiff"),
        QStringLiteral("com.apple.tiff"),
        QStringLiteral("application/x-qt-image")
    };

    for (const QString &format : imageFormats) {
        if (!mimeData->hasFormat(format))
            continue;

        const QByteArray data = mimeData->data(format);
        if (data.isEmpty())
            continue;

        QImage image = QImage::fromData(data);
        if (!image.isNull())
            return image;
    }

    if (mimeData->hasUrls()) {
        for (const QUrl &url : mimeData->urls()) {
            if (!url.isLocalFile())
                continue;

            const QImage image(url.toLocalFile());
            if (!image.isNull())
                return image;
        }
    }

    return {};
}

QString launchAgentPath()
{
    const QString directory = QDir::home().filePath(QStringLiteral("Library/LaunchAgents"));
    return QDir(directory).filePath(QStringLiteral("%1.plist").arg(QString::fromLatin1(LaunchAgentLabel)));
}

bool launchAgentEnabled()
{
    return QFileInfo::exists(launchAgentPath());
}

bool writeLaunchAgent()
{
    const QString path = launchAgentPath();
    QDir().mkpath(QFileInfo(path).absolutePath());

    const QString executablePath = QCoreApplication::applicationFilePath();
    const QString plist = QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n"
        "<dict>\n"
        "  <key>Label</key>\n"
        "  <string>%1</string>\n"
        "  <key>ProgramArguments</key>\n"
        "  <array>\n"
        "    <string>%2</string>\n"
        "  </array>\n"
        "  <key>RunAtLoad</key>\n"
        "  <true/>\n"
        "</dict>\n"
        "</plist>\n")
                              .arg(QString::fromLatin1(LaunchAgentLabel),
                                   executablePath.toHtmlEscaped());

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    file.write(plist.toUtf8());
    return file.commit();
}

void loadLaunchAgent()
{
    QProcess::startDetached(QStringLiteral("launchctl"), {QStringLiteral("load"), launchAgentPath()});
}

void unloadLaunchAgent()
{
    QProcess::startDetached(QStringLiteral("launchctl"), {QStringLiteral("unload"), launchAgentPath()});
}
}

MacServerController::MacServerController(QObject *parent)
    : QObject(parent), m_clipboard(QApplication::clipboard()), m_server(&m_store)
{
    QCoreApplication::setOrganizationName(QStringLiteral("LocalTools"));
    QCoreApplication::setApplicationName(QStringLiteral("NetworkClipboardMacServer"));

    QSettings settings;
    m_autoPublish = settings.value(QStringLiteral("server/autoPublish"), true).toBool();
    m_masterServer = settings.value(QStringLiteral("server/isMaster"), true).toBool();
    m_token = settings.value(QStringLiteral("server/token")).toString();
    if (m_token.isEmpty()) {
        m_token = QUuid::createUuid().toString(QUuid::WithoutBraces);
        settings.setValue(QStringLiteral("server/token"), m_token);
    }
    m_deviceId = deviceId();

    buildMenu();

    connect(m_clipboard, &QClipboard::dataChanged, this, &MacServerController::onClipboardChanged);
    connect(&m_clipboardChangeTimer, &QTimer::timeout, this, &MacServerController::processClipboardChange);
    m_clipboardChangeTimer.setSingleShot(true);
    m_clipboardChangeTimer.setInterval(250);

    connect(&m_clipboardPollTimer, &QTimer::timeout, this, &MacServerController::processClipboardChange);
    m_clipboardPollTimer.setInterval(750);
    m_clipboardPollTimer.start();

    connect(&m_store, &ClipboardStore::latestChanged, this, [this](const ClipboardEntry &entry) {
        applyEntryToClipboard(entry, false, false);
        if (m_contentWindow && m_contentWindow->isVisible())
            updateContentWindow();
    });

    QTimer::singleShot(0, this, &MacServerController::startServer);
    QTimer::singleShot(750, this, [this]() { publishClipboardNow(); });
}

void MacServerController::show()
{
    m_tray.show();
}

void MacServerController::startServer()
{
    m_server.setMasterServer(m_masterServer);

    QString error;
    if (!m_server.start(ApiPort, m_token, deviceName(), &error)) {
        showStatus(QStringLiteral("Server konnte nicht gestartet werden: %1").arg(error));
        return;
    }

    showStatus(QStringLiteral("Server aktiv auf Port %1.").arg(m_server.port()));
}

void MacServerController::buildMenu()
{
    m_menu = new QMenu();

    QAction *showContentAction = m_menu->addAction(QStringLiteral("Show Content"));
    connect(showContentAction, &QAction::triggered, this, &MacServerController::showContent);
    m_menu->addSeparator();

    QAction *pasteAction = m_menu->addAction(QStringLiteral("Paste from Network"));
    connect(pasteAction, &QAction::triggered, this, &MacServerController::pasteFromNetwork);

    QAction *publishAction = m_menu->addAction(QStringLiteral("Send Clipboard Now"));
    connect(publishAction, &QAction::triggered, this, &MacServerController::publishClipboardNow);

    QAction *copyInfoAction = m_menu->addAction(QStringLiteral("Copy Server Info"));
    connect(copyInfoAction, &QAction::triggered, this, &MacServerController::copyServerInfo);

    QAction *clearAction = m_menu->addAction(QStringLiteral("Clear History"));
    connect(clearAction, &QAction::triggered, this, &MacServerController::clearHistory);

    m_menu->addSeparator();
    m_autoPublishAction = m_menu->addAction(QStringLiteral("Auto-send macOS Clipboard"));
    m_autoPublishAction->setCheckable(true);
    m_autoPublishAction->setChecked(m_autoPublish);
    connect(m_autoPublishAction, &QAction::toggled, this, &MacServerController::setAutoPublish);

    m_masterAction = m_menu->addAction(QStringLiteral("Is Master"));
    m_masterAction->setCheckable(true);
    m_masterAction->setChecked(m_masterServer);
    connect(m_masterAction, &QAction::toggled, this, &MacServerController::setMasterServer);

    m_startAtLoginAction = m_menu->addAction(QStringLiteral("Start at Login"));
    m_startAtLoginAction->setCheckable(true);
    connect(m_startAtLoginAction, &QAction::toggled, this, &MacServerController::setStartAtLogin);
    updateStartAtLoginAction();

    m_menu->addSeparator();
    QAction *quitAction = m_menu->addAction(QStringLiteral("Quit"));
    connect(quitAction, &QAction::triggered, qApp, &QApplication::quit);

    m_tray.setContextMenu(m_menu);
    m_tray.setToolTip(QStringLiteral("Network Clipboard"));

    QIcon trayIcon(QStringLiteral(":/icons/net-copy-paste.png"));
    if (trayIcon.isNull())
        trayIcon = QApplication::style()->standardIcon(QStyle::SP_DriveNetIcon);
    m_tray.setIcon(trayIcon);
    QApplication::setWindowIcon(trayIcon);
}

void MacServerController::onClipboardChanged()
{
    m_clipboardChangeTimer.start();
}

void MacServerController::processClipboardChange()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now < m_ignoreClipboardChangesUntil)
        return;

    const QMimeData *mimeData = m_clipboard->mimeData();

    if (mimeData) {
        const QImage image = imageFromClipboard(m_clipboard, mimeData);
        const QByteArray pngData = imagePngData(image);
        if (!pngData.isEmpty()) {
            const QByteArray hash = imageHash(QImage::fromData(pngData, "PNG"));
            if (!m_autoPublish || hash == m_lastPublishedImageHash)
                return;
            publishClipboardImage(image, false);
            return;
        }
    }

    const QString text = m_clipboard->text().trimmed();
    if (!m_autoPublish || text.isEmpty() || text == m_lastPublishedContent)
        return;

    publishClipboardText(text, false);
}

void MacServerController::publishClipboardNow()
{
    const QMimeData *mimeData = m_clipboard->mimeData();
    if (mimeData) {
        const QImage image = imageFromClipboard(m_clipboard, mimeData);
        if (!imagePngData(image).isEmpty()) {
            publishClipboardImage(image, true, true);
            return;
        }
    }

    publishClipboardText(m_clipboard->text().trimmed(), true, true);
}

void MacServerController::showContent()
{
    if (!m_contentWindow) {
        m_contentWindow = new ClipboardContentWindow();
        connect(m_contentWindow, &ClipboardContentWindow::makeCurrentRequested, this, [this](ClipboardEntry entry) {
            entry.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            entry.deviceId = m_deviceId;
            entry.deviceName = deviceName();
            entry.timestamp = QDateTime::currentSecsSinceEpoch();
            m_store.setLatest(entry);
            applyEntryToClipboard(entry, false, true);
        });
    }

    updateContentWindow();

    m_contentWindow->show();
    m_contentWindow->raise();
    m_contentWindow->activateWindow();
}

void MacServerController::updateContentWindow()
{
    if (!m_contentWindow)
        return;

    const QList<ClipboardEntry> history = m_store.history();
    if (!history.isEmpty())
        m_contentWindow->setEntries(history.mid(0, 15));
    else
        m_contentWindow->setMessage(QStringLiteral("No network clipboard entry available."));
}

void MacServerController::pasteFromNetwork()
{
    const std::optional<ClipboardEntry> latest = m_store.latest();
    if (!latest) {
        showStatus(QStringLiteral("No network clipboard entry available."));
        return;
    }

    applyEntryToClipboard(*latest, true, true);
}

void MacServerController::copyServerInfo()
{
    const QString info = serverInfo();
    m_ignoreClipboardChangesUntil = QDateTime::currentMSecsSinceEpoch() + ClipboardIgnoreWindowMs;
    m_ignoredClipboardContent = info.trimmed();
    m_ignoredClipboardImageHash.clear();
    m_clipboard->setText(info);
    showStatus(QStringLiteral("Server info copied to clipboard."));
}

void MacServerController::clearHistory()
{
    m_store.clear();
    showStatus(QStringLiteral("Network clipboard history cleared."));
}

void MacServerController::setAutoPublish(bool enabled)
{
    m_autoPublish = enabled;
    QSettings().setValue(QStringLiteral("server/autoPublish"), enabled);
}

void MacServerController::setMasterServer(bool enabled)
{
    m_masterServer = enabled;
    QSettings().setValue(QStringLiteral("server/isMaster"), enabled);
    m_server.setMasterServer(enabled);
}

void MacServerController::setStartAtLogin(bool enabled)
{
    if (enabled) {
        if (!writeLaunchAgent()) {
            showStatus(QStringLiteral("Could not write LaunchAgent."));
            updateStartAtLoginAction();
            return;
        }
        loadLaunchAgent();
        showStatus(QStringLiteral("Start at login enabled."));
    } else {
        unloadLaunchAgent();
        QFile::remove(launchAgentPath());
        showStatus(QStringLiteral("Start at login disabled."));
    }

    updateStartAtLoginAction();
}

void MacServerController::updateStartAtLoginAction()
{
    if (!m_startAtLoginAction)
        return;

    const bool blocked = m_startAtLoginAction->blockSignals(true);
    m_startAtLoginAction->setChecked(launchAgentEnabled());
    m_startAtLoginAction->blockSignals(blocked);
}

void MacServerController::applyEntryToClipboard(const ClipboardEntry &entry, bool showMessage, bool allowOwnEntry)
{
    if (entry.content.isEmpty())
        return;

    if (!allowOwnEntry && entry.deviceId == m_deviceId) {
        m_lastSeenEntryId = entry.id;
        return;
    }

    if (!allowOwnEntry && !entry.id.isEmpty() && entry.id == m_lastSeenEntryId)
        return;

    m_lastSeenEntryId = entry.id;
    m_ignoreClipboardChangesUntil = QDateTime::currentMSecsSinceEpoch() + ClipboardIgnoreWindowMs;

    if (entry.type == QStringLiteral("image")) {
        const QByteArray pngData = QByteArray::fromBase64(entry.content.toLatin1());
        const QImage image = QImage::fromData(pngData, "PNG");
        if (image.isNull()) {
            if (showMessage)
                showStatus(QStringLiteral("Network clipboard image is invalid."));
            return;
        }

        const QByteArray hash = imageHash(image);
        m_ignoredClipboardContent.clear();
        m_ignoredClipboardImageHash = hash;
        m_lastPublishedContent.clear();
        m_lastPublishedImageHash = hash;
        m_clipboard->setImage(image);
        if (showMessage)
            showStatus(QStringLiteral("Copied latest network image to macOS clipboard."));
        return;
    }

    QString content = entry.content;
    content.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    content.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    m_ignoredClipboardContent = content;
    m_ignoredClipboardImageHash.clear();
    m_lastPublishedContent = content;
    m_lastPublishedImageHash.clear();
    m_clipboard->setText(content);
    if (showMessage)
        showStatus(QStringLiteral("Copied latest network entry to macOS clipboard."));
}

void MacServerController::publishClipboardText(const QString &text, bool showMessage, bool force)
{
    if (text.isEmpty()) {
        if (showMessage)
            showStatus(QStringLiteral("macOS clipboard is empty."));
        return;
    }

    if (!force && text == m_lastPublishedContent)
        return;

    ClipboardEntry entry;
    entry.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    entry.deviceId = m_deviceId;
    entry.deviceName = deviceName();
    entry.type = isUrlText(text) ? QStringLiteral("url") : QStringLiteral("text");
    entry.content = text;
    entry.timestamp = QDateTime::currentSecsSinceEpoch();

    QString error;
    if (!entry.isValid(&error)) {
        if (showMessage)
            showStatus(error);
        return;
    }

    const std::optional<ClipboardEntry> latest = m_store.latest();
    if (!force
        && !showMessage
        && latest
        && latest->deviceId != m_deviceId
        && latest->timestamp > 0
        && QDateTime::currentSecsSinceEpoch() - latest->timestamp <= FreshRemoteEntryWindowSecs
        && !sameClipboardPayload(entry, *latest)) {
        applyEntryToClipboard(*latest, false, false);
        return;
    }

    m_lastPublishedContent = text;
    m_lastPublishedImageHash.clear();
    if (!latest || !sameClipboardPayload(entry, *latest))
        m_store.setLatest(entry);
    if (showMessage)
        showStatus(QStringLiteral("Sent macOS clipboard to server."));
}

void MacServerController::publishClipboardImage(const QImage &image, bool showMessage, bool force)
{
    const QByteArray pngData = imagePngData(image);
    if (pngData.isEmpty()) {
        if (showMessage)
            showStatus(QStringLiteral("macOS clipboard contains no usable image."));
        return;
    }

    const QByteArray hash = imageHash(QImage::fromData(pngData, "PNG"));
    if (!force && hash == m_lastPublishedImageHash)
        return;

    ClipboardEntry entry;
    entry.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    entry.deviceId = m_deviceId;
    entry.deviceName = deviceName();
    entry.type = QStringLiteral("image");
    entry.mimeType = QStringLiteral("image/png");
    entry.content = QString::fromLatin1(pngData.toBase64());
    entry.timestamp = QDateTime::currentSecsSinceEpoch();

    QString error;
    if (!entry.isValid(&error)) {
        if (showMessage)
            showStatus(error);
        return;
    }

    const std::optional<ClipboardEntry> latest = m_store.latest();
    if (!force
        && !showMessage
        && latest
        && latest->deviceId != m_deviceId
        && latest->timestamp > 0
        && QDateTime::currentSecsSinceEpoch() - latest->timestamp <= FreshRemoteEntryWindowSecs
        && !sameClipboardPayload(entry, *latest)) {
        applyEntryToClipboard(*latest, false, false);
        return;
    }

    m_lastPublishedImageHash = hash;
    m_lastPublishedContent.clear();
    if (!latest || !sameClipboardPayload(entry, *latest))
        m_store.setLatest(entry);
    if (showMessage)
        showStatus(QStringLiteral("Sent macOS image clipboard to server."));
}

void MacServerController::showStatus(const QString &message)
{
    m_tray.showMessage(QStringLiteral("Network Clipboard"), message);
}

QString MacServerController::deviceName() const
{
    const QString hostName = QHostInfo::localHostName().trimmed();
    return hostName.isEmpty() ? QStringLiteral("Mac Server") : hostName;
}

QString MacServerController::deviceId()
{
    QSettings settings;
    QString id = settings.value(QStringLiteral("device/id")).toString();
    if (id.isEmpty()) {
        id = QStringLiteral("macos-server-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        settings.setValue(QStringLiteral("device/id"), id);
    }
    return id;
}

QString MacServerController::serverInfo() const
{
    const QStringList urls = m_server.serverUrls();
    const QString urlText = urls.isEmpty()
        ? QStringLiteral("http://127.0.0.1:%1").arg(ApiPort)
        : urls.join(QLatin1Char('\n'));
    return QStringLiteral("URL:\n%1\nAuthorization: Bearer %2").arg(urlText, m_token);
}
