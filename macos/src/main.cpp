#include "MacServerController.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QLockFile>
#include <QProcess>
#include <QStandardPaths>
#include <QThread>

namespace {
void terminateExistingServers()
{
    QProcess pgrep;
    pgrep.start(QStringLiteral("/usr/bin/pgrep"), {QStringLiteral("-x"), QStringLiteral("NetworkClipboardMacServer")});
    if (!pgrep.waitForFinished(1000))
        return;

    const qint64 currentPid = QCoreApplication::applicationPid();
    const QList<QByteArray> lines = pgrep.readAllStandardOutput().split('\n');
    for (const QByteArray &line : lines) {
        bool ok = false;
        const qint64 pid = line.trimmed().toLongLong(&ok);
        if (!ok || pid <= 0 || pid == currentPid)
            continue;

        QProcess::execute(QStringLiteral("/bin/kill"),
                          {QStringLiteral("-TERM"), QString::number(pid)});
    }
    QThread::msleep(300);
}
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setQuitOnLastWindowClosed(false);

    terminateExistingServers();

    QString lockDirectory = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    if (lockDirectory.isEmpty())
        lockDirectory = QDir::tempPath();
    QDir().mkpath(lockDirectory);

    QLockFile appLock(QDir(lockDirectory).filePath(QStringLiteral("NetworkClipboardMacServer.lock")));
    appLock.setStaleLockTime(0);
    appLock.removeStaleLockFile();
    if (!appLock.tryLock(100)) {
        qint64 pid = 0;
        QString hostName;
        QString appName;
        if (appLock.getLockInfo(&pid, &hostName, &appName) && pid > 0) {
            QProcess::execute(QStringLiteral("/bin/kill"),
                              {QStringLiteral("-TERM"), QString::number(pid)});
            for (int attempt = 0; attempt < 15 && !appLock.tryLock(200); ++attempt) {
            }
            if (!appLock.isLocked()) {
                QProcess::execute(QStringLiteral("/bin/kill"),
                                  {QStringLiteral("-KILL"), QString::number(pid)});
                appLock.tryLock(1000);
            }
        }
    }
    if (!appLock.isLocked())
        return 1;

    MacServerController controller;
    controller.show();

    return app.exec();
}
