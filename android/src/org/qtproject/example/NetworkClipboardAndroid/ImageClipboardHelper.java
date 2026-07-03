package org.qtproject.example.NetworkClipboardAndroid;

import android.content.ClipData;
import android.content.ClipDescription;
import android.content.ClipboardManager;
import android.content.Context;
import android.net.Uri;
import android.os.Build;
import android.util.Base64;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;

public final class ImageClipboardHelper
{
    private static final String TAG = "NCImageClipboard";
    private static final int MAX_IMAGE_BYTES = 10 * 1024 * 1024;

    private ImageClipboardHelper()
    {
    }

    public static boolean hasImage(Context context)
    {
        return clipboardImageUri(context) != null;
    }

    public static String imageKey(Context context)
    {
        Uri uri = clipboardImageUri(context);
        if (uri == null)
            return "";

        String marker = clipboardMarker(context);
        return marker == null || marker.isEmpty() ? uri.toString() : marker;
    }

    public static String imageBase64(Context context)
    {
        byte[] data = clipboardImageBytes(context);
        if (data == null || data.length == 0)
            return "";

        return Base64.encodeToString(data, Base64.NO_WRAP);
    }

    public static boolean isNetworkClipboardImage(Context context)
    {
        Uri uri = clipboardImageUri(context);
        if (uri == null)
            return false;

        String authority = uri.getAuthority();
        return authority != null
            && authority.startsWith("org.qtproject.example.NetworkClipboardAndroid")
            && authority.endsWith(".imageprovider");
    }

    private static byte[] clipboardImageBytes(Context context)
    {
        Uri uri = clipboardImageUri(context);
        if (uri == null)
            return null;

        try (InputStream input = context.getContentResolver().openInputStream(uri);
             ByteArrayOutputStream output = new ByteArrayOutputStream()) {
            if (input == null)
                return null;

            byte[] buffer = new byte[32 * 1024];
            int total = 0;
            int read;
            while ((read = input.read(buffer)) >= 0) {
                total += read;
                if (total > MAX_IMAGE_BYTES)
                    return null;
                output.write(buffer, 0, read);
            }
            return output.toByteArray();
        } catch (Exception exception) {
            Log.w(TAG, "clipboardImageBytes failed uri=" + uri + " error=" + exception);
            return null;
        }
    }

    public static boolean setImageBase64(Context context, String base64)
    {
        try {
            byte[] pngData = Base64.decode(base64, Base64.DEFAULT);
            if (pngData.length == 0 || pngData.length > MAX_IMAGE_BYTES)
                return false;

            File directory = new File(context.getCacheDir(), "network_clipboard");
            if (!directory.exists() && !directory.mkdirs())
                return false;

            File[] oldFiles = directory.listFiles();
            if (oldFiles != null) {
                for (File oldFile : oldFiles)
                    oldFile.delete();
            }

            File imageFile = new File(directory, "clipboard-" + System.currentTimeMillis() + ".png");
            try (FileOutputStream output = new FileOutputStream(imageFile)) {
                output.write(pngData);
            }

            Uri uri = NetworkClipboardImageProvider.uriForFile(context, imageFile);
            ClipboardManager clipboard =
                (ClipboardManager)context.getSystemService(Context.CLIPBOARD_SERVICE);
            if (clipboard == null)
                return false;

            clipboard.setPrimaryClip(
                ClipData.newUri(context.getContentResolver(), "Network Clipboard Image", uri));
            return true;
        } catch (Exception exception) {
            Log.w(TAG, "setImageBase64 failed error=" + exception);
            return false;
        }
    }

    private static Uri clipboardImageUri(Context context)
    {
        try {
            ClipboardManager clipboard =
                (ClipboardManager)context.getSystemService(Context.CLIPBOARD_SERVICE);
            if (clipboard == null || !clipboard.hasPrimaryClip())
                return null;

            ClipData clip = clipboard.getPrimaryClip();
            if (clip == null || clip.getItemCount() == 0)
                return null;

            Uri uri = clip.getItemAt(0).getUri();
            if (uri == null)
                return null;

            String mimeType = context.getContentResolver().getType(uri);
            if (mimeType == null || !mimeType.startsWith("image/"))
                return null;
            return uri;
        } catch (Exception exception) {
            Log.w(TAG, "clipboardImageUri failed error=" + exception);
            return null;
        }
    }

    private static String clipboardMarker(Context context)
    {
        try {
            ClipboardManager clipboard =
                (ClipboardManager)context.getSystemService(Context.CLIPBOARD_SERVICE);
            if (clipboard == null || !clipboard.hasPrimaryClip())
                return "";

            ClipDescription description = clipboard.getPrimaryClipDescription();
            String timestamp = "";
            if (description != null && Build.VERSION.SDK_INT >= 26)
                timestamp = String.valueOf(description.getTimestamp());

            ClipData clip = clipboard.getPrimaryClip();
            Uri uri = clip != null && clip.getItemCount() > 0 ? clip.getItemAt(0).getUri() : null;
            return timestamp + ":" + (uri == null ? "" : uri.toString());
        } catch (Exception ignored) {
            return "";
        }
    }
}