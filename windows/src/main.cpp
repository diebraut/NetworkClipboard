#include "ApiServer.h"
#include "TrayController.h"

#include <QApplication>
#include <QHostInfo>
#include <QMessageBox>
#include <QSettings>
#include <QUuid>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setQuitOnLastWindowClosed(false);
    QCoreApplication::setOrganizationName(QStringLiteral("LocalTools"));
    QCoreApplication::setApplicationName(QStringLiteral("NetworkClipboard"));

    QSettings settings;
    const quint16 port = settings.value(QStringLiteral("server/port"), 8787).toUInt();
    QString token = settings.value(QStringLiteral("server/token")).toString();
    if (token.isEmpty()) {
        token = QUuid::createUuid().toString(QUuid::WithoutBraces);
        settings.setValue(QStringLiteral("server/token"), token);
    }

    QString deviceId = settings.value(QStringLiteral("device/id")).toString();
    if (deviceId.isEmpty()) {
        deviceId = QStringLiteral("windows-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        settings.setValue(QStringLiteral("device/id"), deviceId);
    }
    const QString deviceName = settings.value(QStringLiteral("device/name"), QHostInfo::localHostName()).toString();

    ClipboardStore store;
    ApiServer server(&store);
    QString error;
    if (!server.start(port, token, &error)) {
        QMessageBox::critical(nullptr, QStringLiteral("Network Clipboard"), QStringLiteral("Could not start server on port %1:\n%2").arg(port).arg(error));
        return 1;
    }

    TrayController tray(&store, deviceId, deviceName);
    tray.setServerInfo(server.port(), token);
    tray.show();

    return app.exec();
}
