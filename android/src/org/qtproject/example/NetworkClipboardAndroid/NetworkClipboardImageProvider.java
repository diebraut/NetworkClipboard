package org.qtproject.example.NetworkClipboardAndroid;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileNotFoundException;

public final class NetworkClipboardImageProvider extends ContentProvider
{
    private static final String FILE_NAME = "image.png";

    public static Uri uriForFile(Context context, File file)
    {
        return new Uri.Builder()
            .scheme("content")
            .authority(context.getPackageName() + ".imageprovider")
            .appendPath(FILE_NAME)
            .appendQueryParameter("name", file.getName())
            .build();
    }

    @Override
    public boolean onCreate()
    {
        return true;
    }

    @Override
    public String getType(Uri uri)
    {
        return "image/png";
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException
    {
        if (!"r".equals(mode))
            throw new FileNotFoundException("Read-only provider");

        File file = resolveFile(uri);
        if (!file.isFile())
            throw new FileNotFoundException("Clipboard image not found");
        return ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override
    public Cursor query(Uri uri,
                        String[] projection,
                        String selection,
                        String[] selectionArgs,
                        String sortOrder)
    {
        File file = resolveFile(uri);
        MatrixCursor cursor = new MatrixCursor(
            projection != null
                ? projection
                : new String[]{OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE});
        MatrixCursor.RowBuilder row = cursor.newRow();
        String[] columns = cursor.getColumnNames();
        for (String column : columns) {
            if (OpenableColumns.DISPLAY_NAME.equals(column))
                row.add(file.getName());
            else if (OpenableColumns.SIZE.equals(column))
                row.add(file.length());
            else
                row.add(null);
        }
        return cursor;
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs)
    {
        return 0;
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs)
    {
        return 0;
    }

    @Override
    public Uri insert(Uri uri, ContentValues values)
    {
        return null;
    }

    private File resolveFile(Uri uri)
    {
        String name = uri.getQueryParameter("name");
        File directory = new File(getContext().getCacheDir(), "network_clipboard");
        File file = new File(directory, name == null ? "" : name);
        try {
            String directoryPath = directory.getCanonicalPath() + File.separator;
            String filePath = file.getCanonicalPath();
            if (!filePath.startsWith(directoryPath))
                return new File(directory, "__invalid__");
        } catch (Exception ignored) {
            return new File(directory, "__invalid__");
        }
        return file;
    }
}
