#include "ClipboardBridge.h"
#include "NetworkClipboardClient.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QUrl>

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("LocalTools"));
    QCoreApplication::setApplicationName(QStringLiteral("NetworkClipboard"));
    QQuickStyle::setStyle(QStringLiteral("Material"));

    NetworkClipboardClient client;
    ClipboardBridge clipboard;

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("networkClipboard"), &client);
    engine.rootContext()->setContextProperty(QStringLiteral("localClipboard"), &clipboard);
    engine.load(QUrl(QStringLiteral("qrc:/NetworkClipboardIOS/Main.qml")));

    if (engine.rootObjects().isEmpty())
        return 1;

    return app.exec();
}
