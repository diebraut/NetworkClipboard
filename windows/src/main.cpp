#include "TrayController.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QHostInfo>
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

    const QUrl serverUrl(settings.value(QStringLiteral("server/url"), QStringLiteral("http://127.0.0.1:%1").arg(port)).toString());

    QString deviceId = settings.value(QStringLiteral("device/id")).toString();
    if (deviceId.isEmpty()) {
        deviceId = QStringLiteral("windows-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        settings.setValue(QStringLiteral("device/id"), deviceId);
    }
    const QString deviceName = settings.value(QStringLiteral("device/name"), QHostInfo::localHostName()).toString();

    TrayController tray(deviceId, deviceName);
    tray.setServerInfo(serverUrl, port, token);
    tray.show();

    return app.exec();
}
