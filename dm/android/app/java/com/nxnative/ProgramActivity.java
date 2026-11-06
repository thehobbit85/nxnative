package com.nxnative;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.IBinder;
import android.os.RemoteException;
import android.widget.TextView;

/** Runs in the menu process, waiting for the previous native host to exit. */
public final class ProgramActivity extends Activity {
    private IBinder process;
    private boolean dead, resumed, launched, linked, cancelled;
    private TextView text;
    private final IBinder.DeathRecipient exited = () -> runOnUiThread(() -> { dead = true; launch(); });

    @Override protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        cancelled = saved != null && saved.getBoolean("cancelled");
        text = new TextView(this); text.setTextSize(24); text.setGravity(android.view.Gravity.CENTER);
        String name = getIntent().getStringExtra("name");
        text.setText(cancelled ? "Returning to menu…" : name == null ? "Starting game…" : "Starting " + name + "…"); setContentView(text);
        Bundle extras = getIntent().getExtras(); process = extras == null ? null : extras.getBinder("process");
        if (process == null) { finish(); return; }
        try { process.linkToDeath(exited, 0); linked = true; }
        catch (RemoteException e) { dead = true; } // exited before this activity received the request
    }
    @Override protected void onResume() { super.onResume(); resumed = true; launch(); }
    @Override protected void onSaveInstanceState(Bundle saved) {
        saved.putBoolean("cancelled", cancelled); super.onSaveInstanceState(saved);
    }
    @Override protected void onPause() { resumed = false; super.onPause(); }
    @Override public void onBackPressed() {
        cancelled = true; text.setText("Returning to menu…");
        launch(); // Keep Play behind this activity until the old :game process is gone.
    }
    private void launch() {
        if (!dead || !resumed || launched || isFinishing()) return;
        launched = true;
        if (cancelled) { finish(); return; }
        Intent next = new Intent(this, GameActivity.class);
        next.addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP);
        next.putExtra("handoff", true).putExtra("previous", getIntent().getIntExtra("previous", -1));
        next.putExtra("gameRoot", getIntent().getStringExtra("gameRoot"));
        startActivity(next); finish();
    }
    @Override protected void onDestroy() {
        if (linked) process.unlinkToDeath(exited, 0);
        super.onDestroy();
    }
}
