package com.nxnative;

import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.os.Build;
import android.os.SystemClock;
import android.view.InputDevice;
import java.util.HashMap;

/** Pair the latest acceleration with each gyroscope sample in the same sensor clock. */
final class GameMotion {
    private static final int PHONE = -1;
    private final GameActivity activity;
    private final HashMap<Integer, Source> sources = new HashMap<>();
    private boolean resumed, focused;
    private final int modes;

    GameMotion(GameActivity activity) {
        this.activity = activity;
        modes = GameActivity.nativeMotionModes();
        if ((modes & 1) != 0) add(PHONE, activity.getSystemService(SensorManager.class));
    }

    private final class Source implements SensorEventListener {
        final int device;
        final SensorManager manager;
        final Sensor accel, gyro;
        final float[] a = new float[3];
        boolean live;
        long offset, subscribedAt, accelTime, gyroTime;

        Source(int id, SensorManager sm, Sensor acc, Sensor rate) {
            device = id; manager = sm; accel = acc; gyro = rate;
        }
        void update(boolean enable) {
            if (enable == live) return;
            if (enable) {
                subscribedAt = SystemClock.elapsedRealtimeNanos();
                offset = GameActivity.nativeMotionClockOffset();
                live = manager.registerListener(this, accel, 10000) && manager.registerListener(this, gyro, 10000);
                if (!live) manager.unregisterListener(this);
            } else {
                live = false;
                manager.unregisterListener(this);
                GameActivity.nativeMotionClear(device);
            }
            accelTime = gyroTime = 0;
        }
        @Override public void onAccuracyChanged(Sensor sensor, int accuracy) {}
        @Override public void onSensorChanged(SensorEvent e) {
            if (!live || e.timestamp < subscribedAt) return;
            if (e.sensor.getType() == Sensor.TYPE_ACCELEROMETER) {
                if (e.timestamp <= accelTime) return;
                System.arraycopy(e.values, 0, a, 0, 3);
                accelTime = e.timestamp;
            } else if (e.timestamp > gyroTime && accelTime > 0 && Math.abs(e.timestamp - accelTime) <= 100000000L) {
                gyroTime = e.timestamp;
                int orientation = device == PHONE ? activity.getWindowManager().getDefaultDisplay().getRotation() : 0;
                GameActivity.nativeMotionSample(device, orientation, gyroTime - offset, a[0], a[1], a[2],
                                                e.values[0], e.values[1], e.values[2]);
            }
        }
    }

    private void add(int id, SensorManager manager) {
        if (manager == null) return;
        Sensor a = manager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER);
        Sensor w = manager.getDefaultSensor(Sensor.TYPE_GYROSCOPE);
        if (a == null || w == null) return;
        Source source = new Source(id, manager, a, w);
        sources.put(id, source);
        source.update(resumed && focused);
    }
    void controller(InputDevice device) {
        if ((modes & 2) != 0 && Build.VERSION.SDK_INT >= 31) {
            remove(device.getId());
            add(device.getId(), device.getSensorManager());
        }
    }
    void remove(int id) {
        Source source = sources.remove(id);
        if (source != null) source.update(false);
    }
    void resume(boolean value) { resumed = value; update(); }
    void focus(boolean value) { focused = value; update(); }
    private void update() { for (Source s : sources.values()) s.update(resumed && focused); }
}
