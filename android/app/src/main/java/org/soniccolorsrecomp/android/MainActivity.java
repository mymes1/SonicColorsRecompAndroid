package org.soniccolorsrecomp.android;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.ActivityInfo;
import android.content.res.Configuration;
import android.database.Cursor;
import android.graphics.Color;
import android.hardware.input.InputManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.OpenableColumns;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;
import android.view.animation.AccelerateDecelerateInterpolator;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.TextView;

import java.io.File;
import java.util.Locale;

/**
 * Sonic Colors (Wii) Android launcher.
 *
 * Flow: pick your own PAL ISO/ZIP → the native installer extracts the game data into the
 * app's private storage → "Play" starts the native runtime on the game surface. The
 * launcher, installer and runtime are all part of the same APK and share one process;
 * the runtime owns the SurfaceView once it starts.
 */
public class MainActivity extends Activity {
    private static final int PICK_DISC = 1;

    private SharedPreferences prefs;
    private FrameLayout root;
    private TextView status;
    private TextView discLine;
    private Button playButton;
    private ProgressBar progress;
    private Uri disc;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private InputManager inputManager;

    /** Native runtime state, polled from the UI thread. */
    private static final int STATE_IDLE = 0;
    private static final int STATE_INSTALLING = 1;
    private static final int STATE_INSTALLED = 2;
    private static final int STATE_RUNNING = 3;
    private static final int STATE_FAILED = 4;

    private GameInputView touch;
    private FrameLayout gameLayer;
    private NativeRunner runner;

    private final InputManager.InputDeviceListener deviceListener = new InputManager.InputDeviceListener() {
        @Override public void onInputDeviceAdded(int id) { refreshControllerState(); }
        @Override public void onInputDeviceChanged(int id) { refreshControllerState(); }
        @Override public void onInputDeviceRemoved(int id) { refreshControllerState(); }
    };

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        inputManager = (InputManager) getSystemService(INPUT_SERVICE);
        if (inputManager != null) inputManager.registerInputDeviceListener(deviceListener, null);
        prefs = getSharedPreferences("launcher", MODE_PRIVATE);

        String uri = prefs.getString("disc", null);
        if (uri != null) disc = Uri.parse(uri);

        boolean wantsGame = prefs.getBoolean("autostart", false) && NativeRunner.dataInstalled();
        if (wantsGame) {
            startGame();
        } else {
            showLauncher();
        }
    }

    // ---------------------------------------------------------------- launcher UI
    private TextView text(String value, float size) {
        TextView view = new TextView(this);
        view.setText(value);
        view.setTextSize(size);
        view.setTextColor(Color.WHITE);
        view.setPadding(0, dp(6), 0, dp(6));
        return view;
    }

    private Button button(String label, View.OnClickListener action) {
        Button b = new Button(this);
        b.setText(label);
        b.setAllCaps(false);
        b.setOnClickListener(action);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2);
        lp.topMargin = dp(8);
        b.setLayoutParams(lp);
        return b;
    }

    private int dp(int value) {
        return (int) (value * getResources().getDisplayMetrics().density + 0.5f);
    }

    private void showLauncher() {
        getWindow().clearFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN);
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED);

        root = new FrameLayout(this);
        ScrollView scroll = new ScrollView(this);
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setPadding(dp(24), dp(28), dp(24), dp(28));
        page.setBackgroundColor(Color.rgb(10, 22, 48));

        page.addView(text("SONIC COLORS", 30));
        page.addView(text("Recompiled for Android", 16));
        page.addView(text("Own the game. No disc image or game data is included in this app; "
                + "the installer reads your own PAL copy (SNCP8P) and extracts it into private storage.", 13));

        discLine = text("", 14);
        page.addView(discLine);
        updateDiscLine();

        page.addView(button(disc == null ? "Choose your disc image…" : "Change disc image…", v -> pickDisc()));

        status = text("", 14);
        page.addView(status);

        progress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progress.setMax(1000);
        progress.setVisibility(View.GONE);
        page.addView(progress);

        playButton = button("Play", v -> startGame());
        page.addView(playButton);

        page.addView(button("Controller & touch settings", v -> showSettings()));
        page.addView(text(BuildConfig.VERSION_NAME + (BuildConfig.RECOMP_INCLUDED ? " · recompiled code included" : ""), 11));

        scroll.addView(page);
        root.addView(scroll);
        setContentView(root);
        refreshState();
    }

    private void updateDiscLine() {
        if (discLine == null) return;
        if (disc == null) {
            discLine.setText("No disc image selected.");
            return;
        }
        String name;
        try (Cursor c = getContentResolver().query(disc, new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            name = (c != null && c.moveToFirst() && !c.isNull(0)) ? c.getString(0) : disc.getLastPathSegment();
        } catch (SecurityException e) {
            name = "(permission lost — choose again)";
        }
        discLine.setText("Disc image: " + name);
    }

    private void refreshState() {
        if (status == null || playButton == null) return;
        final int state = runner != null ? runner.state() : (NativeRunner.dataInstalled() ? STATE_INSTALLED : STATE_IDLE);
        switch (state) {
            case STATE_IDLE:
                status.setText("Game data is not installed yet.");
                playButton.setEnabled(false);
                break;
            case STATE_INSTALLED:
                status.setText("Game data installed.");
                playButton.setEnabled(true);
                break;
            case STATE_INSTALLING:
                status.setText("Installing…");
                playButton.setEnabled(false);
                break;
            case STATE_RUNNING:
                status.setText("Running.");
                playButton.setEnabled(false);
                break;
            default:
                status.setText("Install failed. Check free space and that the image is PAL SNCP8P.");
                playButton.setEnabled(NativeRunner.dataInstalled());
                break;
        }
        if (runner != null) {
            int pct = runner.progress();
            progress.setVisibility(runner.isInstalling() ? View.VISIBLE : View.GONE);
            progress.setProgress(Math.max(0, Math.min(1000, pct * 10)));
            String msg = runner.statusText();
            if (msg != null && !msg.isEmpty()) status.setText(msg);
        }
    }

    private void pickDisc() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        startActivityForResult(intent, PICK_DISC);
    }

    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != PICK_DISC || result != RESULT_OK || data == null || data.getData() == null) return;
        Uri chosen = data.getData();
        String name = displayName(chosen).toLowerCase(Locale.ROOT);
        if (!name.endsWith(".iso") && !name.endsWith(".zip") && !name.endsWith(".rvz")
                && !name.endsWith(".wbfs") && !name.endsWith(".wia") && !name.endsWith(".gcz")) {
            status.setText("Choose an ISO / ZIP / RVZ / WBFS / WIA image of your disc.");
            return;
        }
        try {
            getContentResolver().takePersistableUriPermission(chosen, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException e) {
            status.setText("This file provider does not allow keeping access; copy the file to Downloads and retry.");
            return;
        }
        Uri old = disc;
        disc = chosen;
        prefs.edit().putString("disc", chosen.toString()).apply();
        if (old != null && !old.equals(chosen)) {
            try { getContentResolver().releasePersistableUriPermission(old, Intent.FLAG_GRANT_READ_URI_PERMISSION); }
            catch (SecurityException ignored) { }
        }
        updateDiscLine();
        installDisc();
    }

    private String displayName(Uri uri) {
        try (Cursor c = getContentResolver().query(uri, new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst() && !c.isNull(0)) return c.getString(0);
        } catch (SecurityException ignored) {
            return "disc";
        }
        String last = uri.getLastPathSegment();
        return last == null ? "disc" : last;
    }

    // ---------------------------------------------------------------- install
    private void installDisc() {
        if (disc == null) return;
        if (runner == null) runner = new NativeRunner(this);
        runner.install(disc);
        progress.setVisibility(View.VISIBLE);
        ui.postDelayed(new Runnable() {
            @Override public void run() {
                refreshState();
                if (runner != null && (runner.isInstalling() || runner.state() == STATE_INSTALLED || runner.state() == STATE_FAILED))
                    return;
                ui.postDelayed(this, 250);
            }
        }, 250);
    }

    // ---------------------------------------------------------------- game
    private void startGame() {
        if (!NativeRunner.dataInstalled() && disc == null) {
            if (status != null) status.setText("Choose your disc image first.");
            return;
        }
        if (runner == null) runner = new NativeRunner(this);
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION);

        gameLayer = new FrameLayout(this);
        gameLayer.setBackgroundColor(Color.BLACK);
        View surface = runner.start(this);
        if (surface != null) gameLayer.addView(surface, new FrameLayout.LayoutParams(-1, -1));

        touch = new GameInputView(this);
        gameLayer.addView(touch, new FrameLayout.LayoutParams(-1, -1));

        Button menu = new Button(this);
        menu.setText("≡");
        menu.setAlpha(0.55f);
        FrameLayout.LayoutParams mlp = new FrameLayout.LayoutParams(dp(56), dp(48), Gravity.TOP | Gravity.START);
        mlp.leftMargin = dp(8);
        mlp.topMargin = dp(8);
        menu.setLayoutParams(mlp);
        menu.setOnClickListener(v -> showPauseMenu());
        gameLayer.addView(menu);

        setContentView(gameLayer);
        runner.setAutoStart(true);
        status = null;
        playButton = null;
        progress = null;
        AndroidInput.setConnected(true);
        if (!NativeRunner.isControllerConnected(this))
            toastStatus("No gamepad – touch controls are active");
    }

    private void toastStatus(String msg) {
        if (touch == null) return;
        final TextView t = text(msg, 14);
        t.setBackgroundColor(Color.argb(190, 0, 0, 0));
        t.setPadding(dp(12), dp(8), dp(12), dp(8));
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(-2, -2, Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL);
        lp.bottomMargin = dp(24);
        gameLayer.addView(t, lp);
        ui.postDelayed(() -> { if (gameLayer != null) gameLayer.removeView(t); }, 3500);
    }

    private void showPauseMenu() {
        if (touch != null) touch.releaseAll();
        AlertDialog.Builder b = new AlertDialog.Builder(this);
        b.setTitle("Paused");
        b.setItems(new CharSequence[] {
                "Resume",
                "Touch control settings",
                "Return to launcher (exits the game)"
        }, (dialog, which) -> {
            if (which == 1) {
                showSettings();
            } else if (which == 2) {
                prefs.edit().putBoolean("autostart", false).apply();
                NativeRunner.requestExit(this);
                recreate();
            }
        });
        b.setCancelable(true);
        b.setOnCancelListener(d -> { if (touch != null) touch.releaseAll(); });
        b.show();
    }

    private void showSettings() {
        ScrollView scroll = new ScrollView(this);
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setPadding(dp(20), dp(20), dp(20), dp(20));
        page.setBackgroundColor(Color.rgb(10, 22, 48));

        page.addView(text("Movement input", 18));
        final int[] modes = { GameInputView.MODE_STICK, GameInputView.MODE_DPAD };
        page.addView(button("Movement: " + (touch != null && touch.getMode() == GameInputView.MODE_DPAD ? "D-pad" : "Analog stick"), v -> {
            int next = (touch != null && touch.getMode() == GameInputView.MODE_DPAD) ? GameInputView.MODE_STICK : GameInputView.MODE_DPAD;
            if (touch != null) touch.setMode(next);
            showSettings();
        }));

        page.addView(text("Control opacity", 18));
        SeekBar opacity = new SeekBar(this);
        opacity.setMax(75);
        opacity.setProgress(30);
        opacity.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar bar, int value, boolean fromUser) {
                if (touch != null && fromUser) touch.setOpacity(0.15f + value / 100f);
            }
            @Override public void onStartTrackingTouch(SeekBar bar) { }
            @Override public void onStopTrackingTouch(SeekBar bar) { }
        });
        page.addView(opacity);

        page.addView(button("Rearrange touch controls", v -> {
            if (touch == null) {
                toastNoGame();
                return;
            }
            showTouchEditor();
        }));
        page.addView(button("Reset touch layout", v -> {
            if (touch != null) touch.resetLayout();
        }));

        page.addView(text("Gamepad", 18));
        page.addView(text(NativeRunner.describeControllers(this), 13));

        page.addView(text("Graphics", 18));
        page.addView(text("Internal resolution, frame rate and audio volume live in the game's own "
                + "settings overlay (hold the pad's View button, or tap the ≡ button while running). "
                + "On Mali devices the renderer selects the tiled-friendly path automatically.", 13));

        page.addView(button("Back", v -> {
            if (runner != null && runner.state() == STATE_RUNNING) {
                startGameSurfaceOnly();
            } else {
                showLauncher();
            }
        }));
        scroll.addView(page);
        setContentView(scroll);
    }

    private void toastNoGame() {
        new AlertDialog.Builder(this).setMessage("Start the game first to arrange its touch controls.")
                .setPositiveButton("OK", null).show();
    }

    private void showTouchEditor() {
        FrameLayout layer = new FrameLayout(this);
        layer.setBackgroundColor(Color.BLACK);
        layer.addView(touch, new FrameLayout.LayoutParams(-1, -1));
        Button done = new Button(this);
        done.setText("Done");
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(-2, -2, Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL);
        lp.bottomMargin = dp(16);
        layer.addView(done, lp);
        done.setOnClickListener(v -> {
            touch.setEditMode(false);
            startGameSurfaceOnly();
        });
        touch.setEditMode(true);
        setContentView(layer);
    }

    /** Re-attaches the running game surface after a settings page was shown. */
    private void startGameSurfaceOnly() {
        if (runner == null || gameLayer == null) {
            showLauncher();
            return;
        }
        setContentView(gameLayer);
        status = null;
        playButton = null;
        progress = null;
    }

    @Override public void onBackPressed() {
        if (runner != null && runner.state() == STATE_RUNNING) {
            showPauseMenu();
            return;
        }
        super.onBackPressed();
    }

    // ---------------------------------------------------------------- input forwarding
    @Override public boolean dispatchKeyEvent(KeyEvent event) {
        int token = AndroidInput.fromAndroidKeyCode(event.getKeyCode());
        if (token != 0) {
            if (event.getAction() == KeyEvent.ACTION_DOWN) AndroidInput.setButton(token, true);
            else if (event.getAction() == KeyEvent.ACTION_UP) AndroidInput.setButton(token, false);
            return true;
        }
        if (event.getKeyCode() == KeyEvent.KEYCODE_BUTTON_START) {
            if (event.getAction() == KeyEvent.ACTION_UP && runner != null && runner.state() == STATE_RUNNING) showPauseMenu();
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    @Override public boolean dispatchGenericMotionEvent(MotionEvent event) {
        if ((event.getSource() & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK) {
            float lx = event.getAxisValue(MotionEvent.AXIS_X);
            float ly = -event.getAxisValue(MotionEvent.AXIS_Y);
            float rx = event.getAxisValue(MotionEvent.AXIS_RX);
            float ry = -event.getAxisValue(MotionEvent.AXIS_RY);
            // Deadzone from the settings (same default as the PC build's 0.24).
            lx = applyDeadzone(lx);
            ly = applyDeadzone(ly);
            rx = applyDeadzone(rx);
            ry = applyDeadzone(ry);
            if (event.getSource() == InputDevice.SOURCE_JOYSTICK || lx != 0 || ly != 0) {
                AndroidInput.setStick(lx, ly);
            } else {
                AndroidInput.setStick(lx, ly);
            }
            float rt = event.getAxisValue(MotionEvent.AXIS_RTRIGGER);
            float lt = event.getAxisValue(MotionEvent.AXIS_LTRIGGER);
            if (rt > 0.5f) AndroidInput.setButton(AndroidInput.KEY_R2, true);
            else AndroidInput.setButton(AndroidInput.KEY_R2, false);
            if (lt > 0.5f) AndroidInput.setButton(AndroidInput.KEY_L2, true);
            else AndroidInput.setButton(AndroidInput.KEY_L2, false);
            // HAT axes keep working on pads that report the D-pad that way.
            float hatX = event.getAxisValue(MotionEvent.AXIS_HAT_X);
            float hatY = event.getAxisValue(MotionEvent.AXIS_HAT_Y);
            AndroidInput.setButton(AndroidInput.KEY_LEFT, hatX < -0.5f);
            AndroidInput.setButton(AndroidInput.KEY_RIGHT, hatX > 0.5f);
            AndroidInput.setButton(AndroidInput.KEY_UP, hatY < -0.5f);
            AndroidInput.setButton(AndroidInput.KEY_DOWN, hatY > 0.5f);
            return true;
        }
        return super.dispatchGenericMotionEvent(event);
    }

    private static float applyDeadzone(float v) {
        final float dead = 0.24f;
        if (Math.abs(v) < dead) return 0f;
        float s = v < 0 ? -1f : 1f;
        return s * Math.min(1f, (Math.abs(v) - dead) / (1f - dead));
    }

    private void refreshControllerState() {
        AndroidInput.setConnected(NativeRunner.isControllerConnected(this));
    }

    // ---------------------------------------------------------------- lifecycle
    @Override protected void onPause() {
        if (touch != null) touch.releaseAll();
        if (runner != null) runner.onHostPause();
        super.onPause();
    }

    @Override protected void onResume() {
        super.onResume();
        if (runner != null) runner.onHostResume();
        refreshControllerState();
    }

    @Override public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        if (touch != null) touch.releaseAll();
    }

    @Override protected void onDestroy() {
        if (inputManager != null) inputManager.unregisterInputDeviceListener(deviceListener);
        if (touch != null) touch.releaseAll();
        if (runner != null) runner.stop();
        super.onDestroy();
    }
}
