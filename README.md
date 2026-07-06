# Network Clipboard

Local LAN clipboard for Windows, macOS, Android, and iOS.

## Milestone 1

- Windows tray app.
- Local HTTP API on port `8787`.
- `POST /api/clipboard` stores text or URLs.
- `GET /api/clipboard/latest` returns the latest entry.
- Windows clipboard changes are captured automatically when auto-send is enabled.
- Tray action `Paste from Network` writes the latest network entry into the Windows/macOS clipboard.
- Bearer token authentication.
- 1 MB max text payload.
- Loop prevention when pasting from the network clipboard.

## Projects

- `windows/` - Qt/C++ Windows server and tray client.
- `macos/` - Qt/C++ macOS menu-bar server and clipboard agent.
- `android/` - Qt/QML Android client shell.
- `ios/` - Qt/QML iOS client shell.

## Default API

Base URL on Windows host:

```text
http://<windows-lan-ip>:8787
```

Every request must include:

```text
Authorization: Bearer <token>
```

The Windows app creates a token on first start and stores it via `QSettings`.
