package org.qtproject.example.NetworkClipboardAndroid;

import android.app.Activity;
import android.content.ClipData;
import android.content.ClipDescription;
import android.content.ClipboardManager;
import android.content.ContentUris;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.net.Uri;
import android.os.Build;
import android.provider.MediaStore;
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
    private static final int MAX_PHOTO_BYTES = 500 * 1024;

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

    public static String recentPhotosJson(Context context, int maxCount)
    {
        if (!hasPhotoPermission(context)) {
            requestPhotoPermission(context);
            return "[]";
        }

        int boundedMaxCount = Math.max(1, Math.min(maxCount, 80));
        StringBuilder builder = new StringBuilder();
        builder.append('[');

        String[] projection = new String[] {
            MediaStore.Images.Media._ID,
            MediaStore.Images.Media.DATE_TAKEN,
            MediaStore.Images.Media.DATE_ADDED
        };
        String sortOrder = MediaStore.Images.Media.DATE_ADDED + " DESC";

        int added = 0;
        try (Cursor cursor = context.getContentResolver().query(
                MediaStore.Images.Media.EXTERNAL_CONTENT_URI,
                projection,
                null,
                null,
                sortOrder)) {
            if (cursor == null)
                return "[]";

            int idColumn = cursor.getColumnIndexOrThrow(MediaStore.Images.Media._ID);
            int takenColumn = cursor.getColumnIndexOrThrow(MediaStore.Images.Media.DATE_TAKEN);
            int addedColumn = cursor.getColumnIndexOrThrow(MediaStore.Images.Media.DATE_ADDED);

            while (cursor.moveToNext() && added < boundedMaxCount) {
                long id = cursor.getLong(idColumn);
                Uri uri = ContentUris.withAppendedId(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, id);
                Bitmap thumbnail = loadBitmap(context, uri, 220);
                String thumbnailBase64 = pngBase64(thumbnail, 0);
                if (thumbnail != null)
                    thumbnail.recycle();
                if (thumbnailBase64.isEmpty())
                    continue;

                long takenMs = cursor.getLong(takenColumn);
                if (takenMs <= 0)
                    takenMs = cursor.getLong(addedColumn) * 1000L;

                if (added > 0)
                    builder.append(',');
                builder.append('{')
                    .append("\"id\":\"").append(jsonEscape(uri.toString())).append("\",")
                    .append("\"thumbnail\":\"").append(thumbnailBase64).append("\",")
                    .append("\"createdAtMs\":").append(takenMs)
                    .append('}');
                ++added;
            }
        } catch (Exception exception) {
            Log.w(TAG, "recentPhotosJson failed error=" + exception);
            return "[]";
        }

        builder.append(']');
        return builder.toString();
    }

    public static String photoContentBase64(Context context, String uriText)
    {
        if (!hasPhotoPermission(context) || uriText == null || uriText.isEmpty())
            return "";

        try {
            Bitmap bitmap = loadBitmap(context, Uri.parse(uriText), 4096);
            String base64 = pngBase64(bitmap, MAX_PHOTO_BYTES);
            if (bitmap != null)
                bitmap.recycle();
            return base64;
        } catch (Exception exception) {
            Log.w(TAG, "photoContentBase64 failed uri=" + uriText + " error=" + exception);
            return "";
        }
    }

    public static boolean hasCameraApp(Context context)
    {
        if (context == null)
            return false;

        Intent intent = new Intent(MediaStore.ACTION_IMAGE_CAPTURE);
        return intent.resolveActivity(context.getPackageManager()) != null;
    }

    public static String createCameraCaptureUri(Context context)
    {
        if (context == null)
            return "";

        try {
            File directory = new File(context.getCacheDir(), "network_clipboard");
            if (!directory.exists() && !directory.mkdirs())
                return "";

            File[] oldFiles = directory.listFiles();
            if (oldFiles != null) {
                for (File oldFile : oldFiles) {
                    String name = oldFile.getName();
                    if (name != null && name.startsWith("camera-"))
                        oldFile.delete();
                }
            }

            File imageFile = new File(directory, "camera-" + System.currentTimeMillis() + ".jpg");
            return NetworkClipboardImageProvider.uriForFile(context, imageFile).toString();
        } catch (Exception exception) {
            Log.w(TAG, "createCameraCaptureUri failed error=" + exception);
            return "";
        }
    }

    public static String cameraContentBase64(Context context, String uriText)
    {
        if (context == null || uriText == null || uriText.isEmpty())
            return "";

        try {
            Bitmap bitmap = loadBitmap(context, Uri.parse(uriText), 4096);
            String base64 = pngBase64(bitmap, MAX_PHOTO_BYTES);
            if (bitmap != null)
                bitmap.recycle();
            return base64;
        } catch (Exception exception) {
            Log.w(TAG, "cameraContentBase64 failed uri=" + uriText + " error=" + exception);
            return "";
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

    private static boolean hasPhotoPermission(Context context)
    {
        if (Build.VERSION.SDK_INT >= 33) {
            return context.checkSelfPermission(android.Manifest.permission.READ_MEDIA_IMAGES)
                == PackageManager.PERMISSION_GRANTED;
        }
        return Build.VERSION.SDK_INT < 23
            || context.checkSelfPermission(android.Manifest.permission.READ_EXTERNAL_STORAGE)
                == PackageManager.PERMISSION_GRANTED;
    }

    private static void requestPhotoPermission(Context context)
    {
        if (Build.VERSION.SDK_INT < 23)
            return;

        Activity activity = activityForContext(context);
        if (activity == null)
            return;

        String permission = Build.VERSION.SDK_INT >= 33
            ? android.Manifest.permission.READ_MEDIA_IMAGES
            : android.Manifest.permission.READ_EXTERNAL_STORAGE;
        activity.requestPermissions(new String[] { permission }, 46021);
    }

    private static Activity activityForContext(Context context)
    {
        if (context instanceof Activity)
            return (Activity)context;

        try {
            Class<?> qtNative = Class.forName("org.qtproject.qt.android.QtNative");
            Object activity = qtNative.getMethod("activity").invoke(null);
            if (activity instanceof Activity)
                return (Activity)activity;
        } catch (Exception exception) {
            Log.w(TAG, "activityForContext failed error=" + exception);
        }
        return null;
    }

    private static Bitmap loadBitmap(Context context, Uri uri, int maxSize)
    {
        try {
            BitmapFactory.Options bounds = new BitmapFactory.Options();
            bounds.inJustDecodeBounds = true;
            try (InputStream input = context.getContentResolver().openInputStream(uri)) {
                BitmapFactory.decodeStream(input, null, bounds);
            }
            if (bounds.outWidth <= 0 || bounds.outHeight <= 0)
                return null;

            BitmapFactory.Options options = new BitmapFactory.Options();
            options.inSampleSize = sampleSize(bounds.outWidth, bounds.outHeight, maxSize);
            options.inPreferredConfig = Bitmap.Config.ARGB_8888;
            try (InputStream input = context.getContentResolver().openInputStream(uri)) {
                return BitmapFactory.decodeStream(input, null, options);
            }
        } catch (Exception exception) {
            Log.w(TAG, "loadBitmap failed uri=" + uri + " error=" + exception);
            return null;
        }
    }

    private static int sampleSize(int width, int height, int maxSize)
    {
        int sample = 1;
        while (width / sample > maxSize * 2 || height / sample > maxSize * 2)
            sample *= 2;
        return sample;
    }

    private static String pngBase64(Bitmap source, int maxBytes)
    {
        if (source == null || source.getWidth() <= 0 || source.getHeight() <= 0)
            return "";

        Bitmap bitmap = source;
        for (int attempt = 0; attempt < 20; ++attempt) {
            ByteArrayOutputStream output = new ByteArrayOutputStream();
            if (!bitmap.compress(Bitmap.CompressFormat.PNG, 100, output))
                return "";
            byte[] data = output.toByteArray();
            if (maxBytes <= 0 || data.length <= maxBytes)
                return Base64.encodeToString(data, Base64.NO_WRAP);

            int nextWidth = Math.max(1, Math.round(bitmap.getWidth() * 0.8f));
            int nextHeight = Math.max(1, Math.round(bitmap.getHeight() * 0.8f));
            if (nextWidth == bitmap.getWidth() && nextHeight == bitmap.getHeight())
                break;
            Bitmap scaled = Bitmap.createScaledBitmap(bitmap, nextWidth, nextHeight, true);
            if (bitmap != source)
                bitmap.recycle();
            bitmap = scaled;
        }

        if (bitmap != source)
            bitmap.recycle();
        return "";
    }

    private static String jsonEscape(String value)
    {
        if (value == null)
            return "";

        return value.replace("\\", "\\\\").replace("\"", "\\\"");
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
