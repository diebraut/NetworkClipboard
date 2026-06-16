#include "AndroidServerController.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("LocalTools"));
    QCoreApplication::setApplicationName(QStringLiteral("NetworkClipboardAndroidServer"));

    AndroidServerController server;

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("androidServer"), &server);
    engine.load(QUrl(QStringLiteral("qrc:/NetworkClipboardAndroidServer/Main.qml")));
    if (engine.rootObjects().isEmpty())
        return 1;

    return app.exec();
}
