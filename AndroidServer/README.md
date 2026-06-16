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

This first milestone runs the Qt Android application process as the server host.
Android restricts clipboard access for background processes on modern versions,
so a later milestone should add a native Android foreground service with a
persistent notification if always-on background behavior is required.
