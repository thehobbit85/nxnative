package com.nxnative;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Arrays;
import java.util.HashSet;
import java.util.Set;

/** Replace only managed game roots; interrupted renames recover before Play. */
final class AssetInstall {
    // The identity moves last: it is the commit point for the whole asset set.
    static final String[] ROOTS = {"dm", "romfs", "vk", "font", "aoc", ".unpacked"};
    interface Move { void run(File from, File to) throws IOException; }
    private AssetInstall() { }

    static String read(File file) throws IOException {
        if (!file.isFile()) return "";
        if (file.length() > 8192) throw new IOException("Invalid installation marker");
        return new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8).trim();
    }
    static String identity(File root) throws IOException { return read(new File(root, ".unpacked")); }
    static void move(File from, File to) throws IOException {
        if (!from.renameTo(to)) throw new IOException("Could not move game assets: " + from.getName() + " to " + to);
    }
    static void write(File file, String text) throws IOException {
        File tmp = new File(file.getParentFile(), file.getName() + ".tmp");
        try (FileOutputStream out = new FileOutputStream(tmp)) {
            out.write(text.getBytes(StandardCharsets.UTF_8)); out.getFD().sync();
        }
        move(tmp, file);
    }
    static void remove(File file) throws IOException {
        if (!Files.isSymbolicLink(file.toPath()) && file.isDirectory()) {
            File[] entries = file.listFiles();
            if (entries == null) throw new IOException("Could not list game asset staging folder");
            for (File entry : entries) remove(entry);
        }
        if (Files.exists(file.toPath(), java.nio.file.LinkOption.NOFOLLOW_LINKS) && !file.delete())
            throw new IOException("Could not remove old game assets: " + file);
    }
    private static void directory(File file) throws IOException {
        if (!file.isDirectory() && !file.mkdirs()) throw new IOException("Could not create game asset folder: " + file);
    }
    static void recover(File root) throws IOException {
        File previous = new File(root, ".nx-previous"), stage = new File(root, ".nx-install");
        String journal = read(new File(previous, "journal"));
        if (journal.isEmpty()) { remove(previous); return; } // No rename happens before the journal.
        String[] lines = journal.split("\n");
        if (!lines[0].matches("[0-9a-f]{64}")) throw new IOException("Invalid game asset recovery identity");
        Set<String> old = new HashSet<>(Arrays.asList(lines).subList(1, lines.length));
        if (!new HashSet<>(Arrays.asList(ROOTS)).containsAll(old)) throw new IOException("Invalid game asset recovery roots");
        if (!identity(root).equals(lines[0])) {
            for (String name : ROOTS) {
                File backup = new File(previous, name), live = new File(root, name);
                if (backup.exists()) { remove(live); move(backup, live); }
                else if (!old.contains(name)) remove(live);
                // A missing backup for an original root means it was not moved
                // yet, or a previous recovery already restored it.
            }
        }
        remove(previous); remove(stage);
    }
    static File prepare(File root, String identity) throws IOException {
        if (!identity.matches("[0-9a-f]{64}")) throw new IOException("Invalid game asset identity");
        recover(root);
        File stage = new File(root, ".nx-install");
        if (!read(new File(stage, ".identity")).equals(identity)) remove(stage);
        directory(stage);
        for (String name : ROOTS) if (!name.startsWith(".")) directory(new File(stage, name));
        write(new File(stage, ".identity"), identity);
        return stage;
    }
    static void publish(File root, String identity, Move rename) throws IOException {
        File stage = new File(root, ".nx-install"), previous = new File(root, ".nx-previous");
        if (!read(new File(stage, ".identity")).equals(identity)) throw new IOException("Game asset candidate changed");
        if (previous.exists()) throw new IOException("Game asset recovery is required before installation");
        if (identity(root).equals(identity)) { remove(stage); return; }
        write(new File(stage, ".unpacked"), identity);
        StringBuilder journal = new StringBuilder(identity + "\n");
        for (String name : ROOTS) if (new File(root, name).exists()) journal.append(name).append('\n');
        directory(previous); write(new File(previous, "journal"), journal.toString());
        for (String name : ROOTS) {
            File live = new File(root, name);
            if (live.exists()) rename.run(live, new File(previous, name));
            rename.run(new File(stage, name), live);
        }
        remove(previous); remove(stage);
    }
}
