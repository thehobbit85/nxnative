package com.nxnative;

import android.app.NativeActivity;
import android.content.Intent;
import android.os.Binder;
import android.os.Bundle;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicReference;

/** The native host still runs once per :game process. */
public final class GameActivity extends NativeActivity {
    private final Binder processLifetime = new Binder();

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
