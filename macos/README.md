# Network Clipboard Mac Server

macOS menu-bar server for the Network Clipboard LAN API.

## Features

- Local HTTP API on port `8787`.
- UDP discovery on port `8788`.
- Text, URL, and PNG image clipboard entries.
- Automatic publishing of macOS clipboard changes.
- Incoming API entries are written into the macOS clipboard.
- Menu action to copy server URLs and bearer token.
- Optional LaunchAgent for start at login.

## API

- `GET /api/discovery`
- `POST /api/clipboard`
- `GET /api/clipboard/latest`
- `GET /api/clipboard/history`
- `DELETE /api/clipboard/history`

Requests except discovery require:

```http
Authorization: Bearer <token>
```

## Build

```sh
cmake -S macos -B build-macos
cmake --build build-macos
open build-macos/NetworkClipboardMacServer.app
```

The app stores its token and device id with `QSettings` under organization
`LocalTools` and application `NetworkClipboardMacServer`.

Clipboard history is stored atomically in the application's writable data
directory. Up to 100 entries and 32 MB of encoded content are restored when the
server restarts. Clearing the history also clears the persisted file.
