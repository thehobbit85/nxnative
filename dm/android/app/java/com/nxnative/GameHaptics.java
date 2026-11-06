package com.nxnative;

import android.media.AudioAttributes;
import android.os.Build;
import android.os.CombinedVibration;
import android.os.VibrationAttributes;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.os.VibratorManager;
import android.util.Log;
import android.view.InputDevice;
import java.util.Arrays;

/** All hardware operations run on the activity looper, together with controller hotplug. */
final class GameHaptics {
    private static final AudioAttributes GAME_AUDIO = new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_GAME).build();
    private final Route phone;
    private final Route[] players = new Route[8];

    GameHaptics(GameActivity activity) {
        phone = mono(activity.getSystemService(Vibrator.class));
        controller(0, null);
    }

    // Android 11-15 expose amplitude and timing for portable waveforms. Encode
    // the guest's band-weighted sharpness as pulse spacing on the motor's carrier.
    private static VibrationEffect effect(Vibrator motor, float intensity, float sharpness) {
        if (intensity <= 0) return null;
        long period = Math.round(32 - 24 * sharpness);
        if (!motor.hasAmplitudeControl()) {
            long on = Math.max(1, Math.round(period * intensity));
            return VibrationEffect.createWaveform(new long[]{period - on, on}, 0);
        }
        int peak = Math.max(1, Math.round(255 * intensity));
        return VibrationEffect.createWaveform(new long[]{period / 2, period - period / 2},
                new int[]{peak, Math.max(1, peak / 2)}, 0);
    }

    private static class Route {
        final Vibrator motor;
        boolean playing;
        Route(Vibrator motor) { this.motor = motor; }
        void send(float li, float ls, float ri, float rs, float mi, float ms) {
            VibrationEffect e = effect(motor, mi, ms);
            if (e == null) { stop(); return; }
            motor.vibrate(e, GAME_AUDIO);
            playing = true;
        }
        void cancel() { motor.cancel(); }
        final void stop() {
            if (playing) {
                playing = false;
                try { cancel(); }
                catch (RuntimeException error) { Log.e("nxnative", "[RED] haptics: cancel failed", error); }
            }
        }
    }
    private static Route mono(Vibrator v) { return v != null && v.hasVibrator() ? new Route(v) : null; }

    private static final class Dual extends Route {
        final VibratorManager manager;
        final Vibrator right;
        Dual(VibratorManager manager, Vibrator left, Vibrator right) {
            super(left); this.manager = manager; this.right = right;
        }
        static Route discover(InputDevice pad) {
            VibratorManager mgr = pad.getVibratorManager();
            int[] ids = mgr.getVibratorIds();
            if (ids.length < 2) return ids.length == 1 ? mono(mgr.getVibrator(ids[0])) : null;
            return new Dual(mgr, mgr.getVibrator(ids[0]), mgr.getVibrator(ids[1]));
        }
        // Android's input driver takes one timeline and an array of amplitudes per motor.
        // Giving it two separately timed waveforms makes the right motor use the left
        // motor's timing. Split the timeline at every transition of either envelope.
        private static final int CYCLE_MS = 120;
        private static final class Band {
            final int cycles, threshold, high, low;
            Band(Vibrator motor, float intensity, float sharpness) {
                cycles = Math.round(CYCLE_MS / (32 - 24 * sharpness));
                boolean variable = motor.hasAmplitudeControl();
                threshold = variable ? CYCLE_MS / 2 : Math.max(1, Math.round(CYCLE_MS * intensity));
                high = intensity <= 0 ? 0 : variable ? Math.max(1, Math.round(255 * intensity)) : 255;
                low = variable && high != 0 ? Math.max(1, high / 2) : 0;
            }
            int at(int ms) { return ms * cycles % CYCLE_MS < threshold ? high : low; }
        }
        @Override void send(float li, float ls, float ri, float rs, float mi, float ms) {
            if (li <= 0 && ri <= 0) { stop(); return; }
            Band l = new Band(motor, li, ls), r = new Band(right, ri, rs);
            long[] times = new long[CYCLE_MS];
            int[] leftLevels = new int[CYCLE_MS], rightLevels = new int[CYCLE_MS];
            int count = 0;
            for (int t = 0; t < CYCLE_MS; t++) {
                int a = l.at(t), b = r.at(t);
                if (count == 0 || leftLevels[count-1] != a || rightLevels[count-1] != b) {
                    leftLevels[count] = a; rightLevels[count] = b;
                    count++;
                }
                times[count-1]++;
            }
            times = Arrays.copyOf(times, count);
            CombinedVibration.ParallelCombination pair = CombinedVibration.startParallel();
            if (li > 0) pair.addVibrator(motor.getId(), VibrationEffect.createWaveform(times, Arrays.copyOf(leftLevels, count), 0));
            if (ri > 0) pair.addVibrator(right.getId(), VibrationEffect.createWaveform(times, Arrays.copyOf(rightLevels, count), 0));
            if (Build.VERSION.SDK_INT >= 33)
                manager.vibrate(pair.combine(), VibrationAttributes.createForUsage(VibrationAttributes.USAGE_MEDIA));
            else manager.vibrate(pair.combine());
            playing = true;
        }
        @Override void cancel() { manager.cancel(); }
    }

    void controller(int player, InputDevice pad) {
        Route old = players[player];
        Route next = null;
        if (pad == null) {
            if (player == 0) next = phone;
        } else {
            try { next = Build.VERSION.SDK_INT >= 31 ? Dual.discover(pad) : mono(pad.getVibrator()); }
            catch (RuntimeException error) { Log.e("nxnative", "[RED] haptics: querying controller failed", error); }
        }
        GameActivity.nativeHapticRoute(player, next != null, pad != null);
        if (old != null) old.stop();
        players[player] = next;
    }
    void apply(int player, float li, float ls, float ri, float rs, float mi, float ms) {
        Route route = players[player];
        if (route == null) return;
        try {
            route.send(li, ls, ri, rs, mi, ms);
        } catch (RuntimeException error) {
            Log.e("nxnative", "[RED] haptics: failed to drive player " + (player + 1), error);
            GameActivity.nativeHapticRoute(player, false, false);
            players[player] = null;
            route.stop();
        }
    }
    void stop() {
        for (Route route : players) if (route != null) route.stop();
    }
}
