package com.nxnative;

import android.app.AlertDialog;
import android.os.Looper;
import android.view.KeyEvent;
import android.view.WindowManager;
import java.util.concurrent.CountDownLatch;

/** Controller connection events wake the guest once enough players have joined. */
final class GameControllerSupport {
    private final GameActivity activity;
    private AlertDialog dialog;
    private Request request;
    private boolean paused;
    private static final class Request {
        final CountDownLatch done = new CountDownLatch(1);
        final int min, max;
        int result;
        Request(int min, int max) { this.min = min; this.max = max; }
    }
    GameControllerSupport(GameActivity activity) { this.activity = activity; }
    void pause(boolean value) { paused = value; if (!value) changed(); }
    void close() { if (dialog != null) dialog.dismiss(); }
    void changed() {
        if (dialog == null || paused) return;
        int players = GameActivity.nativeControllerPlayers();
        dialog.setMessage(players + " player(s) connected. Connect controllers for at least " + request.min + " players.");
        // Android assigns every connected controller immediately, so there are
        // no connected controllers waiting for a first button to join.
        if (players >= request.min) { request.result = 1; dialog.dismiss(); }
    }
    int show(int min, int max) {
        if (Looper.myLooper() == Looper.getMainLooper()) return 0;
        Request req = new Request(min, max);
        activity.runOnUiThread(() -> {
            if (dialog != null || activity.isFinishing() || activity.isDestroyed()) { req.done.countDown(); return; }
            try {
                AlertDialog current = new AlertDialog.Builder(activity).setTitle("Connect controllers")
                    .setNegativeButton(android.R.string.cancel, null).create();
                current.setOnDismissListener(d -> {
                    if (dialog == current) { dialog = null; request = null; }
                    req.done.countDown();
                });
                current.setOnKeyListener((d, key, event) -> {
                    if (key == KeyEvent.KEYCODE_BUTTON_A || key == KeyEvent.KEYCODE_ESCAPE || key == KeyEvent.KEYCODE_BACK) {
                        if (event.getAction() == KeyEvent.ACTION_UP) current.dismiss();
                        return true;
                    }
                    return GameControllers.isController(event.getSource());
                });
                request = req; dialog = current; current.show(); changed();
            } catch (WindowManager.BadTokenException e) { dialog = null; request = null; req.done.countDown(); }
        });
        boolean interrupted = false;
        for (;;) {
            try { req.done.await(); break; }
            catch (InterruptedException e) { interrupted = true; }
        }
        if (interrupted) Thread.currentThread().interrupt();
        return req.result;
    }
}
