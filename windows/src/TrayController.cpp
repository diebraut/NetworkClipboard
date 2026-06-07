#include "TrayController.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QMenu>
#include <QStyle>

TrayController::TrayController(ClipboardStore *store, const QString &deviceId, const QString &deviceName, QObject *parent)
    : QObject(parent), m_store(store), m_clipboard(QApplication::clipboard()), m_deviceId(deviceId), m_deviceName(deviceName)
{
    m_menu = new QMenu();

    QAction *pasteAction = m_menu->addAction(QStringLiteral("Paste from Network"));
    connect(pasteAction, &QAction::triggered, this, &TrayController::pasteFromNetwork);

    QAction *copyInfoAction = m_menu->addAction(QStringLiteral("Copy Server Info"));
    connect(copyInfoAction, &QAction::triggered, this, &TrayController::copyServerInfo);

    m_menu->addSeparator();
    m_autoSendAction = m_menu->addAction(QStringLiteral("Auto-send Windows Clipboard"));
    m_autoSendAction->setCheckable(true);
    m_autoSendAction->setChecked(m_autoSendEnabled);
    connect(m_autoSendAction, &QAction::toggled, this, &TrayController::setAutoSendEnabled);

    m_menu->addSeparator();
    QAction *quitAction = m_menu->addAction(QStringLiteral("Quit"));
    connect(quitAction, &QAction::triggered, qApp, &QApplication::quit);

    m_tray.setContextMenu(m_menu);
    m_tray.setToolTip(QStringLiteral("Network Clipboard"));
    m_tray.setIcon(QApplication::style()->standardIcon(QStyle::SP_DriveNetIcon));

    connect(m_clipboard, &QClipboard::dataChanged, this, &TrayController::onClipboardChanged);
}

void TrayController::show()
{
    m_tray.show();
}

void TrayController::setServerInfo(quint16 port, const QString &token)
{
    m_port = port;
    m_token = token;
}

void TrayController::onClipboardChanged()
{
    if (m_suppressNextClipboardChange) {
        m_suppressNextClipboardChange = false;
        return;
    }

    if (!m_autoSendEnabled)
        return;

    const QString text = m_clipboard->text().trimmed();
    if (text.isEmpty() || text == m_lastPublishedContent)
        return;

    ClipboardEntry entry;
    entry.deviceId = m_deviceId;
    entry.deviceName = m_deviceName;
    entry.type = text.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive) || text.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive)
        ? QStringLiteral("url")
        : QStringLiteral("text");
    entry.content = text;
    entry.timestamp = QDateTime::currentSecsSinceEpoch();

    QString error;
    if (!entry.isValid(&error))
        return;

    m_lastPublishedContent = text;
    m_store->setLatest(entry);
}

void TrayController::pasteFromNetwork()
{
    const auto latest = m_store->latest();
    if (!latest) {
        m_tray.showMessage(QStringLiteral("Network Clipboard"), QStringLiteral("No network clipboard entry available."));
        return;
    }

    m_suppressNextClipboardChange = true;
    m_lastPublishedContent = latest->content;
    m_clipboard->setText(latest->content);
    m_tray.showMessage(QStringLiteral("Network Clipboard"), QStringLiteral("Copied latest network entry to Windows clipboard."));
}

void TrayController::copyServerInfo()
{
    const QString info = QStringLiteral("URL: http://<windows-lan-ip>:%1\nAuthorization: Bearer %2").arg(m_port).arg(m_token);
    m_suppressNextClipboardChange = true;
    m_clipboard->setText(info);
    m_tray.showMessage(QStringLiteral("Network Clipboard"), QStringLiteral("Server info copied to clipboard."));
}

void TrayController::setAutoSendEnabled(bool enabled)
{
    m_autoSendEnabled = enabled;
}
