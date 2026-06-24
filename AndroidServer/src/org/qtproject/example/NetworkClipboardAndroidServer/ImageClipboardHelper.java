package org.qtproject.example.NetworkClipboardAndroidServer;

import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.net.Uri;
import android.util.Base64;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;

public final class ImageClipboardHelper
{
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
        return uri == null ? "" : uri.toString();
    }

    public static String imageBase64(Context context)
    {
        Uri uri = clipboardImageUri(context);
        if (uri == null)
            return "";

        try (InputStream input = context.getContentResolver().openInputStream(uri);
             ByteArrayOutputStream output = new ByteArrayOutputStream()) {
            if (input == null)
                return "";

            byte[] buffer = new byte[32 * 1024];
            int total = 0;
            int read;
            while ((read = input.read(buffer)) >= 0) {
                total += read;
                if (total > MAX_IMAGE_BYTES)
                    return "";
                output.write(buffer, 0, read);
            }
            return Base64.encodeToString(output.toByteArray(), Base64.NO_WRAP);
        } catch (Exception ignored) {
            return "";
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
        } catch (Exception ignored) {
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
        } catch (Exception ignored) {
            return null;
        }
    }
}
