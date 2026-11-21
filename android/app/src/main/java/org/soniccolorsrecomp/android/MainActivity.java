package org.soniccolorsrecomp.android;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.graphics.Color;
import android.hardware.input.InputManager;
import android.net.Uri;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.util.Locale;
import java.io.IOException;
import java.io.InputStream;

/** Launcher prototype. It intentionally does not pretend a native game backend exists. */
public class MainActivity extends Activity {
    private static final int PICK_DISC = 1;
    private SharedPreferences prefs;
    private TextView selected;
    private Uri disc;
    private final AndroidInput controller = new AndroidInput();
    private InputManager inputManager;
    private final InputManager.InputDeviceListener deviceListener = new InputManager.InputDeviceListener() {
        @Override public void onInputDeviceAdded(int id) { }
        @Override public void onInputDeviceChanged(int id) { }
        @Override public void onInputDeviceRemoved(int id) { controller.deviceRemoved(id); }
    };

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        inputManager = (InputManager) getSystemService(INPUT_SERVICE);
        if (inputManager != null) inputManager.registerInputDeviceListener(deviceListener, null);
        prefs = getSharedPreferences("launcher", MODE_PRIVATE);
        String uri = prefs.getString("disc", null);
        if (uri != null) disc = Uri.parse(uri);
        showLauncher();
    }

    @Override public boolean dispatchKeyEvent(KeyEvent event) {
        controller.key(event);
        return super.dispatchKeyEvent(event);
    }
    @Override public boolean dispatchGenericMotionEvent(MotionEvent event) {
        controller.motion(event);
        return super.dispatchGenericMotionEvent(event);
    }
    @Override protected void onPause() {
        controller.clear();
        super.onPause();
    }
    @Override protected void onDestroy() {
        if (inputManager != null) inputManager.unregisterInputDeviceListener(deviceListener);
        controller.clear();
        super.onDestroy();
    }

    private TextView text(String value, int size) {
        TextView view = new TextView(this);
        view.setText(value); view.setTextSize(size); view.setTextColor(Color.WHITE);
        view.setPadding(0, 12, 0, 12);
        return view;
    }

    private Button button(String label, Runnable action) {
        Button b = new Button(this); b.setText(label); b.setOnClickListener(v -> action.run()); return b;
    }

    private void showLauncher() {
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL); page.setPadding(32, 32, 32, 32);
        page.setBackgroundColor(Color.rgb(12, 24, 52));
        page.addView(text("SONIC COLORS\nRECOMP · ANDROID PREVIEW", 24));
        page.addView(text("This is a launcher prototype, not a playable game. The Android runtime and disc installation are not implemented yet.", 16));
        if (BuildConfig.RECOMP_INCLUDED) {
            try {
                page.addView(text("Recompiled code included: " + RecompCode.gameId() + " (runtime unavailable)", 14));
            } catch (LinkageError e) {
                page.addView(text("Recompiled code could not be loaded on this device.", 14));
            }
        }
        selected = text(disc == null ? "No disc selected" : "Selected: " + displayName(disc), 16);
        page.addView(selected);
        page.addView(button("Choose Wii ISO or ZIP", () -> {
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType("*/*");
            intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
            startActivityForResult(intent, PICK_DISC);
        }));
        Button play = button("Play (runtime unavailable)", () -> {});
        play.setEnabled(false); page.addView(play);
        page.addView(button("Arrange touch controls", this::showControls));
        page.addView(text("Control layout is a preview only. Touch and controller input are not connected to the game.", 14));
        setContentView(page);
    }

    private String displayName(Uri uri) {
        try (Cursor c = getContentResolver().query(uri, new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst() && !c.isNull(0)) return c.getString(0);
        } catch (SecurityException ignored) { return "Permission lost — select again"; }
        return uri.getLastPathSegment() == null ? "Unknown file" : uri.getLastPathSegment();
    }

    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != PICK_DISC || result != RESULT_OK || data == null || data.getData() == null) return;
        Uri chosen = data.getData();
        String name = displayName(chosen).toLowerCase(Locale.ROOT);
        if (!name.endsWith(".iso") && !name.endsWith(".zip")) {
            selected.setText("Choose an ISO or ZIP file."); return;
        }
        try {
            getContentResolver().takePersistableUriPermission(chosen, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException e) {
            selected.setText("Cannot retain access to this file; choose another provider."); return;
        }
        Uri old = disc;
        disc = chosen;
        prefs.edit().putString("disc", chosen.toString()).apply();
        if (old != null && !old.equals(chosen)) {
            try { getContentResolver().releasePersistableUriPermission(old, Intent.FLAG_GRANT_READ_URI_PERMISSION); }
            catch (SecurityException ignored) { }
        }
        selected.setText("Checking disc header…");
        final String filename = displayName(chosen);
        new Thread(() -> {
            String message;
            try (InputStream in = getContentResolver().openInputStream(chosen)) {
                if (in == null) throw new IOException("Cannot open file");
                DiscInspector.check(in, filename);
                message = "Disc identified: " + filename + " (not installed)";
            } catch (IOException | SecurityException e) {
                message = "Disc check failed: " + e.getMessage();
            }
            final String resultMessage = message;
            runOnUiThread(() -> {
                if (!isFinishing() && !isDestroyed() && chosen.equals(disc) && selected != null)
                    selected.setText(resultMessage);
            });
        }, "disc-check").start();
    }

    private void showControls() {
        FrameLayout canvas = new FrameLayout(this);
        canvas.setBackgroundColor(Color.rgb(12, 24, 52));
        TextView hint = text("Drag controls to arrange them · Preview only", 16);
        FrameLayout.LayoutParams hp = new FrameLayout.LayoutParams(-2, -2, Gravity.TOP | Gravity.CENTER_HORIZONTAL);
        canvas.addView(hint, hp);
        Button back = button("Done", this::showLauncher);
        FrameLayout.LayoutParams bp = new FrameLayout.LayoutParams(-2, -2, Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL);
        canvas.addView(back, bp);
        addControl(canvas, "Stick", .12f, .58f);
        addControl(canvas, "A", .78f, .56f);
        addControl(canvas, "B", .64f, .68f);
        addControl(canvas, "Pause", .48f, .16f);
        setContentView(canvas);
    }

    private void addControl(FrameLayout canvas, String name, float defaultX, float defaultY) {
        Button control = new Button(this); control.setText(name);
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(dp(82), dp(64));
        canvas.addView(control, lp);
        canvas.post(() -> {
            control.setX(prefs.getFloat(name + "X", defaultX) * Math.max(0, canvas.getWidth() - control.getWidth()));
            control.setY(prefs.getFloat(name + "Y", defaultY) * Math.max(0, canvas.getHeight() - control.getHeight()));
        });
        control.setOnTouchListener(new View.OnTouchListener() {
            float dx, dy;
            @Override public boolean onTouch(View v, MotionEvent e) {
                switch (e.getActionMasked()) {
                    case MotionEvent.ACTION_DOWN: dx = e.getRawX() - v.getX(); dy = e.getRawY() - v.getY(); return true;
                    case MotionEvent.ACTION_MOVE:
                        v.setX(Math.max(0, Math.min(canvas.getWidth() - v.getWidth(), e.getRawX() - dx)));
                        v.setY(Math.max(0, Math.min(canvas.getHeight() - v.getHeight(), e.getRawY() - dy))); return true;
                    case MotionEvent.ACTION_UP:
                        prefs.edit().putFloat(name + "X", v.getX() / Math.max(1, canvas.getWidth() - v.getWidth()))
                            .putFloat(name + "Y", v.getY() / Math.max(1, canvas.getHeight() - v.getHeight())).apply(); return true;
                    case MotionEvent.ACTION_CANCEL: return true;
                    default: return true;
                }
            }
        });
    }
    private int dp(int value) { return (int) (value * getResources().getDisplayMetrics().density + .5f); }
}
