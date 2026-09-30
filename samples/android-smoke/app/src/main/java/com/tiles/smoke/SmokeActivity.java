package com.tiles.smoke;

import android.app.Activity;
import android.content.res.AssetManager;
import android.graphics.Color;
import android.os.Bundle;
import android.view.Gravity;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.Window;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.TextView;

/**
 * Minimal on-device smoke harness (ADR-0023 G1).
 *
 * Full-screen SurfaceView for the SDK renderer + a status TextView overlay.
 * The user just opens the app and waits ~30s: native code loads the bundled
 * p3 fixture, renders until the tile pipeline settles, then shows
 * PASS/FAIL with the key numbers on screen (screenshot-friendly).
 */
public class SmokeActivity extends Activity {
    static {
        System.loadLibrary("smoke_jni");
    }

    private TextView statusView;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        requestWindowFeature(Window.FEATURE_NO_TITLE);
        getWindow().setFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN,
                WindowManager.LayoutParams.FLAG_FULLSCREEN);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        FrameLayout root = new FrameLayout(this);

        SurfaceView surfaceView = new SurfaceView(this);
        root.addView(surfaceView, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT));

        statusView = new TextView(this);
        statusView.setTextSize(22);
        statusView.setTextColor(Color.WHITE);
        statusView.setBackgroundColor(0xB0000000);
        statusView.setPadding(24, 24, 24, 24);
        statusView.setText("starting…");
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.WRAP_CONTENT);
        lp.gravity = Gravity.TOP;
        root.addView(statusView, lp);

        setContentView(root);

        surfaceView.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override
            public void surfaceCreated(SurfaceHolder holder) {
                nativeStart(holder.getSurface(), getAssets(),
                        getFilesDir().getAbsolutePath());
            }

            @Override
            public void surfaceChanged(SurfaceHolder holder, int format,
                                       int width, int height) {
                // Minimal harness: fixed surface size, no resize handling.
            }

            @Override
            public void surfaceDestroyed(SurfaceHolder holder) {
                nativeStop();
            }
        });
    }

    @Override
    protected void onDestroy() {
        nativeStop();
        super.onDestroy();
    }

    /** Called from native code (any thread) with the current status text. */
    @SuppressWarnings("unused")
    private void onStatus(final String text) {
        runOnUiThread(() -> statusView.setText(text));
    }

    private native void nativeStart(Surface surface, AssetManager assets,
                                    String filesDir);

    private native void nativeStop();
}
