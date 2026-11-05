package com.nxnative;

import android.app.NativeActivity;
import android.content.Intent;
import android.os.Binder;
import android.os.Bundle;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicReference;

/** The native host still runs once per :game process. */
public final class GameActivity extends NativeActivity {
    private final Binder processLifetime = new Binder();
    private OfflineWeb web;
    private boolean paused;
    static native void nativeWebEvent(long id, int kind, int reason, byte[] message);
    static native long[] nativeWebPad();
    private static native boolean nativeWebKey(int device, int key, int action);
    private static native void nativeWebAxes(int device, float[] axes);
    private static String text(byte[] bytes) { return bytes == null ? "" : new String(bytes, StandardCharsets.UTF_8); }

    public int[] displayPixels() {
        // VI asks for the physical display mode, not the app's surface size or
        // density-scaled logical bounds. Read it each time for mode changes.
        android.view.Display.Mode mode = getWindowManager().getDefaultDisplay().getMode();
        return new int[] {mode.getPhysicalWidth(), mode.getPhysicalHeight()};
    }

    // Never wait for the UI thread: hle_web calls these with its session mutex
    // held, and UI callbacks take the same mutex to deliver page messages.
    public void webCommand(long id, int command, byte[] a, byte[] b, byte[] c, byte[] d, int hidden, int delay, int pulse, int muted) {
        runOnUiThread(() -> {
            if (command == 0) {
                if (web != null) { web.close(); web = null; }
                try { web = new OfflineWeb(this, id, text(a), text(b), text(c), text(d), hidden != 0, delay, pulse, muted != 0); if (paused) web.pause(true); }
                catch (Exception error) { nativeWebEvent(id, 2, 0, error.toString().getBytes(StandardCharsets.UTF_8)); }
            } else if (web != null && web.id == id) {
                if (command == 1) web.show();
                if (command == 2) web.send(text(a));
                if (command == 3) { web.close(); web = null; }
            }
        });
    }
    @Override protected void onPause() { paused = true; if (web != null) web.pause(true); super.onPause(); }
    @Override protected void onResume() { super.onResume(); paused = false; if (web != null) web.pause(false); }
    @Override protected void onDestroy() { if (web != null) { web.close(); web = null; } super.onDestroy(); }
    @Override public boolean dispatchKeyEvent(KeyEvent event) {
        if (web != null && web.active()) {
            if (event.getKeyCode() == KeyEvent.KEYCODE_BACK) { if (event.getAction() == KeyEvent.ACTION_DOWN && event.getRepeatCount() == 0) web.back(); return true; }
            if (nativeWebKey(event.getDeviceId(), event.getKeyCode(), event.getAction())) return true;
        }
        return super.dispatchKeyEvent(event);
    }
    @Override public boolean dispatchGenericMotionEvent(MotionEvent event) {
        if (web != null && web.active() && (event.getSource() & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK) {
            int[] names = {MotionEvent.AXIS_X, MotionEvent.AXIS_Y, MotionEvent.AXIS_Z, MotionEvent.AXIS_RZ, MotionEvent.AXIS_HAT_X, MotionEvent.AXIS_HAT_Y,
                MotionEvent.AXIS_LTRIGGER, MotionEvent.AXIS_RTRIGGER, MotionEvent.AXIS_BRAKE, MotionEvent.AXIS_GAS};
            float[] values = new float[names.length]; for (int i = 0; i < names.length; i++) values[i] = event.getAxisValue(names[i]);
            nativeWebAxes(event.getDeviceId(), values); return true;
        }
        return super.dispatchGenericMotionEvent(event);
    }

    // Called on the guest thread after hle_oe has closed program.bin. Deliver the
    // request before returning to native graceful shutdown; posting alone races exit.
    public void programHandoff(int previous, String name) {
        CountDownLatch delivered = new CountDownLatch(1);
        AtomicReference<RuntimeException> error = new AtomicReference<>();
        runOnUiThread(() -> {
            try {
                Bundle extras = new Bundle();
                extras.putBinder("process", processLifetime);
                extras.putInt("previous", previous); extras.putString("name", name);
                startActivity(new Intent(this, ProgramActivity.class).putExtras(extras));
                finish(); // Back must never expose/recreate this host with its old launch intent.
            } catch (RuntimeException e) { error.set(e); }
            finally { delivered.countDown(); }
        });
        boolean interrupted = false;
        for (;;) {
            try { delivered.await(); break; }
            catch (InterruptedException e) { interrupted = true; }
        }
        if (interrupted) Thread.currentThread().interrupt();
        if (error.get() != null) throw error.get();
    }
}
