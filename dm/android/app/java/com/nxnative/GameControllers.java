package com.nxnative;

import android.hardware.input.InputManager;
import android.view.InputDevice;
import java.util.HashMap;

/** Observe gamepads independently of button presses so idle players stay connected. */
final class GameControllers implements InputManager.InputDeviceListener {
    private final InputManager input;
    private final GameMotion motion;
    private final GameHaptics haptics;
    private boolean closed;
    private static final class Pad {
        final String descriptor;
        final int player;
        Pad(InputDevice device, int player) { this.descriptor = device.getDescriptor(); this.player = player; }
    }
    private final HashMap<Integer, Pad> attached = new HashMap<>();

    GameControllers(GameActivity activity, GameMotion motion, GameHaptics haptics) {
        this.haptics = haptics;
        this.motion = motion;
        input = activity.getSystemService(InputManager.class);
        input.registerInputDeviceListener(this, null);
        refresh();
    }

    static boolean isController(int sources) {
        return (sources & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
               (sources & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
    }

    private void refresh() {
        if (closed) return;
        // Retire devices first. A ninth controller can take a slot when one opens.
        for (int id : new java.util.ArrayList<>(attached.keySet())) {
            InputDevice d = input.getInputDevice(id);
            if (d == null || !isController(d.getSources()) || !attached.get(id).descriptor.equals(d.getDescriptor())) {
                haptics.controller(attached.get(id).player, null);
                motion.remove(id);
                GameActivity.nativePadDetach(id);
                attached.remove(id);
            }
        }
        for (int id : input.getInputDeviceIds()) {
            if (attached.containsKey(id)) continue;
            InputDevice d = input.getInputDevice(id);
            if (d == null || d.isVirtual() || !isController(d.getSources())) continue;
            int player = GameActivity.nativePadAttach(id, d.getDescriptor());
            if (player >= 0) {
                attached.put(id, new Pad(d, player));
                haptics.controller(player, d);
                motion.controller(d);
            }
        }
    }

    @Override public void onInputDeviceAdded(int id) { refresh(); }
    @Override public void onInputDeviceChanged(int id) {
        Pad previous = attached.get(id);
        refresh();
        InputDevice d = input.getInputDevice(id);
        if (d != null && previous != null && previous == attached.get(id)) {
            motion.controller(d);
            haptics.controller(previous.player, d);
        }
    }
    @Override public void onInputDeviceRemoved(int id) { refresh(); }

    void close() {
        closed = true;
        input.unregisterInputDeviceListener(this);
        for (int id : attached.keySet()) { motion.remove(id); GameActivity.nativePadDetach(id); }
        attached.clear();
    }
}
