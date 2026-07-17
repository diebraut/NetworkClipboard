#include "TrayController.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QHostInfo>
#include <QLockFile>
#include <QSettings>
#include <QStringList>
#include <QUrl>
#include <QUuid>

namespace {
QString serviceConfigPath()
{
    const QStringList candidates = {
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("NetworkClipboardService.ini")),
        QStringLiteral("K:/Program Files/NetworkClipboard/NetworkClipboardService.ini"),
        QDir(qEnvironmentVariable("ProgramFiles")).filePath(QStringLiteral("NetworkClipboard/NetworkClipboardService.ini"))
    };

    for (const QString &candidate : candidates) {
        if (QFileInfo::exists(candidate))
            return candidate;
    }

    return candidates.first();
}
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setQuitOnLastWindowClosed(false);
    QCoreApplication::setOrganizationName(QStringLiteral("LocalTools"));
    QCoreApplication::setApplicationName(QStringLiteral("NetworkClipboard"));

    QLockFile instanceLock(QDir::temp().filePath(QStringLiteral("NetworkClipboardWindows.lock")));
    instanceLock.setStaleLockTime(30000);
    if (!instanceLock.tryLock(100))
        return 0;

    QSettings settings;
    QSettings serviceSettings(serviceConfigPath(), QSettings::IniFormat);
    const bool hasServiceConfig = QFileInfo::exists(serviceConfigPath());

    const quint16 port = hasServiceConfig
        ? serviceSettings.value(QStringLiteral("server/port"), 8787).toUInt()
        : settings.value(QStringLiteral("server/port"), 8787).toUInt();

    QString token = hasServiceConfig
        ? serviceSettings.value(QStringLiteral("server/token")).toString()
        : settings.value(QStringLiteral("server/token")).toString();
    if (token.isEmpty()) {
        token = QUuid::createUuid().toString(QUuid::WithoutBraces);
        settings.setValue(QStringLiteral("server/token"), token);
    }

    const QString localServiceUrl = QStringLiteral("http://127.0.0.1:%1").arg(port);
    const QUrl serverUrl(hasServiceConfig
                             ? localServiceUrl
                             : settings.value(QStringLiteral("server/url"), localServiceUrl).toString());
    if (hasServiceConfig)
        settings.setValue(QStringLiteral("server/url"), localServiceUrl);

    QString deviceId = settings.value(QStringLiteral("device/id")).toString();
    if (deviceId.isEmpty()) {
        deviceId = QStringLiteral("windows-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        settings.setValue(QStringLiteral("device/id"), deviceId);
    }
    const QString deviceName = settings.value(QStringLiteral("device/name"), QHostInfo::localHostName()).toString();

    TrayController tray(deviceId, deviceName);
    tray.setServerInfo(serverUrl, port, token);
    const QString localServiceExecutable = QDir(QCoreApplication::applicationDirPath())
                                               .filePath(QStringLiteral("NetworkClipboardService.exe"));
    if (QFileInfo::exists(localServiceExecutable)
        && QDir::cleanPath(QCoreApplication::applicationDirPath()).compare(
               QDir::cleanPath(QStringLiteral(NETWORKCLIPBOARD_BUILD_DIR)), Qt::CaseInsensitive) == 0) {
        tray.setDevelopmentServiceExecutable(localServiceExecutable);
    }
    tray.show();

    return app.exec();
}
