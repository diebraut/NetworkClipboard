#include "ApiServer.h"
#include "ClipboardStore.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QHostInfo>
#include <QMetaObject>
#include <QSettings>
#include <QUuid>

#include <windows.h>

namespace {
constexpr wchar_t ServiceName[] = L"NetworkClipboardServer";

SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
SERVICE_STATUS g_status = {};
QCoreApplication *g_app = nullptr;

void setServiceStatus(DWORD state, DWORD win32ExitCode = NO_ERROR, DWORD waitHint = 0)
{
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = state;
    g_status.dwWin32ExitCode = win32ExitCode;
    g_status.dwWaitHint = waitHint;
    g_status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;

    if (state == SERVICE_START_PENDING)
        ++g_status.dwCheckPoint;
    else
        g_status.dwCheckPoint = 0;

    if (g_statusHandle)
        SetServiceStatus(g_statusHandle, &g_status);
}

QString configPath()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("NetworkClipboardService.ini"));
}

int runServer()
{
    QSettings settings(configPath(), QSettings::IniFormat);
    const quint16 port = settings.value(QStringLiteral("server/port"), 8787).toUInt();
    settings.setValue(QStringLiteral("server/port"), port);

    QString token = settings.value(QStringLiteral("server/token")).toString();
    if (token.isEmpty()) {
        token = QUuid::createUuid().toString(QUuid::WithoutBraces);
        settings.setValue(QStringLiteral("server/token"), token);
    }

    QString deviceId = settings.value(QStringLiteral("device/id")).toString();
    if (deviceId.isEmpty()) {
        deviceId = QStringLiteral("windows-service-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        settings.setValue(QStringLiteral("device/id"), deviceId);
    }
    settings.setValue(QStringLiteral("device/name"), settings.value(QStringLiteral("device/name"), QHostInfo::localHostName()).toString());
    settings.sync();

    ClipboardStore store;
    ApiServer server(&store);
    QString error;
    if (!server.start(port, token, &error)) {
        qCritical() << "Could not start Network Clipboard service on port" << port << error;
        return 1;
    }

    qInfo() << "Network Clipboard service listening on port" << server.port();
    qInfo() << "Config file:" << configPath();
    return QCoreApplication::exec();
}

void WINAPI serviceControlHandler(DWORD control)
{
    if (control != SERVICE_CONTROL_STOP && control != SERVICE_CONTROL_SHUTDOWN)
        return;

    setServiceStatus(SERVICE_STOP_PENDING, NO_ERROR, 3000);
    if (g_app)
        QMetaObject::invokeMethod(g_app, &QCoreApplication::quit, Qt::QueuedConnection);
}

void WINAPI serviceMain(DWORD argc, LPWSTR *argv)
{
    Q_UNUSED(argc)
    Q_UNUSED(argv)

    g_statusHandle = RegisterServiceCtrlHandlerW(ServiceName, serviceControlHandler);
    if (!g_statusHandle)
        return;

    setServiceStatus(SERVICE_START_PENDING, NO_ERROR, 3000);

    int appArgc = 1;
    char appName[] = "NetworkClipboardService";
    char *appArgv[] = {appName, nullptr};
    QCoreApplication app(appArgc, appArgv);
    g_app = &app;

    setServiceStatus(SERVICE_RUNNING);
    const int result = runServer();
    g_app = nullptr;

    setServiceStatus(SERVICE_STOPPED, result == 0 ? NO_ERROR : ERROR_SERVICE_SPECIFIC_ERROR);
}

int runConsole(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    g_app = &app;
    const int result = runServer();
    g_app = nullptr;
    return result;
}
}

int main(int argc, char *argv[])
{
    for (int i = 1; i < argc; ++i) {
        if (QString::fromLocal8Bit(argv[i]) == QStringLiteral("--console"))
            return runConsole(argc, argv);
    }

    SERVICE_TABLE_ENTRYW serviceTable[] = {
        {const_cast<LPWSTR>(ServiceName), serviceMain},
        {nullptr, nullptr}
    };

    if (!StartServiceCtrlDispatcherW(serviceTable))
        return runConsole(argc, argv);

    return 0;
}
