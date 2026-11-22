package org.soniccolorsrecomp.android;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.os.Build;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.view.MotionEvent;
import android.view.View;

import java.util.LinkedHashMap;
import java.util.Map;

/**
 * Touch controls over the game surface.
 *
 * The layout is a Wii Remote standing on its side, which is how Sonic Colors is played
 * with a GameCube/Nunchuk-style controller: a floating analog stick on the left (or a
 * D-pad, configurable), a two-button cluster on the right (A = jump, B = the Wii B /
 * slide button), and Start on the top right. Every control is a round hit area, the
 * touch that presses it is remembered so sliding a thumb off a button releases it, and
 * positions are stored as fractions of the view so they survive rotation and TVs.
 *
 * Input is delivered to the native runtime through {@link AndroidInput}, which is the
 * same state the Activity forwards gamepad events to, so both sources share one path.
 */
public class GameInputView extends View {
    public static final int MODE_STICK = 0;
    public static final int MODE_DPAD = 1;

    /** One configurable control: a button or the movement input. */
    static final class Control {
        final String id;
        final String label;
        int key;          // AndroidKeycode-ish token consumed by AndroidInput; 0 = movement
        float x, y;       // centre, as a fraction of width/height
        float size;       // diameter, as a fraction of the smaller side
        boolean movement;
        Control(String id, String label, int key, float x, float y, float size, boolean movement) {
            this.id = id; this.label = label; this.key = key; this.x = x; this.y = y;
            this.size = size; this.movement = movement;
        }
    }

    private final Map<String, Control> controls = new LinkedHashMap<>();
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint textPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final ControlLayoutStore store;
    private int mode = MODE_STICK;
    private boolean editing;
    private float opacity = 0.45f;

    // Active touches: pointer id -> control id ("" for the empty area, which drags the stick)
    private final Map<Integer, String> pointerControl = new LinkedHashMap<>();
    private final Map<Integer, float[]> pointerStart = new LinkedHashMap<>();
    private String dragControl;          // control being repositioned while editing

    // Virtual stick state (analog): origin + current vector in pixels.
    private float stickOriginX = -1, stickOriginY = -1, stickDx, stickDy;
    private int stickPointer = -1;
    private int dpadMask;

    private Vibrator vibrator;
    private long lastHapticMs;

    public interface OnEditFinished {
        void onEditFinished();
    }
    private OnEditFinished editFinished;

    public GameInputView(Context context) {
        super(context);
        store = new ControlLayoutStore(context);
        setFocusable(false);
        textPaint.setColor(Color.WHITE);
        textPaint.setTextAlign(Paint.Align.CENTER);
        textPaint.setTypeface(Typeface.DEFAULT_BOLD);
        resetLayout();
        loadLayout();
    }

    public void setEditFinishedListener(OnEditFinished l) {
        editFinished = l;
    }

    /** Re-read the stored layout (settings screen, reset). */
    public final void loadLayout() {
        mode = store.getMode();
        store.load(controls);
        for (Control c : controls.values()) {
            c.x = store.getFloat(c.id + ".x", c.x);
            c.y = store.getFloat(c.id + ".y", c.y);
            c.size = store.getFloat(c.id + ".size", c.size);
        }
        opacity = store.getFloat("opacity", opacity);
        invalidate();
    }

    public void setOpacity(float value) {
        opacity = Math.max(0.15f, Math.min(0.9f, value));
        store.putFloat("opacity", opacity);
        invalidate();
    }

    public void setEditMode(boolean on) {
        editing = on;
        if (!on) saveLayout();
        invalidate();
    }

    public boolean isEditing() {
        return editing;
    }

    public void resetLayout() {
        controls.clear();
        // A Wii Remote on its side / Nunchuk layout. Fractions are of the view size.
        controls.put("stick", new Control("stick", "", 0, 0.16f, 0.70f, 0.30f, true));
        controls.put("A", new Control("A", "A", AndroidInput.KEY_A, 0.86f, 0.68f, 0.13f, false));
        controls.put("B", new Control("B", "B", AndroidInput.KEY_B, 0.74f, 0.80f, 0.11f, false));
        controls.put("START", new Control("START", "II", AndroidInput.KEY_PLUS, 0.90f, 0.16f, 0.085f, false));
        controls.put("DPAD", new Control("DPAD", "", AndroidInput.KEY_UP, 0.16f, 0.70f, 0.30f, true));
        saveLayout();
        invalidate();
    }

    private void saveLayout() {
        store.putMode(mode);
        for (Control c : controls.values()) {
            store.putFloat(c.id + ".x", c.x);
            store.putFloat(c.id + ".y", c.y);
            store.putFloat(c.id + ".size", c.size);
        }
    }

    public int getMode() {
        return mode;
    }

    public void setMode(int m) {
        mode = m;
        store.putMode(m);
        invalidate();
    }

    private Control movementControl() {
        return mode == MODE_DPAD ? controls.get("DPAD") : controls.get("stick");
    }

    // ---------------------------------------------------------------- geometry
    private float radius(Control c) {
        return Math.min(getWidth(), getHeight()) * c.size * 0.5f;
    }

    private float cx(Control c) {
        return c.x * getWidth();
    }

    private float cy(Control c) {
        return c.y * getHeight();
    }

    private Control hitTest(float x, float y) {
        // The movement control is a big circle; buttons are small and on top of it.
        Control best = null;
        float bestRadius = Float.MAX_VALUE;
        for (Control c : controls.values()) {
            if (c.movement && mode == MODE_STICK && !c.id.equals("stick")) continue;
            if (c.movement && mode == MODE_DPAD && !c.id.equals("DPAD")) continue;
            if (!c.movement && (mode == MODE_STICK ? c.id.equals("DPAD") : c.id.equals("stick"))) continue;
            float dx = x - cx(c), dy = y - cy(c);
            if (dx * dx + dy * dy <= radius(c) * radius(c) && radius(c) < bestRadius) {
                best = c;
                bestRadius = radius(c);
            }
        }
        return best;
    }

    // ---------------------------------------------------------------- drawing
    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        Control move = movementControl();
        if (move != null) drawMovement(canvas, move);
        for (Control c : controls.values()) {
            if (c.movement) continue;
            boolean down = pointerControl.containsValue(c.id);
            drawButton(canvas, c, down);
        }
        if (editing) drawEditOverlay(canvas);
    }

    private void drawMovement(Canvas canvas, Control c) {
        float r = radius(c);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(Math.max(2f, r * 0.06f));
        paint.setColor(Color.argb((int) (255 * opacity * 0.7f), 255, 255, 255));
        canvas.drawCircle(cx(c), cy(c), r, paint);
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(Color.argb((int) (255 * opacity * 0.25f), 0, 0, 0));
        canvas.drawCircle(cx(c), cy(c), r, paint);
        if (mode == MODE_DPAD) {
            paint.setColor(Color.argb((int) (255 * opacity), 255, 255, 255));
            paint.setStrokeWidth(Math.max(2f, r * 0.12f));
            paint.setStyle(Paint.Style.STROKE);
            float a = r * 0.55f;
            canvas.drawLine(cx(c), cy(c) - a, cx(c), cy(c) + a, paint);
            canvas.drawLine(cx(c) - a, cy(c), cx(c) + a, cy(c), paint);
            paint.setStyle(Paint.Style.FILL);
            return;
        }
        float knobR = r * 0.42f;
        float kx = cx(c), ky = cy(c);
        if (stickPointer >= 0) {
            kx += stickDx;
            ky += stickDy;
        }
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(Color.argb((int) (255 * (opacity + 0.2f) * 0.8f), 220, 235, 255));
        canvas.drawCircle(kx, ky, knobR, paint);
        paint.setColor(Color.argb((int) (255 * opacity), 255, 255, 255));
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(Math.max(1.5f, r * 0.04f));
        canvas.drawCircle(kx, ky, knobR, paint);
    }

    private void drawButton(Canvas canvas, Control c, boolean down) {
        float r = radius(c);
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(down ? Color.argb((int) (255 * 0.75f), 70, 150, 255)
                            : Color.argb((int) (255 * opacity * 0.5f), 10, 20, 40));
        canvas.drawCircle(cx(c), cy(c), r, paint);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(Math.max(2f, r * 0.07f));
        paint.setColor(Color.argb((int) (255 * (opacity + 0.35f)), 255, 255, 255));
        canvas.drawCircle(cx(c), cy(c), r, paint);
        if (!c.label.isEmpty()) {
            textPaint.setTextSize(r * (c.label.length() > 2 ? 0.55f : 0.9f));
            textPaint.setColor(Color.argb((int) (255 * Math.min(1f, opacity + 0.4f)), 255, 255, 255));
            float ty = cy(c) - (textPaint.descent() + textPaint.ascent()) * 0.5f;
            canvas.drawText(c.label, cx(c), ty, textPaint);
        }
    }

    private void drawEditOverlay(Canvas canvas) {
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(3f);
        paint.setColor(Color.argb(220, 255, 210, 70));
        for (Control c : controls.values()) {
            if (c.movement && c.id.equals(mode == MODE_STICK ? "DPAD" : "stick")) continue;
            canvas.drawCircle(cx(c), cy(c), radius(c) + 8, paint);
        }
        textPaint.setTextSize(Math.max(28f, getHeight() * 0.035f));
        textPaint.setColor(Color.argb(235, 255, 230, 140));
        canvas.drawText("Drag controls — Done to save", getWidth() / 2f, getHeight() * 0.08f, textPaint);
    }

    // ---------------------------------------------------------------- touch
    @Override
    public boolean onTouchEvent(MotionEvent e) {
        final int action = e.getActionMasked();
        final int index = e.getActionIndex();
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                float x = e.getX(index), y = e.getY(index);
                int id = e.getPointerId(index);
                Control hit = hitTest(x, y);
                if (editing) {
                    if (hit != null) {
                        dragControl = hit.id;
                        pointerStart.put(id, new float[] { x - cx(hit), y - cy(hit) });
                        pointerControl.put(id, hit.id);
                    }
                    return true;
                }
                if (hit == null) return true;
                pointerControl.put(id, hit.id);
                if (hit.movement) {
                    if (mode == MODE_STICK) {
                        stickPointer = id;
                        stickOriginX = cx(hit);
                        stickOriginY = cy(hit);
                        stickDx = 0;
                        stickDy = 0;
                        AndroidInput.setStick(0f, 0f);
                    } else {
                        updateDpad(x, y, hit);
                    }
                    haptic(8);
                } else {
                    AndroidInput.setButton(hit.key, true);
                    haptic(12);
                }
                invalidate();
                return true;
            }
            case MotionEvent.ACTION_MOVE: {
                for (int i = 0; i < e.getPointerCount(); i++) {
                    int id = e.getPointerId(i);
                    String ctlId = pointerControl.get(id);
                    if (ctlId == null) continue;
                    Control c = controls.get(ctlId);
                    if (c == null) continue;
                    if (editing) {
                        float[] off = pointerStart.get(id);
                        if (off == null || !ctlId.equals(dragControl)) continue;
                        c.x = Math.max(0f, Math.min(1f, (e.getX(i) - off[0]) / Math.max(1, getWidth())));
                        c.y = Math.max(0f, Math.min(1f, (e.getY(i) - off[1]) / Math.max(1, getHeight())));
                        invalidate();
                        continue;
                    }
                    if (c.movement) {
                        if (mode == MODE_STICK && id == stickPointer) {
                            float r = radius(c);
                            float dx = e.getX(i) - stickOriginX;
                            float dy = e.getY(i) - stickOriginY;
                            float len = (float) Math.hypot(dx, dy);
                            float max = r * 0.9f;
                            if (len > max && len > 0) {
                                dx = dx / len * max;
                                dy = dy / len * max;
                            }
                            stickDx = dx;
                            stickDy = dy;
                            // Screen down is +y; the Wii stick reports up as +y.
                            float nx = dx / Math.max(1f, max);
                            float ny = -dy / Math.max(1f, max);
                            AndroidInput.setStick(nx, ny);
                            invalidate();
                        } else if (mode == MODE_DPAD) {
                            updateDpad(e.getX(i), e.getY(i), c);
                        }
                    }
                }
                return true;
            }
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP: {
                int id = e.getPointerId(index);
                String ctlId = pointerControl.remove(id);
                pointerStart.remove(id);
                if (ctlId != null) {
                    Control c = controls.get(ctlId);
                    if (!editing && c != null) {
                        if (c.movement) {
                            if (mode == MODE_STICK && id == stickPointer) {
                                stickPointer = -1;
                                stickDx = stickDy = 0;
                                AndroidInput.setStick(0f, 0f);
                            } else {
                                updateDpad(-1, -1, c);
                            }
                        } else {
                            AndroidInput.setButton(c.key, false);
                        }
                    }
                    if (editing && ctlId.equals(dragControl)) dragControl = null;
                }
                invalidate();
                return true;
            }
            case MotionEvent.ACTION_CANCEL: {
                releaseAll();
                return true;
            }
            default:
                return true;
        }
    }

    private void updateDpad(float x, float y, Control c) {
        int mask = 0;
        if (x >= 0 && y >= 0) {
            float dx = x - cx(c), dy = y - cy(c);
            float dead = radius(c) * 0.18f;
            if (Math.abs(dx) > dead) mask |= dx > 0 ? AndroidInput.KEY_RIGHT : AndroidInput.KEY_LEFT;
            if (Math.abs(dy) > dead) mask |= dy > 0 ? AndroidInput.KEY_DOWN : AndroidInput.KEY_UP;
        }
        if (mask != dpadMask) {
            int changed = mask ^ dpadMask;
            if ((changed & AndroidInput.KEY_UP) != 0) AndroidInput.setButton(AndroidInput.KEY_UP, (mask & AndroidInput.KEY_UP) != 0);
            if ((changed & AndroidInput.KEY_DOWN) != 0) AndroidInput.setButton(AndroidInput.KEY_DOWN, (mask & AndroidInput.KEY_DOWN) != 0);
            if ((changed & AndroidInput.KEY_LEFT) != 0) AndroidInput.setButton(AndroidInput.KEY_LEFT, (mask & AndroidInput.KEY_LEFT) != 0);
            if ((changed & AndroidInput.KEY_RIGHT) != 0) AndroidInput.setButton(AndroidInput.KEY_RIGHT, (mask & AndroidInput.KEY_RIGHT) != 0);
            dpadMask = mask;
            invalidate();
        }
    }

    /** Releases every button and the stick (pause, focus loss, overlay). */
    public void releaseAll() {
        for (Control c : controls.values()) {
            if (!c.movement) AndroidInput.setButton(c.key, false);
            else if (c.id.equals("DPAD")) {
                AndroidInput.setButton(AndroidInput.KEY_UP, false);
                AndroidInput.setButton(AndroidInput.KEY_DOWN, false);
                AndroidInput.setButton(AndroidInput.KEY_LEFT, false);
                AndroidInput.setButton(AndroidInput.KEY_RIGHT, false);
            }
        }
        AndroidInput.setStick(0f, 0f);
        pointerControl.clear();
        pointerStart.clear();
        stickPointer = -1;
        stickDx = stickDy = 0;
        dpadMask = 0;
        dragControl = null;
        invalidate();
    }

    private void haptic(int ms) {
        if (store.getBoolean("haptics", true) == false) return;
        long now = System.currentTimeMillis();
        if (now - lastHapticMs < 40) return;
        lastHapticMs = now;
        try {
            if (vibrator == null) vibrator = (Vibrator) getContext().getSystemService(Context.VIBRATOR_SERVICE);
            if (vibrator == null || !vibrator.hasVibrator()) return;
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                vibrator.vibrate(VibrationEffect.createOneShot(ms, VibrationEffect.DEFAULT_AMPLITUDE));
            } else {
                vibrator.vibrate(ms);
            }
        } catch (SecurityException | IllegalStateException ignored) {
            // Vibration is optional; never let it break input.
        }
    }
}
