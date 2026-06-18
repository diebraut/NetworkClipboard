# Network Clipboard Android Server

Android-side clipboard server with the same LAN HTTP API as the Windows server.

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

## Notes

Default port: Android builds use `8787`, desktop test builds use `8789` so they
can run beside the Windows server.

The Android build starts a native foreground service with a persistent
notification and a partial wake lock. It keeps the Qt process and its HTTP
server active while another app is in the foreground.

Android can still restrict reading clipboard contents while this app is in the
background. HTTP access and clipboard entries received through the API remain
available.
