package org.qtproject.example.NetworkClipboardAndroid;

import android.content.ClipData;
import android.content.Intent;
import android.os.Bundle;
import android.net.Uri;
import android.provider.MediaStore;

import org.qtproject.qt.android.bindings.QtActivity;

public final class NetworkClipboardActivity extends QtActivity
{
    private static final int CAMERA_CAPTURE_REQUEST_CODE = 46022;
    private static String cameraCaptureResult = "";
    private String pendingCameraUri = "";

    @Override
    public void onCreate(Bundle savedInstanceState)
    {
        super.onCreate(savedInstanceState);
    }

    public void startCameraCapture(String uriText)
    {
        runOnUiThread(() -> {
            Uri uri = Uri.parse(uriText);
            Intent intent = new Intent(MediaStore.ACTION_IMAGE_CAPTURE);
            intent.putExtra(MediaStore.EXTRA_OUTPUT, uri);
            intent.setClipData(ClipData.newRawUri("Network Clipboard Camera", uri));
            intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
            pendingCameraUri = uriText;
            synchronized (NetworkClipboardActivity.class) {
                cameraCaptureResult = "pending";
            }
            startActivityForResult(intent, CAMERA_CAPTURE_REQUEST_CODE);
        });
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data)
    {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != CAMERA_CAPTURE_REQUEST_CODE)
            return;

        synchronized (NetworkClipboardActivity.class) {
            cameraCaptureResult = resultCode == RESULT_OK
                ? "ok:" + pendingCameraUri
                : "cancel";
        }
        pendingCameraUri = "";
    }

    public static synchronized String takeCameraCaptureResult()
    {
        String result = cameraCaptureResult;
        if (!result.equals("pending"))
            cameraCaptureResult = "";
        return result;
    }
}
