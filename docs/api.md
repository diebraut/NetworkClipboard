# Network Clipboard API

All endpoints require `Authorization: Bearer <token>`.

## POST /api/clipboard

Stores a new clipboard entry. Supported types: `text`, `url`, `image`.

- Text and URL content is limited to 1 MB.
- Images use `type: "image"`, `mimeType: "image/png"` and Base64-encoded PNG data in `content`.
- Decoded PNG data is limited to 10 MB.

```json
{
  "deviceId": "android-phone-1",
  "deviceName": "Android Phone",
  "type": "text",
  "content": "Hallo Welt",
  "timestamp": 1234567890
}
```

Image example:

```json
{
  "deviceId": "windows-pc-1",
  "deviceName": "Windows PC",
  "type": "image",
  "mimeType": "image/png",
  "content": "<base64-png-data>",
  "timestamp": 1234567890
}
```

Response: `201 Created` with the stored entry.

## GET /api/clipboard/latest

Returns the latest entry or `404 Not Found` if empty.

## GET /api/clipboard/history

Returns `{ "items": [] }`.

## DELETE /api/clipboard/history

Clears history and latest entry. Returns `{ "ok": true }`.
