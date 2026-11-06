package com.nxnative;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

/** Simulate a removed volume and interrupted copies with real files, markers and locks. */
public final class GameStorageTest {
    private static void write(File path,String text) throws IOException {
        Files.createDirectories(path.toPath().getParent());
        Files.writeString(path.toPath(),text);
    }
    interface Action { void run() throws IOException; }
    private static void fails(Action action) throws IOException {
        try { action.run(); } catch (IOException expected) { return; }
        throw new AssertionError("Expected a storage error");
    }
    public static void main(String[] argv) throws Exception {
        File base = Files.createTempDirectory("nx-storage").toFile().getCanonicalFile();
        try {
            File files = new File(base,"private"), primary = new File(base,"primary"), sd = new File(base,"sd");
            File internal = new File(files,"game-data");
            GameStorage storage = new GameStorage(files,new File[]{primary,sd});
            assert storage.selected().equals(primary);
            String bytes = "fixture", name = "romfs/check.bin";
            String hash = GameAssets.hex(GameAssets.digest().digest(bytes.getBytes(StandardCharsets.UTF_8)));
            String row = name + "\t7\t=d\t" + hash + "\n";
            GameAssets.Manifest m = GameAssets.Manifest.read(new ByteArrayInputStream(row.getBytes(StandardCharsets.UTF_8)));
            write(new File(files,"save/slot"),"progress"); write(new File(files,"settings.env"),"language");
            write(new File(primary,name),bytes); write(new File(primary,"env.txt"),"debug");
            write(new File(primary,".unpacked"),m.identity);
            fails(() -> storage.select(primary,new File(base,"unlisted"),m,true));
            storage.select(primary,internal,m,true);
            assert storage.selected().equals(internal) && Files.readString(new File(internal,name).toPath()).equals(bytes);
            assert !new File(internal,"env.txt").exists() && new File(primary,name).isFile();
            write(new File(primary,name),"damaged");
            storage.select(internal,primary,m,true); // same marker on damaged destination: repair from verified bytes
            assert Files.readString(new File(primary,name).toPath()).equals(bytes);
            write(new File(sd,name),"old set"); write(new File(sd,".unpacked"),"b".repeat(64));
            write(new File(primary,name),"invalid");
            fails(() -> storage.select(primary,sd,m,true));
            assert storage.selected().equals(primary) && Files.readString(new File(sd,name).toPath()).equals("old set");
            write(new File(primary,name),bytes);
            try (AssetInstall.Guard locked = AssetInstall.acquire(sd)) { fails(() -> storage.select(primary,sd,m,true)); }
            storage.select(primary,sd,m,true);
            GameStorage detached = new GameStorage(files,new File[]{primary,null});
            fails(detached::selected);
            detached.select(null,internal,m,true);
            assert detached.selected().equals(internal);
            assert Files.readString(new File(files,"save/slot").toPath()).equals("progress");
            assert Files.readString(new File(files,"settings.env").toPath()).equals("language");
            System.out.println("PASS: verified storage copy, repair, failure rollback, volume loss and recovery");
        } finally { AssetInstall.remove(base); }
    }
}
