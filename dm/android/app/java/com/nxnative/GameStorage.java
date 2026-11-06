package com.nxnative;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.util.LinkedHashMap;
import java.util.Map;

/** Only app-owned roots can be selected. Saves and settings always live in filesDir. */
final class GameStorage {
    final Map<File,String> locations = new LinkedHashMap<>();
    private final File marker, primary, internal;

    GameStorage(File files, File[] external) throws IOException {
        marker = new File(files, "game-storage.txt");
        primary = external.length > 0 && external[0] != null ? external[0].getCanonicalFile() : null;
        if (primary != null) locations.put(primary, "Primary shared storage");
        internal = new File(files,"game-data").getCanonicalFile();
        locations.put(internal, "Private app storage");
        for (int i = 1; i < external.length; i++) if (external[i] != null) {
            File dir = external[i].getCanonicalFile();
            locations.putIfAbsent(dir, "Storage volume " + dir);
        }
    }

    File selected() throws IOException {
        String saved = AssetInstall.read(marker);
        File dir;
        if (saved.isEmpty() || saved.equals("primary")) dir = primary;
        else if (saved.equals("private")) dir = internal;
        else dir = new File(saved);
        if (!locations.containsKey(dir)) throw new IOException("Selected storage is unavailable. Reconnect it or choose another game data location.");
        return dir;
    }

    void select(File from, File to, GameAssets.Manifest manifest, boolean bundled) throws IOException {
        if (!locations.containsKey(to) || (from != null && !locations.containsKey(from))) throw new IOException("Unknown game storage");
        if (!to.isDirectory() && !to.mkdirs()) throw new IOException("Could not create " + to);
        try (AssetInstall.Guard target = AssetInstall.acquire(to)) {
            AssetInstall.recover(to);
            boolean matches = manifest.identity.equals(AssetInstall.identity(to));
            boolean canCopy = from != null && !from.equals(to) && manifest.identity.equals(AssetInstall.identity(from));
            if (matches) {
                try { AssetDeploy.verify(to,manifest,false); }
                catch (IOException damaged) { if (!canCopy) throw damaged; matches = false; }
            }
            if (!matches && canCopy) {
                try (AssetInstall.Guard source = AssetInstall.acquire(from)) {
                    AssetInstall.recover(from);
                    if (!manifest.identity.equals(AssetInstall.identity(from))) throw new IOException("Source installation changed");
                    copy(from,to,manifest);
                }
            } else if (!matches && !bundled && !to.equals(primary)) {
                throw new IOException("Deploy this build to primary shared storage first, then copy it here.");
            }
            AssetInstall.write(marker, to.equals(primary) ? "primary" : to.equals(internal) ? "private" : to.getPath());
        }
    }

    private static void copy(File source, File target, GameAssets.Manifest manifest) throws IOException {
        if (target.getUsableSpace() < manifest.total) throw new IOException("Not enough space to copy this game's data.");
        File stage = new File(target,".nx-install");
        AssetInstall.remove(stage);
        try {
            AssetInstall.prepare(target,manifest.identity);
            Map<String,Long> lengths = new LinkedHashMap<>();
            Map<String,String> hashes = new LinkedHashMap<>();
            for (GameAssets.Entry e : manifest.entries.values()) { lengths.put(e.name,e.size); hashes.put(e.name,e.hash); }
            GameAssets.DataImport transfer = new GameAssets.DataImport(stage,lengths,hashes);
            for (GameAssets.Entry e : manifest.entries.values()) {
                try (FileInputStream in = new FileInputStream(GameAssets.path(source,e.name))) { transfer.copy(e.name,in); }
            }
            if (!transfer.complete()) throw new IOException("Game data copy is incomplete");
            AssetInstall.publish(target,manifest.identity,AssetInstall::move,true);
        } catch (IOException failure) {
            try { AssetInstall.recover(target); AssetInstall.remove(stage); }
            catch (IOException recovery) { failure.addSuppressed(recovery); }
            throw failure;
        }
    }
}
