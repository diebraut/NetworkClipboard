#include "AndroidServerController.h"

#include <QDateTime>
#include <QGuiApplication>
#include <QHostInfo>
#include <QSettings>
#include <QUrl>
#include <QUuid>

#ifdef Q_OS_ANDROID
#include <QtCore/qcoreapplication_platform.h>
#endif

namespace {
#ifdef Q_OS_ANDROID
constexpr quint16 ApiPort = 8787;
#else
constexpr quint16 ApiPort = 8789;
#endif
constexpr qint64 ClipboardIgnoreWindowMs = 1500;

bool isUrlText(const QString &text)
{
    return text.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive)
        || text.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive);
}
}

AndroidServerController::AndroidServerController(QObject *parent)
    : QObject(parent), m_server(&m_store), m_clipboard(QGuiApplication::clipboard())
{
    QSettings settings;
    m_token = settings.value(QStringLiteral("server/token")).toString();
    if (m_token.isEmpty()) {
        m_token = QUuid::createUuid().toString(QUuid::WithoutBraces);
        settings.setValue(QStringLiteral("server/token"), m_token);
    }

    connect(m_clipboard, &QClipboard::dataChanged, this, &AndroidServerController::onClipboardChanged);
    connect(&m_store, &ClipboardStore::latestChanged, this, [this](const ClipboardEntry &entry) {
        setLatestContent(entry.content);
        if (entry.deviceId != m_deviceId)
            applyEntryToClipboard(entry);
    });

    QTimer::singleShot(0, this, &AndroidServerController::start);
    QTimer::singleShot(750, this, [this]() { publishClipboardText(m_clipboard->text().trimmed(), false); });
}

QString AndroidServerController::status() const { return m_status; }
QString AndroidServerController::token() const { return m_token; }
QString AndroidServerController::serverInfo() const { return currentServerInfo(); }
QString AndroidServerController::latestContent() const { return m_latestContent; }
bool AndroidServerController::autoPublish() const { return m_autoPublish; }

void AndroidServerController::setAutoPublish(bool enabled)
{
    if (m_autoPublish == enabled)
        return;
    m_autoPublish = enabled;
    emit autoPublishChanged();
}

void AndroidServerController::publishClipboardNow()
{
    publishClipboardText(m_clipboard->text().trimmed(), true);
}

void AndroidServerController::copyServerInfo()
{
    m_clipboard->setText(currentServerInfo());
}

void AndroidServerController::start()
{
    QString error;
    m_deviceId = deviceId();
    acquireMulticastLock();
    if (!m_server.start(ApiPort, m_token, deviceName(), &error)) {
        setStatus(QStringLiteral("Server konnte nicht gestartet werden: %1").arg(error));
        return;
    }

    m_started = true;
    setStatus(QStringLiteral("Server aktiv auf Port %1").arg(m_server.port()));
    emit serverInfoChanged();
}

void AndroidServerController::acquireMulticastLock()
{
#ifdef Q_OS_ANDROID
    QJniObject context = QNativeInterface::QAndroidApplication::context();
    if (!context.isValid())
        return;

    QJniObject serviceName = QJniObject::fromString(QStringLiteral("wifi"));
    QJniObject wifiManager = context.callObjectMethod("getSystemService",
                                                       "(Ljava/lang/String;)Ljava/lang/Object;",
                                                       serviceName.object<jstring>());
    if (!wifiManager.isValid())
        return;

    QJniObject lockName = QJniObject::fromString(QStringLiteral("NetworkClipboardDiscovery"));
    m_multicastLock = wifiManager.callObjectMethod("createMulticastLock",
                                                   "(Ljava/lang/String;)Landroid/net/wifi/WifiManager$MulticastLock;",
                                                   lockName.object<jstring>());
    if (!m_multicastLock.isValid())
        return;

    m_multicastLock.callMethod<void>("setReferenceCounted", "(Z)V", false);
    m_multicastLock.callMethod<void>("acquire", "()V");
#endif
}

void AndroidServerController::onClipboardChanged()
{
    const QString text = m_clipboard->text().trimmed();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now < m_ignoreClipboardChangesUntil && text == m_ignoredClipboardContent)
        return;
    if (!m_autoPublish || !m_started || text.isEmpty() || text == m_lastPublishedContent)
        return;

    publishClipboardText(text, false);
}

void AndroidServerController::publishClipboardText(const QString &text, bool force)
{
    if (!m_started || text.isEmpty())
        return;
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
        setStatus(error);
        return;
    }

    m_lastPublishedContent = text;
    m_store.setLatest(entry);
    setStatus(QStringLiteral("Server aktiv auf Port %1. Android Clipboard veroeffentlicht.").arg(m_server.port()));
}

void AndroidServerController::applyEntryToClipboard(const ClipboardEntry &entry)
{
    if (entry.content.trimmed().isEmpty())
        return;

    m_ignoreClipboardChangesUntil = QDateTime::currentMSecsSinceEpoch() + ClipboardIgnoreWindowMs;
    m_ignoredClipboardContent = entry.content;
    m_lastPublishedContent = entry.content;
    m_clipboard->setText(entry.content);
    setStatus(QStringLiteral("Server aktiv auf Port %1. Netzwerk-Clipboard nach Android uebernommen.").arg(m_server.port()));
}

void AndroidServerController::setStatus(const QString &status)
{
    if (m_status == status)
        return;
    m_status = status;
    emit statusChanged();
}

void AndroidServerController::setLatestContent(const QString &content)
{
    if (m_latestContent == content)
        return;
    m_latestContent = content;
    emit latestContentChanged();
}

QString AndroidServerController::deviceName() const
{
    const QString hostName = QHostInfo::localHostName().trimmed();
    return hostName.isEmpty() ? QStringLiteral("Android Server") : hostName;
}

QString AndroidServerController::deviceId()
{
    QSettings settings;
    QString id = settings.value(QStringLiteral("device/id")).toString();
    if (id.isEmpty()) {
        id = QStringLiteral("android-server-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        settings.setValue(QStringLiteral("device/id"), id);
    }
    return id;
}

QString AndroidServerController::currentServerInfo() const
{
    const QStringList urls = m_server.serverUrls();
    const QString urlText = urls.isEmpty()
        ? QStringLiteral("http://<android-lan-ip>:%1").arg(ApiPort)
        : urls.join(QLatin1Char('\n'));
    return QStringLiteral("URL:\n%1\nAuthorization: Bearer %2").arg(urlText, m_token);
}
