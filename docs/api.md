# Network Clipboard API

All endpoints require `Authorization: Bearer <token>`.

## POST /api/clipboard

Stores a new clipboard entry. Supported types: `text`, `url`. Content limit: 1 MB.

```json
{
  "deviceId": "android-phone-1",
  "deviceName": "Android Phone",
  "type": "text",
  "content": "Hallo Welt",
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
