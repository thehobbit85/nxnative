// Start-up menu of the Android app: console mode, frame rate, screen fit, language, frame-rate counter, the
// cheats of the game's build (each one chosen by the player, none on by default) and "do not show again". The choices go to <files>/settings.env, which the game's
// native activity (app/front_android.c, process ":game") reads at boot. The menu stays under the game: when the game ends (− and + held two seconds) it shows again.
// On first start the bundled save (assets/save, the player's progress from another device) seeds the app's save folder; the cheat file is refreshed from assets/cheats.txt.
// An APK with the game inside (make_apk.py --bundle: assets/game/ and its list files.txt, each file as its pieces in assets/game/p/) unpacks it into the external folder on its first start, and again after an update,
// with a progress bar; Play waits for it.
// The menu works with the touch screen and with a controller (A confirms, B goes back, START plays). The screen stays on while it is open, as in the game.
package com.nxnative;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.res.AssetManager;
import android.graphics.Color;
import android.os.Bundle;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;

public class MenuActivity extends Activity {
    private SharedPreferences prefs;
    private Spinner mode, fps, fit, lang;
    private String[] languageCodes;
    private CheckBox showFps, skip;
    private final List<CheckBox> cheatBoxes = new ArrayList<>();
    private boolean launched;
    private Button playButton;
    private boolean playKeyDown;
    private TextView status; private android.widget.ProgressBar bar; private volatile boolean unpacking;
    private GameAssets.Manifest manifest;
    private boolean externalAssets;
    private GameStorage storage;
    private File gameRoot;
    private String preparationError;
    private static boolean installationActive; // Accessed on the UI thread, including worker completion callbacks.

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        prefs = getSharedPreferences("menu", MODE_PRIVATE);
        try {
            try (InputStream in = getAssets().open("game/files.txt")) { manifest = GameAssets.Manifest.read(in); }
            catch (java.io.FileNotFoundException e) {
                try (InputStream in = getAssets().open("game/external-files.txt")) {
                    manifest = GameAssets.Manifest.read(in); externalAssets = true;
                }
            }
            TargetContract.checkApk(new File(getApplicationInfo().sourceDir), manifest, getPackageName());
            storage = new GameStorage(getFilesDir(),getExternalFilesDirs(null));
            gameRoot = storage.selected();
            if (!installationActive) {
                try (AssetInstall.Guard guard = AssetInstall.acquire(gameRoot)) { AssetInstall.recover(gameRoot); }
            }
        } catch (Exception e) { preparationError = e.getMessage() != null ? e.getMessage() : e.toString(); }
        if (preparationError == null) seedFiles();
        if (!prefs.contains("lang")) {
            try (BufferedReader r = new BufferedReader(new InputStreamReader(getAssets().open("default-language.txt"), StandardCharsets.UTF_8))) {
                prefs.edit().putString("lang", r.readLine()).apply();
            } catch (Exception e) { /* No packaged language override. */ }
        }
        buildUi();
        if (installationActive) { waitForInstallation(); return; }
        if (preparationError != null) { preparationFailed(preparationError); dataButton.setOnClickListener(v -> recreate()); return; }
        if (needsUnpack()) { if (pendingData() != null) askData(null); else unpack(); return; }
        boolean forced = new File(getFilesDir(), "show_menu").delete();
        if (prefs.getBoolean("skip", false) && !forced && b == null && gameFilesPresent()) play();
    }
    @Override protected void onPause() { super.onPause(); if (mode != null) save(); }   // choices are kept also when the menu is left without playing
    @Override protected void onResume() {
        super.onResume();
        playKeyDown = false;
        if (launched && new File(getFilesDir(), "show_menu").delete()) launched = false;   // back from the game with − and +
    }

    // The bundled save goes in only when the app has none yet (never over progress made here); the cheat file always follows the APK
    private void seedFiles() {
        File save = new File(getFilesDir(), "save");
        String[] have = save.list();
        if (have == null || have.length == 0) copyAssetDir("save", save);
        File diagnostics = getExternalFilesDir(null);
        File env = new File(diagnostics == null ? getFilesDir() : diagnostics, "env.txt");
        if (!env.exists()) copyAsset("default.env", env);
        copyAsset("cheats.txt", new File(getFilesDir(), "cheats.txt"));
        copyAssetDir("cheats", new File(getFilesDir(), "cheats"));
        copyAsset("cheats-build.txt", new File(getFilesDir(), "cheats-build.txt"));
    }
    private void copyAssetDir(String dir, File to) {
        try {
            String[] names = getAssets().list(dir);
            if (names == null || names.length == 0) return;
            to.mkdirs();
            for (String n : names) { String[] sub = getAssets().list(dir + "/" + n);   // folders inside the save (profile0/, userdata/) have entries, files none
                if (sub != null && sub.length > 0) copyAssetDir(dir + "/" + n, new File(to, n)); else copyAsset(dir + "/" + n, new File(to, n)); }
        } catch (Exception e) { /* no bundled save */ }
    }
    private void copyAsset(String name, File to) {
        try (InputStream in = getAssets().open(name, AssetManager.ACCESS_STREAMING); OutputStream out = new FileOutputStream(to)) {
            byte[] buf = new byte[65536]; int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        } catch (Exception e) { /* not bundled */ }
    }
    // ---- The game inside the APK ----
    private long versionCode() { try { return getPackageManager().getPackageInfo(getPackageName(), 0).getLongVersionCode(); } catch (Exception e) { return 0; } }
    private boolean bundled() { return manifest != null && !externalAssets; }
    private static File gameFile(File root, String name) throws java.io.IOException {
        return GameAssets.path(root, name);
    }
    private boolean needsUnpack() {
        if (preparationError != null) return true;
        if (!bundled()) return false;
        try { return !manifest.identity.equals(AssetInstall.identity(gameRoot)); }
        catch (Exception e) { return true; }
    }
    private void waitForInstallation() {
        playButton.setEnabled(false); status.setVisibility(View.VISIBLE); status.setText("Preparing game files...");
        status.postDelayed(() -> {
            if (isFinishing() || isDestroyed()) return;
            if (installationActive) waitForInstallation(); else recreate();
        }, 500);
    }
    private void preparationFailed(String error) {
        playButton.setEnabled(false); status.setVisibility(View.VISIBLE); bar.setVisibility(View.GONE);
        status.setText("Could not prepare the game: " + error); status.setTextColor(Color.rgb(255, 120, 120));
        dataButton.setText("Retry preparation"); dataButton.setOnClickListener(v -> unpack()); dataButton.setVisibility(View.VISIBLE);
    }
    private String recoverInstallation(Exception error) {
        String message = error.getMessage() != null ? error.getMessage() : error.toString();
        try {
            AssetInstall.recover(gameRoot);
            if (!needsUnpack()) return null; // All roots committed; only cleanup had failed.
        } catch (Exception recovery) { message += "; recovery failed: " + recovery.getMessage(); }
        return message;
    }
    private void unpack() {
        if (installationActive || manifest == null) return;
        installationActive = unpacking = true; playButton.setEnabled(false); dataButton.setVisibility(View.GONE);
        status.setVisibility(View.VISIBLE); bar.setVisibility(View.VISIBLE);
        status.setTextColor(Color.WHITE);
        status.setText("Preparing game files (this may take a few minutes)...");
        new Thread(() -> {
            String err = null;
            try {
                File ext = gameRoot;
                AssetInstall.recover(ext); AssetInstall.remove(new File(ext, ".nx-install"));
                if (manifest.total > ext.getUsableSpace() - (200L << 20)) throw new Exception(String.format(Locale.ROOT, "Not enough storage: %.1f GB free is required.", manifest.total / 1e9));
                File stage = AssetInstall.prepare(ext, manifest.identity);
                long total = 0; boolean later = false;
                for (GameAssets.Entry f : manifest.entries.values()) { if (f.data) later = true; else total += f.size; }
                long done = 0; byte[] buf = new byte[1 << 20]; int lastPct = -1;
                for (GameAssets.Entry f : manifest.entries.values()) {
                    if (f.data) continue;
                    File to = gameFile(stage, f.name); to.getParentFile().mkdirs();
                    java.security.MessageDigest hash = GameAssets.digest(); long written = 0;
                    try (OutputStream out = new FileOutputStream(to)) {
                        for (String part : f.pieces) try (InputStream in = getAssets().open("game/p/" + part, AssetManager.ACCESS_STREAMING)) {
                            int n; while ((n = in.read(buf)) > 0) {
                                if (n > f.size - written) throw new Exception("Oversized game asset: " + f.name);
                                out.write(buf, 0, n); hash.update(buf, 0, n); done += n; written += n;
                                int pct = (int) (done * 100 / Math.max(1, total));
                                if (pct != lastPct) { lastPct = pct; final int p = pct; final long d = done, t = total; runOnUiThread(() -> { bar.setProgress(p);
                                    status.setText(String.format(Locale.ROOT, "Preparing game files: %d%% (%.1f of %.1f GB)", p, d / 1e9, t / 1e9)); }); } }
                        }
                    }
                    if (written != f.size || !GameAssets.hex(hash.digest()).equals(f.hash)) throw new Exception("Game asset size or digest differs: " + f.name);
                }
                if (later) AssetInstall.write(new File(stage, ".pending_data"), manifest.identity);
                else AssetInstall.publish(ext, manifest.identity, AssetInstall::move);
            } catch (Exception e) { err = recoverInstallation(e); }
            final String fe = err;
            runOnUiThread(() -> {
                installationActive = unpacking = false;
                if (fe != null) { preparationFailed(fe); return; }
                if (pendingData() != null) { askData(null); return; }
                status.setText("Game ready."); bar.setVisibility(View.GONE); playButton.setEnabled(true); playButton.requestFocus();
            });
        }).start();
    }
    // ---- The data file (a game too big for one APK, make_apk.py): <name>-datos.zip, chosen once by the player, its files written beside the APK's ----
    private Button dataButton;
    private String dataName() {
        try (BufferedReader r = new BufferedReader(new InputStreamReader(getAssets().open("game/datos.txt"), StandardCharsets.UTF_8))) { return r.readLine().split("\t")[0]; }
        catch (Exception e) { return "datos.zip"; }
    }
    private List<GameAssets.Entry> pendingData() {
        if (manifest == null || gameRoot == null) return null;
        File stage = new File(gameRoot, ".nx-install"); List<GameAssets.Entry> out = new ArrayList<>();
        try {
            if (!manifest.identity.equals(AssetInstall.read(new File(stage, ".pending_data"))) ||
                !manifest.identity.equals(AssetInstall.read(new File(stage, ".identity")))) return null;
            for (GameAssets.Entry f : manifest.entries.values()) if (f.data) out.add(f);
        } catch (Exception e) { return null; }
        return out.isEmpty() ? null : out;
    }
    private void askData(String problem) {
        playButton.setEnabled(false); status.setVisibility(View.VISIBLE); bar.setVisibility(View.GONE);
        status.setTextColor(problem != null ? Color.rgb(255, 120, 120) : Color.WHITE);
        dataButton.setText("Choose data file"); dataButton.setOnClickListener(v -> pickData());
        status.setText((problem != null ? problem + "\n" : "") + "One more step: the game needs a separate data file. Tap \"Choose data file\" and select " + dataName()
                       + " (copied alongside the APK). Import it once, then you may delete that file.");
        dataButton.setVisibility(View.VISIBLE); dataButton.requestFocus();
    }
    private void pickData() {
        android.content.Intent i = new android.content.Intent(android.content.Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(android.content.Intent.CATEGORY_OPENABLE); i.setType("*/*");
        try { startActivityForResult(i, 7); } catch (Exception e) { askData("No file picker is available on this Android device."); }
    }
    @Override protected void onActivityResult(int req, int res, android.content.Intent data) {
        super.onActivityResult(req, res, data);
        if (req == 7 && res == RESULT_OK && data != null && data.getData() != null) importData(data.getData());
    }
    private void importData(android.net.Uri uri) {
        List<GameAssets.Entry> want = pendingData(); if (want == null || installationActive) return;
        java.util.Map<String, Long> left = new java.util.HashMap<>(); java.util.Map<String, String> hashes = new java.util.HashMap<>(); long total = 0;
        for (GameAssets.Entry f : want) { left.put(f.name, f.size); hashes.put(f.name, f.hash); total += f.size; }
        final long tot = total;
        installationActive = unpacking = true; dataButton.setVisibility(View.GONE); bar.setVisibility(View.VISIBLE); status.setTextColor(Color.WHITE);
        status.setText("Copying game data...");
        new Thread(() -> {
            String err = null; File ext = gameRoot;
            try (java.util.zip.ZipInputStream zin = new java.util.zip.ZipInputStream(new java.io.BufferedInputStream(getContentResolver().openInputStream(uri), 1 << 20))) {
                File stage = AssetInstall.prepare(ext, manifest.identity);
                GameAssets.DataImport assets = new GameAssets.DataImport(stage, left, hashes);
                java.util.zip.ZipEntry e; long done = 0; byte[] buf = new byte[1 << 20]; int lastPct = -1; boolean checked = false;
                while ((e = zin.getNextEntry()) != null) {
                    String name = e.getName();
                    if (name.equals("nxdatos.txt")) {   // this package and this build: a data file from another version would not match the APK's files
                        java.io.ByteArrayOutputStream bo = new java.io.ByteArrayOutputStream(); int k;
                        while ((k = zin.read(buf)) > 0) { if (bo.size() + k > 512) throw new Exception("Invalid data-file header"); bo.write(buf, 0, k); }
                        String[] id = new String(bo.toByteArray(), StandardCharsets.UTF_8).trim().split("\t");
                        if (id.length < 2 || !id[0].equals(getPackageName()) || !id[1].equals(String.valueOf(versionCode()))) throw new Exception("the data file belongs to a different game build.");
                        checked = true; continue;
                    }
                    if (!checked) throw new Exception("this is not the data file for this game.");
                    done += assets.copy(name, zin);
                    int pct = (int) (done * 100 / Math.max(1, tot));
                    if (pct != lastPct) { lastPct = pct; final int p = pct; final long d = done;
                        runOnUiThread(() -> { bar.setProgress(p); status.setText(String.format(Locale.ROOT,
                            "Copying game data: %d%% (%.1f of %.1f GB)", p, d / 1e9, tot / 1e9)); }); }

                }
                if (!checked) throw new Exception("this is not the data file for this game.");
                if (!assets.complete()) throw new Exception("Game assets are missing from the data archive.");
                AssetInstall.publish(ext, manifest.identity, AssetInstall::move);
            } catch (Exception ex) { err = recoverInstallation(ex); }
            final String fe = err;
            runOnUiThread(() -> {
                installationActive = unpacking = false;
                if (fe != null) { if (pendingData() != null) askData("Could not copy game data: " + fe); else preparationFailed(fe); return; }
                status.setText("Game ready. You can now delete " + dataName() + "."); bar.setVisibility(View.GONE); playButton.setEnabled(true); playButton.requestFocus();
            });
        }).start();
    }
    private boolean gameFilesPresent() {
        File ext = gameRoot;
        if (ext == null) return false;
        try { if (manifest == null || !manifest.identity.equals(AssetInstall.identity(ext))) return false; }
        catch (Exception e) { return false; }
        return ext != null && new File(ext, "romfs").isDirectory() && new File(ext, "dm").isDirectory() && new File(ext, "vk").isDirectory();
    }

    // Stable host keys, including duplicate suffixes; master codes always run.
    private List<String> cheatNames() {
        List<String> out = new ArrayList<>();
        File f = new File(getFilesDir(), "cheats.txt");
        if (!f.exists()) return out;
        try (BufferedReader r = new BufferedReader(new InputStreamReader(new java.io.FileInputStream(f), StandardCharsets.UTF_8))) {
            return CheatSelections.keys(r);
        } catch (Exception e) { /* unreadable: no cheats */ }
        return out;
    }

    private TextView text(String s, float size, boolean bold) {
        TextView t = new TextView(this); t.setText(s); t.setTextSize(size); t.setTextColor(Color.WHITE);
        if (bold) t.setTypeface(t.getTypeface(), android.graphics.Typeface.BOLD);
        t.setPadding(0, 18, 0, 6); return t;
    }
    private Spinner spinner(LinearLayout parent, String label, String[] items, int sel) {
        parent.addView(text(label, 16, false));
        Spinner s = new Spinner(this);
        ArrayAdapter<String> a = new ArrayAdapter<>(this, android.R.layout.simple_spinner_item, items);
        a.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        s.setAdapter(a); s.setSelection(Math.max(0, Math.min(sel, items.length - 1)));
        parent.addView(s); return s;
    }
    private void chooseLocation() {
        if (installationActive) return;
        File[] dirs = storage.locations.keySet().toArray(new File[0]);
        String[] labels = new String[dirs.length];
        for (int i = 0; i < dirs.length; i++) {
            File dir = dirs[i], volume = dir.isDirectory() ? dir : dir.getParentFile();
            labels[i] = String.format(Locale.ROOT,"%s (%.1f GB free)",storage.locations.get(dir),volume.getUsableSpace()/1e9);
        }
        new android.app.AlertDialog.Builder(this).setTitle("Choose game data location")
            .setItems(labels,(dialog,which) -> {
                if (dirs[which].equals(gameRoot) && preparationError == null) return;
                save();
                installationActive = unpacking = true;
                playButton.setEnabled(false); dataButton.setVisibility(View.GONE);
                status.setVisibility(View.VISIBLE); status.setTextColor(Color.WHITE);
                status.setText("Preparing " + storage.locations.get(dirs[which]) + "…");
                new Thread(() -> {
                    String error = null;
                    try { storage.select(gameRoot,dirs[which],manifest,bundled()); }
                    catch (Exception e) { error = e.getMessage() == null ? e.toString() : e.getMessage(); }
                    final String failed = error;
                    runOnUiThread(() -> {
                        installationActive = unpacking = false;
                        if (isFinishing() || isDestroyed()) return;
                        if (failed == null) { recreate(); return; }
                        String message = "Could not switch game storage: " + failed;
                        if (pendingData() != null) { askData(message); return; }
                        if (!gameFilesPresent()) {
                            preparationFailed(message); dataButton.setOnClickListener(v -> recreate()); return;
                        }
                        status.setText(message); status.setTextColor(Color.rgb(255,120,120));
                        playButton.setEnabled(true);
                    });
                }).start();
            }).setNegativeButton("Cancel",null).show();
    }

    private void buildUi() {
        ScrollView sv = new ScrollView(this); sv.setBackgroundColor(Color.rgb(16, 18, 28));
        LinearLayout col = new LinearLayout(this); col.setOrientation(LinearLayout.VERTICAL); col.setPadding(56, 32, 56, 48);
        sv.addView(col, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        col.addView(text(getApplicationInfo().loadLabel(getPackageManager()) + " — Settings", 24, true));
        col.addView(text("Choose your settings. Hold − and + for two seconds while playing to return here.", 14, false));
        status = text("", 15, true); status.setVisibility(View.GONE); col.addView(status);
        bar = new android.widget.ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal); bar.setMax(100); bar.setVisibility(View.GONE); col.addView(bar);
        dataButton = new Button(this); dataButton.setText("Choose data file"); dataButton.setTextSize(18); dataButton.setVisibility(View.GONE);
        dataButton.setOnClickListener(v -> pickData()); col.addView(dataButton);
        if (storage != null) {
            Button location = new Button(this);
            String current = storage.locations.get(gameRoot);
            location.setText("Game data location" + (current == null ? "" : " — " + current));
            location.setOnClickListener(v -> chooseLocation());
            col.addView(location);
        }
        if (!gameFilesPresent() && !bundled()) {
            TextView w = text("Game data is missing or belongs to another build. Install this APK's matching game data from your computer, then reopen the app.", 15, true);
            w.setTextColor(Color.rgb(255, 120, 120)); col.addView(w);
        }
        mode = spinner(col, "Console mode", new String[]{"Handheld", "Docked (TV, higher resolution)"}, prefs.getInt("opmode", 0) == 1 ? 1 : 0);
        fps = spinner(col, "Frame rate", new String[]{"60 (default)", "30 (save battery)"}, prefs.getInt("fps", 60) == 30 ? 1 : 0);
        fit = spinner(col, "Picture", new String[]{"Keep aspect ratio (black bars)", "Stretch to fill"}, prefs.getBoolean("stretch", false) ? 1 : 0);
        String sys = systemLang();
        String selectedLang = prefs.getString("lang", "system");
        boolean extraLang = !selectedLang.equals("es-419") && !selectedLang.equals("en-US") && !selectedLang.equals("system");
        languageCodes = extraLang ? new String[]{"es-419", "en-US", "system", selectedLang} : new String[]{"es-419", "en-US", "system"};
        String[] langs = languageCodes;
        int ls = 2; for (int i = 0; i < langs.length; i++) if (langs[i].equals(prefs.getString("lang", "system"))) ls = i;
        String[] languageLabels = extraLang ? new String[]{"Spanish (Latin America)", "English", "System language (" + sys + ")", selectedLang}
                                          : new String[]{"Spanish (Latin America)", "English", "System language (" + sys + ")"};
        lang = spinner(col, "Game language", languageLabels, ls);
        showFps = new CheckBox(this); showFps.setText("Show frame rate"); showFps.setTextColor(Color.WHITE); showFps.setChecked(prefs.getBoolean("showfps", false));
        col.addView(showFps);
        List<String> cheats = cheatNames();
        Set<String> on = prefs.getStringSet("cheat_keys", new HashSet<>());
        if (cheats.isEmpty()) col.addView(text("No cheats are bundled for this build.", 15, false));
        else {
            col.addView(text("Cheats (choose which to enable)", 18, true));
            for (int i = 0; i < cheats.size(); i++) {
                CheckBox c = new CheckBox(this); c.setText(cheats.get(i)); c.setTextColor(Color.WHITE); c.setChecked(on.contains(cheats.get(i)));
                cheatBoxes.add(c); col.addView(c);
            }
        }
        skip = new CheckBox(this); skip.setText("Skip this menu (hold − and + while playing to reopen it)"); skip.setTextColor(Color.WHITE);
        skip.setChecked(prefs.getBoolean("skip", false)); col.addView(skip);
        Button play = playButton = new Button(this); play.setText("Play (START)"); play.setTextSize(20);
        if (externalAssets && !gameFilesPresent()) play.setEnabled(false);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT); lp.topMargin = 28;
        play.setOnClickListener(v -> { save(); play(); });
        col.addView(play, lp);
        setContentView(sv);
    }
    private static String systemLang() {
        Locale l = Locale.getDefault(); String g = l.getLanguage(), c = l.getCountry();
        if (g.equals("es")) return c.equals("ES") ? "es" : "es-419";
        if (g.equals("en")) return c.equals("US") ? "en-US" : "en-GB";
        if (g.equals("pt")) return c.equals("BR") ? "pt-BR" : "pt";
        if (g.equals("fr")) return c.equals("CA") ? "fr-CA" : "fr";
        if (g.equals("zh")) return c.equals("TW") || c.equals("HK") ? "zh-Hant" : "zh-Hans";
        return g;
    }
    private void save() {
        Set<String> on = new HashSet<>(); StringBuilder list = new StringBuilder();
        for (CheckBox box : cheatBoxes) if (box.isChecked()) { String key = box.getText().toString(); on.add(key); list.append(key).append('\n'); }
        String[] langs = languageCodes;
        prefs.edit().putInt("opmode", mode.getSelectedItemPosition() == 1 ? 1 : 0).putInt("fps", fps.getSelectedItemPosition() == 1 ? 30 : 60)
             .putBoolean("stretch", fit.getSelectedItemPosition() == 1).putString("lang", langs[lang.getSelectedItemPosition()])
             .putBoolean("showfps", showFps.isChecked()).putStringSet("cheat_keys", on).putBoolean("skip", skip.isChecked()).apply();
        String l = langs[lang.getSelectedItemPosition()]; if (l.equals("system")) l = systemLang();
        try (BufferedReader r = new BufferedReader(new InputStreamReader(new java.io.FileInputStream(new File(getFilesDir(), "cheats-build.txt")), StandardCharsets.UTF_8))) {
            String bid = r.readLine();
            if (bid != null && bid.matches("[0-9A-Fa-f]{16}")) {
                try (OutputStream o = new FileOutputStream(new File(getFilesDir(), "cheats/" + bid + ".enabled"))) {
                    o.write(list.toString().getBytes(StandardCharsets.UTF_8));
                }
            }
        } catch (java.io.FileNotFoundException e) { /* No bundled cheats. */ }
        catch (Exception e) { status.setText("Could not save cheat selections: " + e.getMessage()); status.setVisibility(View.VISIBLE); }
        String env = "DM_OPMODE=" + (mode.getSelectedItemPosition() == 1 ? 1 : 0) + "\nDM_FPS=" + (fps.getSelectedItemPosition() == 1 ? 30 : 60)
                   + "\nDM_SCREEN_FIT=" + (fit.getSelectedItemPosition() == 1 ? "stretch" : "keep") + "\nDM_LANG=" + l + "\nDM_SHOW_FPS=" + (showFps.isChecked() ? 1 : 0)
                   + "\n";
        try (OutputStream o = new FileOutputStream(new File(getFilesDir(), "settings.env"))) { o.write(env.getBytes(StandardCharsets.UTF_8)); } catch (Exception e) { /* the defaults apply */ }
    }
    private void play() {
        if (unpacking || needsUnpack() || !gameFilesPresent()) return;
        if (!new File(getFilesDir(), "settings.env").exists()) save();
        launched = true;
        Intent i = new Intent(this, GameActivity.class);
        i.putExtra("gameRoot", gameRoot.getPath());
        startActivity(i);
    }

    // A controller: A confirms like the centre of the D-pad, B goes back
    @Override public boolean dispatchKeyEvent(KeyEvent e) {
        int k = e.getKeyCode();
        if (k == KeyEvent.KEYCODE_BUTTON_START || (k == KeyEvent.KEYCODE_BUTTON_A && getCurrentFocus() == null)) {   // nothing chosen yet (touch mode): A plays, as START always does
            // only a press that began here: the + still held from leaving the game with − and + must not start it again when it is let go
            if (e.getAction() == KeyEvent.ACTION_DOWN && e.getRepeatCount() == 0) playKeyDown = true;
            else if (e.getAction() == KeyEvent.ACTION_UP && playKeyDown) { playKeyDown = false; save(); play(); }
            return true;
        }
        if (k == KeyEvent.KEYCODE_BUTTON_A) return super.dispatchKeyEvent(new KeyEvent(e.getDownTime(), e.getEventTime(), e.getAction(), KeyEvent.KEYCODE_DPAD_CENTER, e.getRepeatCount()));
        if (k == KeyEvent.KEYCODE_BUTTON_B) return super.dispatchKeyEvent(new KeyEvent(e.getDownTime(), e.getEventTime(), e.getAction(), KeyEvent.KEYCODE_BACK, e.getRepeatCount()));
        return super.dispatchKeyEvent(e);
    }
}
