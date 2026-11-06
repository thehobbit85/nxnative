package com.nxnative;

import android.hardware.input.InputManager;
import android.view.InputDevice;
import java.util.HashMap;

/** Observe gamepads independently of button presses so idle players stay connected. */
final class GameControllers implements InputManager.InputDeviceListener {
    private final InputManager input;
    private final HashMap<Integer, String> attached = new HashMap<>();

    GameControllers(GameActivity activity) {
        input = activity.getSystemService(InputManager.class);
        input.registerInputDeviceListener(this, null);
        refresh();
    }

    static boolean isController(int sources) {
        return (sources & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
               (sources & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
    }

    private void refresh() {
        // Retire devices first. A ninth controller can take a slot when one opens.
        for (int id : new java.util.ArrayList<>(attached.keySet())) {
            InputDevice d = input.getInputDevice(id);
            if (d == null || !isController(d.getSources()) || !attached.get(id).equals(d.getDescriptor())) {
                GameActivity.nativePadDetach(id);
                attached.remove(id);
            }
        }
        for (int id : input.getInputDeviceIds()) {
            if (attached.containsKey(id)) continue;
            InputDevice d = input.getInputDevice(id);
            if (d != null && !d.isVirtual() && isController(d.getSources()) &&
                GameActivity.nativePadAttach(id, d.getDescriptor())) attached.put(id, d.getDescriptor());
        }
    }

    @Override public void onInputDeviceAdded(int id) { refresh(); }
    @Override public void onInputDeviceChanged(int id) { refresh(); }
    @Override public void onInputDeviceRemoved(int id) { refresh(); }

    void close() {
        input.unregisterInputDeviceListener(this);
        for (int id : attached.keySet()) GameActivity.nativePadDetach(id);
        attached.clear();
    }
}
