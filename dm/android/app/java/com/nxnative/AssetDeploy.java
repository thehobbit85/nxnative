package com.nxnative;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.HashSet;
import java.util.Set;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/** ADB invokes this from the installed APK, sharing the menu's recovery code. */
public final class AssetDeploy {
    private AssetDeploy() { }
    static GameAssets.Manifest readManifest(File apk) throws IOException {
        try (ZipFile zip = new ZipFile(apk)) {
            ZipEntry entry = zip.getEntry("assets/game/external-files.txt");
            if (entry == null) throw new IOException("Installed APK has no separate-data manifest; rebuild it with --romfs separate");
            GameAssets.Manifest result = GameAssets.Manifest.read(zip.getInputStream(entry));
            if (result.entries.isEmpty()) throw new IOException("Empty separate-data manifest");
            for (GameAssets.Entry asset : result.entries.values())
                if (!asset.data) throw new IOException("Invalid separate-data manifest transport");
            TargetContract.checkApk(apk, result, null);
            return result;
        }
    }
    private static void inventory(File root, File file, Set<String> names) throws IOException {
        if (Files.isSymbolicLink(file.toPath())) throw new IOException("Symbolic link in game assets: " + file);
        if (file.isDirectory()) {
            File[] children = file.listFiles();
            if (children == null) throw new IOException("Could not list game assets: " + file);
            for (File child : children) inventory(root, child, names);
        } else if (file.isFile()) names.add(root.toPath().relativize(file.toPath()).toString().replace(File.separatorChar, '/'));
        else throw new IOException("Invalid game asset: " + file);
    }
    static void verify(File root, GameAssets.Manifest manifest, boolean upload) throws IOException {
        if (!root.isDirectory() || Files.isSymbolicLink(root.toPath())) throw new IOException("Missing game asset directory: " + root);
        Set<String> names = new HashSet<>();
        Set<String> managed = new HashSet<>();
        for (String name : AssetInstall.ROOTS) if (!name.startsWith(".")) managed.add(name);
        if (upload) {
            String[] children = root.list();
            if (children == null) throw new IOException("Could not list deployment");
            for (String child : children) if (!managed.contains(child)) throw new IOException("Unexpected deployment entry: " + child);
        }
        for (String name : managed) {
            File directory = new File(root, name);
            if (directory.exists() || Files.isSymbolicLink(directory.toPath())) inventory(root, directory, names);
        }
        if (!names.equals(manifest.entries.keySet())) throw new IOException("Deployed files do not match the installed APK manifest");
        byte[] buffer = new byte[1 << 20];
        for (GameAssets.Entry entry : manifest.entries.values()) {
            File file = GameAssets.path(root, entry.name);
            if (file.length() != entry.size) throw new IOException("Game asset size differs: " + entry.name);
            MessageDigest digest = GameAssets.digest();
            try (FileInputStream in = new FileInputStream(file)) {
                int n;
                while ((n = in.read(buffer)) != -1) digest.update(buffer, 0, n);
            }
            if (!GameAssets.hex(digest.digest()).equals(entry.hash)) throw new IOException("Game asset digest differs: " + entry.name);
        }
    }
    static void install(File root, GameAssets.Manifest manifest, AssetInstall.Move rename) throws IOException {
        try (AssetInstall.Guard guard = AssetInstall.acquire(root)) { installLocked(root, manifest, rename); }
    }
    private static void installLocked(File root, GameAssets.Manifest manifest, AssetInstall.Move rename) throws IOException {
        AssetInstall.recover(root);
        File upload = new File(root, ".nx-deploy");
        verify(upload, manifest, true); // No live root changes before every byte is checked.
        if (AssetInstall.identity(root).equals(manifest.identity)) {
            boolean valid = false;
            try { verify(root, manifest, false); valid = true; }
            catch (IOException damaged) { /* Reinstall a damaged set even if its marker survived. */ }
            if (valid) { AssetInstall.remove(upload); return; }
        }
        File candidate = AssetInstall.prepare(root, manifest.identity);
        for (String name : AssetInstall.ROOTS) if (!name.startsWith(".")) {
            File from = new File(upload, name), to = new File(candidate, name);
            AssetInstall.remove(to);
            if (from.exists()) AssetInstall.move(from, to);
            else if (!to.mkdirs()) throw new IOException("Could not create candidate root: " + name);
        }
        AssetInstall.remove(upload);
        try { AssetInstall.publish(root, manifest.identity, rename, true); }
        catch (IOException failure) {
            try { AssetInstall.recover(root); }
            catch (IOException recovery) { failure.addSuppressed(recovery); }
            throw failure;
        }
    }
    public static void main(String[] args) {
        try {
            if (args.length != 2) throw new IOException("Expected --check <installed.apk> or <game-root> <installed.apk>");
            GameAssets.Manifest manifest = readManifest(new File(args[1]));
            if (args[0].equals("--check")) { System.out.println(manifest.identity); return; }
            install(new File(args[0]), manifest, AssetInstall::move);
            System.out.println("Game assets verified and committed: " + manifest.identity);
        } catch (Exception error) {
            System.err.println("[RED] " + error.getMessage()); System.exit(1);
        }
    }
}
