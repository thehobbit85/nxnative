package com.nxnative;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Arrays;

public final class AssetInstallTest {
    private static final String OLD = "a".repeat(64), NEW = "b".repeat(64);
    private static void write(File file, String value) throws Exception {
        file.getParentFile().mkdirs(); Files.write(file.toPath(), value.getBytes(StandardCharsets.UTF_8));
    }
    private static String read(File file) throws Exception { return new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8); }
    private static void check(boolean value, String message) { if (!value) throw new AssertionError(message); }
    private static void interruption(int stop, boolean existing) throws Exception {
        File root = Files.createTempDirectory("nx-install").toFile();
        try {
            write(new File(root, "env.txt"), "settings"); write(new File(root, "save/progress"), "save");
            if (existing) {
                for (String name : AssetInstall.ROOTS) if (!name.startsWith(".")) write(new File(root, name + "/old"), name);
                write(new File(root, ".unpacked"), OLD);
            }
            File stage = AssetInstall.prepare(root, NEW);
            for (String name : AssetInstall.ROOTS) if (!name.startsWith(".")) write(new File(stage, name + "/new"), name);
            int[] moves = {0};
            try {
                AssetInstall.publish(root, NEW, (from, to) -> {
                    if (stop == -1) throw new IOException("interrupted before the first move");
                    AssetInstall.move(from, to);
                    if (++moves[0] == stop) throw new IOException("interrupted after move " + stop);
                });
                check(stop == 0, "failure was not reached");
            } catch (IOException expected) { check(stop != 0, "unexpected failure: " + expected); }
            boolean committed = NEW.equals(AssetInstall.identity(root));
            AssetInstall.recover(root); AssetInstall.recover(root);
            for (String name : AssetInstall.ROOTS) if (!name.startsWith(".")) {
                check(new File(root, name + "/new").exists() == committed, "mixed new root: " + name);
                check(new File(root, name + "/old").exists() == (existing && !committed), "lost old root: " + name);
            }
            check(AssetInstall.identity(root).equals(committed ? NEW : existing ? OLD : ""), "marker does not match assets");
            check(read(new File(root, "env.txt")).equals("settings") && read(new File(root, "save/progress")).equals("save"), "settings or save changed");
            check(!new File(root, ".nx-previous").exists() && !new File(root, ".nx-install").exists(), "transaction leftovers");
        } finally { AssetInstall.remove(root); }
    }
    public static void main(String[] args) throws Exception {
        interruption(0, true); interruption(0, false); interruption(-1, true); interruption(-1, false);
        for (int i = 1; i <= 12; i++) interruption(i, true);
        for (int i = 1; i <= 6; i++) interruption(i, false);
        File root = Files.createTempDirectory("nx-install-resume").toFile();
        try {
            File stage = AssetInstall.prepare(root, OLD);
            write(new File(stage, "dm/prepared"), "candidate");
            check(new File(AssetInstall.prepare(root, OLD), "dm/prepared").exists(), "matching candidate not resumed");
            check(!new File(AssetInstall.prepare(root, NEW), "dm/prepared").exists(), "different candidate reused");
            write(new File(root, "aoc/stale"), "removed DLC");
            AssetInstall.publish(root, NEW, AssetInstall::move);
            check(Arrays.equals(new File(root, "aoc").list(), new String[0]), "removed DLC survived replacement");
            write(new File(root, "dm/current"), "current");
            File duplicate = AssetInstall.prepare(root, NEW);
            write(new File(duplicate, "dm/not-installed"), "unused");
            AssetInstall.publish(root, NEW, (from, to) -> { throw new AssertionError("same identity moved live roots"); });
            check(new File(root, "dm/current").exists() && !new File(root, "dm/not-installed").exists(), "same identity changed live assets");
        } finally { AssetInstall.remove(root); }
        System.out.println("Asset installation: successful publish, every interrupted rename, repeated recovery, stale roots and candidate identity passed");
    }
}
