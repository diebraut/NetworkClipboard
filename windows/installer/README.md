# Windows Installer

This folder contains the Inno Setup script for installing the HTTP server as a Windows service.

## Important

The Windows service hosts the LAN HTTP clipboard API. It does not monitor the interactive user's Windows clipboard, because Windows services run in Session 0 and cannot reliably access the user's clipboard. Clipboard monitoring belongs in the tray app, which can later send changes to this service.

## Build

Build the Windows CMake project in Qt Creator. It creates two executables:

- `NetworkClipboardWindows.exe` - tray app.
- `NetworkClipboardService.exe` - Windows service server.

## Prepare deploy folder

Create this folder:

```text
K:\QT-Projekte\NetworkClipboard\windows\deploy
```

Copy both executables from your build output into `windows\deploy`:

- `NetworkClipboardService.exe`
- `NetworkClipboardWindows.exe`

Then run `windeployqt` for both executables, for example:

```powershell
K:\Qt\6.10.1\msvc2022_64\bin\windeployqt.exe K:\QT-Projekte\NetworkClipboard\windows\deploy\NetworkClipboardService.exe
K:\Qt\6.10.1\msvc2022_64\bin\windeployqt.exe K:\QT-Projekte\NetworkClipboard\windows\deploy\NetworkClipboardWindows.exe
```

## Build installer

Open `NetworkClipboardServer.iss` in Inno Setup Compiler and build it.

The installer output is written to:

```text
K:\QT-Projekte\NetworkClipboard\windows\installer-output
```

## Installed service

Service name:

```text
NetworkClipboardServer
```

Default server URL:

```text
http://<windows-lan-ip>:8787
```

The service writes its token and port to:

```text
C:\Program Files\NetworkClipboard\NetworkClipboardService.ini
```

The installer also adds a Windows Firewall rule for TCP port `8787`.

The installer also installs `NetworkClipboardWindows.exe` as the user's startup tray agent. This tray agent watches the interactive Windows clipboard and sends text/URLs to the service.

## Console test

Before building an installer, you can test the service executable manually:

```powershell
NetworkClipboardService.exe --console
```
