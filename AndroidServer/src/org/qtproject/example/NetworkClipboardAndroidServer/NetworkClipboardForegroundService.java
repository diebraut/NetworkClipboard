package org.qtproject.example.NetworkClipboardAndroidServer;

import android.Manifest;
import android.app.Activity;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.IBinder;
import android.os.PowerManager;
import android.provider.Settings;
import android.util.AtomicFile;
import android.util.Base64;
import android.util.Log;

import org.json.JSONArray;
import org.json.JSONObject;
import org.qtproject.qt.android.bindings.QtActivity;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.NetworkInterface;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Enumeration;
import java.util.List;
import java.util.Locale;
import java.util.UUID;

public class NetworkClipboardForegroundService extends Service
{
    private static final String LOG_TAG = "NetworkClipboardServer";
    private static final String CHANNEL_ID = "network_clipboard_server";
    private static final int NOTIFICATION_ID = 8787;
    private static final int API_PORT = 8787;
    private static final int DISCOVERY_PORT = 8788;
    private static final int MAX_TEXT_BYTES = 1024 * 1024;
    private static final int MAX_IMAGE_BYTES = 10 * 1024 * 1024;
    private static final int MAX_REQUEST_BODY_BYTES = 14 * 1024 * 1024;
    private static final int MAX_HISTORY_CONTENT_CHARS = 32 * 1024 * 1024;
    private static final int MAX_HISTORY_ITEMS = 100;
    private static final String DISCOVERY_REQUEST = "NETWORK_CLIPBOARD_DISCOVER_V1";
    private static final String SETTINGS_NAME = "network_clipboard_server";

    private static volatile NetworkClipboardForegroundService instance;

    private final Object entryLock = new Object();
    private final List<JSONObject> history = new ArrayList<>();
    private volatile boolean running;
    private volatile String token = "";
    private volatile String deviceName = "Android Server";
    private volatile boolean isMaster;
    private JSONObject latestEntry;
    private AtomicFile historyFile;
    private ServerSocket serverSocket;
    private DatagramSocket discoverySocket;
    private PowerManager.WakeLock wakeLock;
    private WifiManager.WifiLock wifiLock;
    private WifiManager.MulticastLock multicastLock;

    public static void start(Context context, String token, String deviceName, boolean isMaster)
    {
        if (Build.VERSION.SDK_INT >= 33
            && context instanceof Activity
            && context.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS)
                != PackageManager.PERMISSION_GRANTED) {
            ((Activity)context).requestPermissions(
                new String[]{Manifest.permission.POST_NOTIFICATIONS}, 1002);
        }

        requestBatteryOptimizationExemption(context);

        Intent intent = new Intent(context, NetworkClipboardForegroundService.class);
        intent.putExtra("token", token);
        intent.putExtra("deviceName", deviceName);
        intent.putExtra("isMaster", isMaster);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O)
            context.startForegroundService(intent);
        else
            context.startService(intent);
    }

    public static void publishEntry(Context context, String json)
    {
        NetworkClipboardForegroundService service = instance;
        if (service == null)
            return;
        try {
            service.storeEntry(new JSONObject(json), false);
        } catch (Exception ignored) {
        }
    }

    @Override
    public void onCreate()
    {
        super.onCreate();
        instance = this;
        historyFile = new AtomicFile(new File(getFilesDir(), "NetworkClipboardHistory.json"));
        loadHistory();
        createNotificationChannel();
        startForeground(NOTIFICATION_ID, createNotification());
        acquireWakeLock();
        acquireWifiLock();
        acquireMulticastLock();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId)
    {
        SharedPreferences settings = getSharedPreferences(SETTINGS_NAME, MODE_PRIVATE);
        if (intent != null) {
            String newToken = intent.getStringExtra("token");
            String newDeviceName = intent.getStringExtra("deviceName");
            boolean newIsMaster = intent.getBooleanExtra("isMaster", false);
            if (newToken != null && !newToken.isEmpty()) {
                token = newToken;
                settings.edit().putString("token", token).apply();
            }
            if (newDeviceName != null && !newDeviceName.isEmpty()) {
                deviceName = newDeviceName;
                settings.edit().putString("deviceName", deviceName).apply();
            }
            isMaster = newIsMaster;
            settings.edit().putBoolean("isMaster", isMaster).apply();
        }
        if (token.isEmpty())
            token = settings.getString("token", "");
        if ("Android Server".equals(deviceName))
            deviceName = settings.getString("deviceName", "Android-Server");
        if (intent == null)
            isMaster = settings.getBoolean("isMaster", false);

        if (!running)
            startServers();
        return START_STICKY;
    }

    @Override
    public void onDestroy()
    {
        running = false;
        closeSockets();
        if (wakeLock != null && wakeLock.isHeld())
            wakeLock.release();
        wakeLock = null;
        if (wifiLock != null && wifiLock.isHeld())
            wifiLock.release();
        wifiLock = null;
        if (multicastLock != null && multicastLock.isHeld())
            multicastLock.release();
        multicastLock = null;
        instance = null;
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent)
    {
        return null;
    }

    private void startServers()
    {
        running = true;
        new Thread(this::runHttpServer, "NetworkClipboardHttp").start();
        new Thread(this::runDiscoveryServer, "NetworkClipboardDiscovery").start();
    }

    private void runHttpServer()
    {
        try {
            serverSocket = new ServerSocket(API_PORT);
            serverSocket.setReuseAddress(true);
            while (running) {
                Socket socket = serverSocket.accept();
                new Thread(() -> handleConnection(socket), "NetworkClipboardRequest").start();
            }
        } catch (Exception exception) {
            Log.e(LOG_TAG, "HTTP server stopped", exception);
            running = false;
        }
    }

    private void runDiscoveryServer()
    {
        while (running) {
            try {
                discoverySocket = new DatagramSocket(null);
                discoverySocket.setReuseAddress(true);
                discoverySocket.setBroadcast(true);
                discoverySocket.bind(new java.net.InetSocketAddress(
                    InetAddress.getByName("0.0.0.0"), DISCOVERY_PORT));
                Log.i(LOG_TAG, "UDP discovery listening on port " + DISCOVERY_PORT);

                byte[] buffer = new byte[2048];
                while (running) {
                    DatagramPacket packet = new DatagramPacket(buffer, buffer.length);
                    discoverySocket.receive(packet);
                    String request = new String(packet.getData(), 0, packet.getLength(),
                                                StandardCharsets.UTF_8).trim();
                    if (!DISCOVERY_REQUEST.equals(request))
                        continue;

                    byte[] response = discoveryResponse().toString()
                        .getBytes(StandardCharsets.UTF_8);
                    discoverySocket.send(new DatagramPacket(
                        response, response.length, packet.getAddress(), packet.getPort()));
                    Log.d(LOG_TAG, "Discovery response sent to "
                        + packet.getAddress().getHostAddress() + ":" + packet.getPort());
                }
            } catch (Exception exception) {
                if (running)
                    Log.e(LOG_TAG, "Discovery server stopped; restarting", exception);
            } finally {
                if (discoverySocket != null)
                    discoverySocket.close();
                discoverySocket = null;
            }

            if (running) {
                try {
                    Thread.sleep(1000);
                } catch (InterruptedException ignored) {
                    Thread.currentThread().interrupt();
                }
            }
        }
    }

    private void handleConnection(Socket socket)
    {
        try (Socket connection = socket) {
            connection.setSoTimeout(3000);
            InputStream input = connection.getInputStream();
            ByteArrayOutputStream headerBytes = new ByteArrayOutputStream();
            int state = 0;
            while (headerBytes.size() < 65536) {
                int value = input.read();
                if (value < 0)
                    break;
                headerBytes.write(value);
                state = value == "\r\n\r\n".charAt(state) ? state + 1
                    : (value == '\r' ? 1 : 0);
                if (state == 4)
                    break;
            }

            String headersText = headerBytes.toString(StandardCharsets.ISO_8859_1.name());
            String[] lines = headersText.split("\r\n");
            if (lines.length == 0) {
                sendError(connection, 400, "Invalid HTTP request.");
                return;
            }

            String[] requestLine = lines[0].split(" ");
            if (requestLine.length < 2) {
                sendError(connection, 400, "Invalid HTTP request line.");
                return;
            }

            String method = requestLine[0].toUpperCase(Locale.ROOT);
            String path = requestLine[1].split("\\?", 2)[0];
            String authorization = "";
            int contentLength = 0;
            for (int i = 1; i < lines.length; ++i) {
                int separator = lines[i].indexOf(':');
                if (separator <= 0)
                    continue;
                String name = lines[i].substring(0, separator).trim();
                String value = lines[i].substring(separator + 1).trim();
                if ("Authorization".equalsIgnoreCase(name))
                    authorization = value;
                else if ("Content-Length".equalsIgnoreCase(name))
                    contentLength = Integer.parseInt(value);
            }

            if ("GET".equals(method) && "/api/discovery".equals(path)) {
                sendJson(connection, 200, discoveryResponse());
                return;
            }

            if (!("Bearer " + token).equals(authorization)) {
                sendError(connection, 401, "Missing or invalid bearer token.");
                return;
            }

            if (contentLength > MAX_REQUEST_BODY_BYTES) {
                sendError(connection, 413, "Payload exceeds the request size limit.");
                return;
            }

            byte[] body = readBody(input, contentLength);
            if ("POST".equals(method) && "/api/clipboard".equals(path)) {
                JSONObject entry = new JSONObject(new String(body, StandardCharsets.UTF_8));
                normalizeEntry(entry);
                String validationError = validateEntry(entry);
                if (validationError != null) {
                    sendError(connection, 400, validationError);
                    return;
                }
                storeEntry(entry, true);
                sendJson(connection, 201, entry);
                return;
            }

            if ("GET".equals(method) && "/api/clipboard/latest".equals(path)) {
                synchronized (entryLock) {
                    if (latestEntry == null) {
                        sendError(connection, 404, "Network clipboard is empty.");
                        return;
                    }
                    sendJson(connection, 200, latestEntry);
                }
                return;
            }

            if ("GET".equals(method) && "/api/clipboard/history".equals(path)) {
                JSONArray items = new JSONArray();
                synchronized (entryLock) {
                    for (JSONObject entry : history)
                        items.put(entry);
                }
                sendJson(connection, 200, new JSONObject().put("items", items));
                return;
            }

            if ("DELETE".equals(method) && "/api/clipboard/history".equals(path)) {
                synchronized (entryLock) {
                    history.clear();
                    latestEntry = null;
                    saveHistoryLocked();
                }
                sendJson(connection, 200, new JSONObject().put("ok", true));
                return;
            }

            sendError(connection, 404, "Unknown endpoint.");
        } catch (Exception exception) {
            Log.e(LOG_TAG, "HTTP request failed", exception);
        }
    }

    private byte[] readBody(InputStream input, int contentLength) throws Exception
    {
        byte[] body = new byte[contentLength];
        int offset = 0;
        while (offset < contentLength) {
            int count = input.read(body, offset, contentLength - offset);
            if (count < 0)
                break;
            offset += count;
        }
        return body;
    }

    private String validateEntry(JSONObject entry)
    {
        String type = entry.optString("type", "text");
        String content = entry.optString("content", "");
        if (!"text".equals(type) && !"url".equals(type) && !"image".equals(type))
            return "Unsupported clipboard type.";
        if (content.isEmpty())
            return "Content must not be empty.";

        if ("image".equals(type)) {
            if (!"image/png".equals(entry.optString("mimeType", "")))
                return "Only PNG images are supported.";

            byte[] imageData;
            try {
                imageData = Base64.decode(content, Base64.DEFAULT);
            } catch (Exception exception) {
                return "Image content is not valid Base64.";
            }

            if (imageData.length == 0 || imageData.length > MAX_IMAGE_BYTES)
                return "Image exceeds the 10 MB limit or is invalid.";
            if (imageData.length < 8
                || imageData[0] != (byte)0x89
                || imageData[1] != 0x50
                || imageData[2] != 0x4e
                || imageData[3] != 0x47
                || imageData[4] != 0x0d
                || imageData[5] != 0x0a
                || imageData[6] != 0x1a
                || imageData[7] != 0x0a) {
                return "Image exceeds the 10 MB limit or is invalid.";
            }
        } else if (content.getBytes(StandardCharsets.UTF_8).length > MAX_TEXT_BYTES) {
            return "Content exceeds the 1 MB limit.";
        }

        return null;
    }

    private void normalizeEntry(JSONObject entry) throws Exception
    {
        if (entry.optString("id", "").isEmpty())
            entry.put("id", UUID.randomUUID().toString());
        if (entry.optLong("timestamp", 0) <= 0)
            entry.put("timestamp", System.currentTimeMillis() / 1000L);
        if (entry.optString("type", "").isEmpty())
            entry.put("type", "text");
    }

    private void storeEntry(JSONObject entry, boolean applyToClipboard)
    {
        synchronized (entryLock) {
            if (history.isEmpty() || !samePayload(history.get(0), entry)) {
                history.add(0, entry);
                pruneHistoryLocked();
                latestEntry = history.get(0);
                saveHistoryLocked();
            }
        }

        if (applyToClipboard) {
            String type = entry.optString("type", "text");
            String content = entry.optString("content");
            if ("image".equals(type)) {
                setImageClipboard(content);
            } else if (!content.isEmpty()) {
                ClipboardManager clipboard =
                    (ClipboardManager)getSystemService(Context.CLIPBOARD_SERVICE);
                clipboard.setPrimaryClip(ClipData.newPlainText("Network Clipboard", content));
            }
        }
    }

    private boolean samePayload(JSONObject left, JSONObject right)
    {
        return left.optString("type", "text").equals(right.optString("type", "text"))
            && left.optString("mimeType", "").equals(right.optString("mimeType", ""))
            && left.optString("content", "").equals(right.optString("content", ""));
    }

    private void pruneHistoryLocked()
    {
        long storedContentSize = 0;
        int retainedItems = 0;
        for (JSONObject item : history) {
            storedContentSize += item.optString("content", "").length();
            ++retainedItems;
            if (retainedItems >= MAX_HISTORY_ITEMS
                || storedContentSize > MAX_HISTORY_CONTENT_CHARS) {
                break;
            }
        }
        while (history.size() > retainedItems)
            history.remove(history.size() - 1);
    }

    private void loadHistory()
    {
        synchronized (entryLock) {
            history.clear();
            latestEntry = null;
            try (FileInputStream input = historyFile.openRead();
                 ByteArrayOutputStream bytes = new ByteArrayOutputStream()) {
                byte[] buffer = new byte[8192];
                int count;
                while ((count = input.read(buffer)) >= 0)
                    bytes.write(buffer, 0, count);

                JSONObject document = new JSONObject(
                    new String(bytes.toByteArray(), StandardCharsets.UTF_8));
                JSONArray items = document.optJSONArray("items");
                if (items == null)
                    return;

                for (int index = 0; index < items.length(); ++index) {
                    JSONObject entry = items.optJSONObject(index);
                    if (entry != null) {
                        normalizeEntry(entry);
                        if (validateEntry(entry) == null)
                            history.add(entry);
                    }
                }
                pruneHistoryLocked();
                if (!history.isEmpty())
                    latestEntry = history.get(0);
            } catch (Exception exception) {
                Log.i(LOG_TAG, "No valid persisted clipboard history available", exception);
            }
        }
    }

    private void saveHistoryLocked()
    {
        JSONArray items = new JSONArray();
        for (JSONObject entry : history)
            items.put(entry);

        FileOutputStream output = null;
        try {
            output = historyFile.startWrite();
            output.write(new JSONObject().put("items", items).toString()
                .getBytes(StandardCharsets.UTF_8));
            historyFile.finishWrite(output);
        } catch (Exception exception) {
            if (output != null)
                historyFile.failWrite(output);
            Log.e(LOG_TAG, "Could not persist clipboard history", exception);
        }
    }

    private boolean setImageClipboard(String base64)
    {
        boolean ok = ImageClipboardHelper.setImageBase64(this, base64);
        if (!ok)
            Log.w(LOG_TAG, "Could not put image into Android clipboard");
        return ok;
    }

    private JSONObject discoveryResponse() throws Exception
    {
        JSONArray urls = new JSONArray();
        for (String url : serverUrls())
            urls.put(url);

        return new JSONObject()
            .put("service", "NetworkClipboard")
            .put("protocolVersion", 1)
            .put("platform", "android")
            .put("serverName", deviceName)
            .put("url", urls.length() > 0 ? urls.getString(0) : "http://127.0.0.1:8787")
            .put("urls", urls)
            .put("token", token)
            .put("isMaster", isMaster)
            .put("agentActive", true)
            .put("maxImageBytes", MAX_IMAGE_BYTES)
            .put("supportedTypes", new JSONArray()
                .put("text")
                .put("url")
                .put("image"));
    }

    private List<String> serverUrls()
    {
        List<String> urls = new ArrayList<>();
        try {
            Enumeration<NetworkInterface> interfaces = NetworkInterface.getNetworkInterfaces();
            for (NetworkInterface networkInterface : Collections.list(interfaces)) {
                if (!networkInterface.isUp() || networkInterface.isLoopback())
                    continue;
                for (InetAddress address : Collections.list(networkInterface.getInetAddresses())) {
                    if (address instanceof Inet4Address && !address.isLoopbackAddress())
                        urls.add("http://" + address.getHostAddress() + ":8787");
                }
            }
        } catch (Exception ignored) {
        }
        urls.add("http://127.0.0.1:8787");
        return urls;
    }

    private void sendJson(Socket socket, int statusCode, JSONObject body) throws Exception
    {
        byte[] bytes = body.toString().getBytes(StandardCharsets.UTF_8);
        String reason = statusCode == 200 ? "OK"
            : statusCode == 201 ? "Created"
            : statusCode == 400 ? "Bad Request"
            : statusCode == 401 ? "Unauthorized"
            : statusCode == 404 ? "Not Found"
            : statusCode == 413 ? "Payload Too Large"
            : "Internal Server Error";
        String headers = "HTTP/1.1 " + statusCode + " " + reason + "\r\n"
            + "Content-Type: application/json; charset=utf-8\r\n"
            + "Content-Length: " + bytes.length + "\r\n"
            + "Connection: close\r\n"
            + "Access-Control-Allow-Origin: *\r\n\r\n";
        OutputStream output = socket.getOutputStream();
        output.write(headers.getBytes(StandardCharsets.ISO_8859_1));
        output.write(bytes);
        output.flush();
    }

    private void sendError(Socket socket, int statusCode, String message) throws Exception
    {
        sendJson(socket, statusCode, new JSONObject().put("error", message));
    }

    private void closeSockets()
    {
        try {
            if (serverSocket != null)
                serverSocket.close();
        } catch (Exception ignored) {
        }
        if (discoverySocket != null)
            discoverySocket.close();
    }

    private void createNotificationChannel()
    {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O)
            return;
        NotificationChannel channel = new NotificationChannel(
            CHANNEL_ID, "Network Clipboard Server", NotificationManager.IMPORTANCE_LOW);
        channel.setDescription("Haelt den lokalen Clipboard-Server aktiv.");
        channel.setShowBadge(false);
        ((NotificationManager)getSystemService(Context.NOTIFICATION_SERVICE))
            .createNotificationChannel(channel);
    }

    private Notification createNotification()
    {
        Intent activityIntent = new Intent(this, QtActivity.class);
        activityIntent.setFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP | Intent.FLAG_ACTIVITY_CLEAR_TOP);
        int flags = PendingIntent.FLAG_UPDATE_CURRENT;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M)
            flags |= PendingIntent.FLAG_IMMUTABLE;
        PendingIntent pendingIntent = PendingIntent.getActivity(this, 0, activityIntent, flags);
        Notification.Builder builder = Build.VERSION.SDK_INT >= Build.VERSION_CODES.O
            ? new Notification.Builder(this, CHANNEL_ID)
            : new Notification.Builder(this);
        int iconId = getResources().getIdentifier("ic_launcher", "mipmap", getPackageName());
        if (iconId == 0)
            iconId = android.R.drawable.stat_notify_sync;
        return builder.setSmallIcon(iconId)
            .setContentTitle("Network Clipboard Server")
            .setContentText("Clipboard-Server aktiv auf Port 8787")
            .setContentIntent(pendingIntent)
            .setOngoing(true)
            .setCategory(Notification.CATEGORY_SERVICE)
            .build();
    }

    private void acquireWakeLock()
    {
        PowerManager manager = (PowerManager)getSystemService(Context.POWER_SERVICE);
        wakeLock = manager.newWakeLock(
            PowerManager.PARTIAL_WAKE_LOCK, getPackageName() + ":ClipboardServer");
        wakeLock.setReferenceCounted(false);
        wakeLock.acquire();
    }

    private void acquireWifiLock()
    {
        WifiManager manager =
            (WifiManager)getApplicationContext().getSystemService(Context.WIFI_SERVICE);
        if (manager == null)
            return;
        wifiLock = manager.createWifiLock(
            WifiManager.WIFI_MODE_FULL_HIGH_PERF,
            getPackageName() + ":ClipboardServerWifi");
        wifiLock.setReferenceCounted(false);
        wifiLock.acquire();
    }

    private void acquireMulticastLock()
    {
        WifiManager manager =
            (WifiManager)getApplicationContext().getSystemService(Context.WIFI_SERVICE);
        if (manager == null)
            return;
        multicastLock = manager.createMulticastLock(
            getPackageName() + ":ClipboardServerDiscovery");
        multicastLock.setReferenceCounted(false);
        multicastLock.acquire();
    }

    private static void requestBatteryOptimizationExemption(Context context)
    {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M || !(context instanceof Activity))
            return;

        PowerManager manager = (PowerManager)context.getSystemService(Context.POWER_SERVICE);
        if (manager.isIgnoringBatteryOptimizations(context.getPackageName()))
            return;

        try {
            Intent intent = new Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS);
            intent.setData(Uri.parse("package:" + context.getPackageName()));
            context.startActivity(intent);
        } catch (Exception exception) {
            Log.w(LOG_TAG, "Battery optimization settings could not be opened", exception);
        }
    }
}
