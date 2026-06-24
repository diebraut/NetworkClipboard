#include "AndroidServerController.h"

#include <QDateTime>
#include <QBuffer>
#include <QByteArray>
#include <QCryptographicHash>
#include <QGuiApplication>
#include <QHostAddress>
#include <QHostInfo>
#include <QImage>
#include <QJsonDocument>
#include <QNetworkInterface>
#include <QSettings>
#include <QUrl>
#include <QUuid>

#ifdef Q_OS_ANDROID
#include <QJniEnvironment>
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

QString fingerprintForBase64(const QString &base64)
{
    if (base64.isEmpty())
        return {};
    return QString::fromLatin1(QCryptographicHash::hash(base64.toLatin1(), QCryptographicHash::Sha256).toHex());
}
}

AndroidServerController::AndroidServerController(QObject *parent)
    : QObject(parent), m_server(&m_store), m_clipboard(QGuiApplication::clipboard())
{
    QSettings settings;
    m_masterServer = settings.value(QStringLiteral("server/isMaster"), false).toBool();
    m_server.setMasterServer(m_masterServer);
    m_token = settings.value(QStringLiteral("server/token")).toString();
    if (m_token.isEmpty()) {
        m_token = QUuid::createUuid().toString(QUuid::WithoutBraces);
        settings.setValue(QStringLiteral("server/token"), m_token);
    }

    connect(m_clipboard, &QClipboard::dataChanged, this, &AndroidServerController::onClipboardChanged);
    connect(&m_store, &ClipboardStore::latestChanged, this, [this](const ClipboardEntry &entry) {
        setLatestEntry(entry);
        if (entry.deviceId != m_deviceId)
            applyEntryToClipboard(entry);
    });

    QTimer::singleShot(0, this, &AndroidServerController::start);
    QTimer::singleShot(750, this, [this]() { onClipboardChanged(); });
}

QString AndroidServerController::status() const { return m_status; }
QString AndroidServerController::token() const { return m_token; }
QString AndroidServerController::serverInfo() const { return currentServerInfo(); }
QString AndroidServerController::serverUrlsText() const { return currentServerUrls().join(QLatin1Char('\n')); }
QString AndroidServerController::latestContent() const { return m_latestContent; }
QString AndroidServerController::latestImageBase64() const { return m_latestImageBase64; }
bool AndroidServerController::autoPublish() const { return m_autoPublish; }
bool AndroidServerController::masterServer() const { return m_masterServer; }

void AndroidServerController::setAutoPublish(bool enabled)
{
    if (m_autoPublish == enabled)
        return;
    m_autoPublish = enabled;
    emit autoPublishChanged();
}

void AndroidServerController::publishClipboardNow()
{
    const QString imageBase64 = clipboardImageBase64();
    if (!imageBase64.isEmpty()) {
        publishClipboardImage(imageBase64, true);
        return;
    }

    publishClipboardText(m_clipboard->text().trimmed(), true);
}

void AndroidServerController::copyServerInfo()
{
    m_clipboard->setText(currentServerInfo());
}

void AndroidServerController::start()
{
#ifdef Q_OS_ANDROID
    QString foregroundServiceError;
    if (!startForegroundService(&foregroundServiceError)) {
        setStatus(QStringLiteral("Foreground-Service konnte nicht gestartet werden: %1")
                      .arg(foregroundServiceError));
        return;
    }

    if (requestLocalNetworkPermission()) {
        setStatus(QStringLiteral("Bitte die Berechtigung für Geräte in der Nähe bzw. das lokale Netzwerk erlauben."));
        QTimer::singleShot(1500, this, &AndroidServerController::start);
        return;
    }
#endif

    m_deviceId = deviceId();
    acquireMulticastLock();
#ifdef Q_OS_ANDROID
    m_started = true;
#else
    QString error;
    if (!m_server.start(ApiPort, m_token, deviceName(), &error)) {
        setStatus(QStringLiteral("Server konnte nicht gestartet werden: %1").arg(error));
        return;
    }
    m_started = true;
#endif
    emit serverInfoChanged();
    const QStringList urls = currentServerUrls();
    setStatus(urls.isEmpty()
                  ? QStringLiteral("Server aktiv auf Port %1, aber keine LAN-IP gefunden. Android und iPhone muessen im selben WLAN sein.").arg(ApiPort)
                  : QStringLiteral("Server aktiv: %1").arg(urls.join(QStringLiteral(", "))));
}

void AndroidServerController::setMasterServer(bool masterServer)
{
    if (m_masterServer == masterServer)
        return;
    m_masterServer = masterServer;
    QSettings settings;
    settings.setValue(QStringLiteral("server/isMaster"), m_masterServer);
    m_server.setMasterServer(m_masterServer);
    emit masterServerChanged();
    startForegroundService();
}

bool AndroidServerController::startForegroundService(QString *errorMessage)
{
#ifdef Q_OS_ANDROID
    const QJniObject context = QNativeInterface::QAndroidApplication::context();
    if (!context.isValid()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Android-Kontext nicht verfuegbar.");
        return false;
    }

    QJniEnvironment env;
    const QJniObject token = QJniObject::fromString(m_token);
    const QJniObject name = QJniObject::fromString(deviceName());
    QJniObject::callStaticMethod<void>(
        "org/qtproject/example/NetworkClipboardAndroidServer/NetworkClipboardForegroundService",
        "start",
        "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;Z)V",
        context.object(),
        token.object<jstring>(),
        name.object<jstring>(),
        static_cast<jboolean>(m_masterServer));

    if (env.checkAndClearExceptions()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Android hat den Dienststart abgelehnt.");
        return false;
    }
#else
    Q_UNUSED(errorMessage);
#endif
    return true;
}

bool AndroidServerController::requestLocalNetworkPermission()
{
#ifdef Q_OS_ANDROID
    const jint sdkVersion = QJniObject::getStaticField<jint>("android/os/Build$VERSION", "SDK_INT");
    QString permission;
    if (sdkVersion >= 37)
        permission = QStringLiteral("android.permission.ACCESS_LOCAL_NETWORK");
    else if (sdkVersion >= 33)
        permission = QStringLiteral("android.permission.NEARBY_WIFI_DEVICES");
    else
        return false;

    const QJniObject context = QNativeInterface::QAndroidApplication::context();
    if (!context.isValid())
        return false;

    const QJniObject permissionName = QJniObject::fromString(permission);
    const jint granted = context.callMethod<jint>("checkSelfPermission",
                                                  "(Ljava/lang/String;)I",
                                                  permissionName.object<jstring>());
    if (granted == 0)
        return false;

    if (m_localNetworkPermissionRequested)
        return true;
    if (!QNativeInterface::QAndroidApplication::isActivityContext())
        return true;

    const QJniObject activity = context;
    QJniEnvironment env;
    jclass stringClass = env->FindClass("java/lang/String");
    jobjectArray permissions = env->NewObjectArray(1, stringClass, nullptr);
    env->SetObjectArrayElement(permissions, 0, permissionName.object<jstring>());
    activity.callMethod<void>("requestPermissions",
                              "([Ljava/lang/String;I)V",
                              permissions,
                              1001);
    env->DeleteLocalRef(permissions);
    env->DeleteLocalRef(stringClass);
    m_localNetworkPermissionRequested = true;
    return true;
#else
    return false;
#endif
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
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    const QString imageBase64 = clipboardImageBase64();
    if (!imageBase64.isEmpty()) {
        const QString fingerprint = clipboardImageFingerprint();
        if (now < m_ignoreClipboardChangesUntil && fingerprint == m_ignoredClipboardImageFingerprint)
            return;
        if (!m_autoPublish || !m_started || fingerprint == m_lastPublishedImageFingerprint)
            return;

        publishClipboardImage(imageBase64, false);
        return;
    }

    const QString text = m_clipboard->text().trimmed();
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
    m_lastPublishedImageFingerprint.clear();
    m_store.setLatest(entry);
#ifdef Q_OS_ANDROID
    const QJniObject context = QNativeInterface::QAndroidApplication::context();
    const QJniObject json = QJniObject::fromString(
        QString::fromUtf8(QJsonDocument(entry.toJson()).toJson(QJsonDocument::Compact)));
    QJniObject::callStaticMethod<void>(
        "org/qtproject/example/NetworkClipboardAndroidServer/NetworkClipboardForegroundService",
        "publishEntry",
        "(Landroid/content/Context;Ljava/lang/String;)V",
        context.object(),
        json.object<jstring>());
#endif
    setStatus(QStringLiteral("Server aktiv auf Port %1. Android Clipboard veroeffentlicht.").arg(ApiPort));
}

void AndroidServerController::publishClipboardImage(const QString &base64Png, bool force)
{
    if (!m_started || base64Png.isEmpty())
        return;

    const QString fingerprint = fingerprintForBase64(base64Png);
    if (!force && fingerprint == m_lastPublishedImageFingerprint)
        return;

    ClipboardEntry entry;
    entry.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    entry.deviceId = m_deviceId;
    entry.deviceName = deviceName();
    entry.type = QStringLiteral("image");
    entry.mimeType = QStringLiteral("image/png");
    entry.content = base64Png;
    entry.timestamp = QDateTime::currentSecsSinceEpoch();

    QString error;
    if (!entry.isValid(&error)) {
        setStatus(error);
        return;
    }

    m_lastPublishedImageFingerprint = fingerprint;
    m_lastPublishedContent.clear();
    m_store.setLatest(entry);
#ifdef Q_OS_ANDROID
    const QJniObject context = QNativeInterface::QAndroidApplication::context();
    const QJniObject json = QJniObject::fromString(
        QString::fromUtf8(QJsonDocument(entry.toJson()).toJson(QJsonDocument::Compact)));
    QJniObject::callStaticMethod<void>(
        "org/qtproject/example/NetworkClipboardAndroidServer/NetworkClipboardForegroundService",
        "publishEntry",
        "(Landroid/content/Context;Ljava/lang/String;)V",
        context.object(),
        json.object<jstring>());
#endif
    setStatus(QStringLiteral("Server aktiv auf Port %1. Android Bild-Clipboard veroeffentlicht.").arg(ApiPort));
}

void AndroidServerController::applyEntryToClipboard(const ClipboardEntry &entry)
{
    if (entry.content.trimmed().isEmpty())
        return;

    m_ignoreClipboardChangesUntil = QDateTime::currentMSecsSinceEpoch() + ClipboardIgnoreWindowMs;
    if (entry.type == QStringLiteral("image")) {
        const QString fingerprint = fingerprintForBase64(entry.content);
        if (!setClipboardImageBase64(entry.content)) {
            setStatus(QStringLiteral("Server aktiv auf Port %1. Netzwerk-Bild konnte nicht nach Android uebernommen werden.").arg(ApiPort));
            return;
        }
        m_ignoredClipboardContent.clear();
        m_ignoredClipboardImageFingerprint = fingerprint;
        m_lastPublishedContent.clear();
        m_lastPublishedImageFingerprint = fingerprint;
        setStatus(QStringLiteral("Server aktiv auf Port %1. Netzwerk-Bild nach Android uebernommen.").arg(ApiPort));
        return;
    }

    m_ignoredClipboardContent = entry.content;
    m_ignoredClipboardImageFingerprint.clear();
    m_lastPublishedContent = entry.content;
    m_lastPublishedImageFingerprint.clear();
    m_clipboard->setText(entry.content);
    setStatus(QStringLiteral("Server aktiv auf Port %1. Netzwerk-Clipboard nach Android uebernommen.").arg(ApiPort));
}

void AndroidServerController::setStatus(const QString &status)
{
    if (m_status == status)
        return;
    m_status = status;
    emit statusChanged();
}

void AndroidServerController::setLatestEntry(const ClipboardEntry &entry)
{
    const QString latestContent = entry.type == QStringLiteral("image") ? QString{} : entry.content;
    const QString latestImageBase64 = entry.type == QStringLiteral("image") ? entry.content : QString{};
    if (m_latestContent == latestContent && m_latestImageBase64 == latestImageBase64)
        return;
    m_latestContent = latestContent;
    m_latestImageBase64 = latestImageBase64;
    emit latestContentChanged();
}

QString AndroidServerController::clipboardImageBase64() const
{
#ifdef Q_OS_ANDROID
    const QJniObject context = QNativeInterface::QAndroidApplication::context();
    if (!context.isValid())
        return {};

    const QJniObject result = QJniObject::callStaticObjectMethod(
        "org/qtproject/example/NetworkClipboardAndroidServer/ImageClipboardHelper",
        "imageBase64",
        "(Landroid/content/Context;)Ljava/lang/String;",
        context.object());
    return result.isValid() ? result.toString() : QString{};
#else
    const QImage image = m_clipboard->image();
    if (image.isNull())
        return {};
    QByteArray data;
    QBuffer buffer(&data);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG"))
        return {};
    return QString::fromLatin1(data.toBase64());
#endif
}

QString AndroidServerController::clipboardImageFingerprint() const
{
#ifdef Q_OS_ANDROID
    const QJniObject context = QNativeInterface::QAndroidApplication::context();
    if (!context.isValid())
        return {};

    const QJniObject key = QJniObject::callStaticObjectMethod(
        "org/qtproject/example/NetworkClipboardAndroidServer/ImageClipboardHelper",
        "imageKey",
        "(Landroid/content/Context;)Ljava/lang/String;",
        context.object());
    if (key.isValid() && !key.toString().isEmpty())
        return key.toString();
#endif
    return fingerprintForBase64(clipboardImageBase64());
}

bool AndroidServerController::setClipboardImageBase64(const QString &base64Png)
{
#ifdef Q_OS_ANDROID
    const QJniObject context = QNativeInterface::QAndroidApplication::context();
    if (!context.isValid())
        return false;

    const QJniObject encoded = QJniObject::fromString(base64Png);
    return QJniObject::callStaticMethod<jboolean>(
        "org/qtproject/example/NetworkClipboardAndroidServer/ImageClipboardHelper",
        "setImageBase64",
        "(Landroid/content/Context;Ljava/lang/String;)Z",
        context.object(),
        encoded.object<jstring>());
#else
    const QImage image = QImage::fromData(QByteArray::fromBase64(base64Png.toLatin1()), "PNG");
    if (image.isNull())
        return false;
    m_clipboard->setImage(image);
    return true;
#endif
}

QString AndroidServerController::deviceName() const
{
#ifdef Q_OS_ANDROID
    return QStringLiteral("Android-Server");
#else
    const QString hostName = QHostInfo::localHostName().trimmed();
    return hostName.isEmpty() ? QStringLiteral("Android Server") : hostName;
#endif
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
    const QStringList urls = currentServerUrls();
    const QString urlText = urls.isEmpty()
        ? QStringLiteral("http://<android-lan-ip>:%1").arg(ApiPort)
        : urls.join(QLatin1Char('\n'));
    return QStringLiteral("URL:\n%1\nAuthorization: Bearer %2").arg(urlText, m_token);
}

QStringList AndroidServerController::currentServerUrls() const
{
#ifdef Q_OS_ANDROID
    QStringList urls{QStringLiteral("http://127.0.0.1:%1").arg(ApiPort)};
    const auto interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &networkInterface : interfaces) {
        const auto flags = networkInterface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp)
            || !flags.testFlag(QNetworkInterface::IsRunning)
            || flags.testFlag(QNetworkInterface::IsLoopBack)) {
            continue;
        }
        for (const QNetworkAddressEntry &entry : networkInterface.addressEntries()) {
            const QHostAddress address = entry.ip();
            if (address.protocol() == QAbstractSocket::IPv4Protocol && !address.isLoopback())
                urls.append(QStringLiteral("http://%1:%2").arg(address.toString()).arg(ApiPort));
        }
    }
    urls.removeDuplicates();
    return urls;
#else
    return m_server.serverUrls();
#endif
}
