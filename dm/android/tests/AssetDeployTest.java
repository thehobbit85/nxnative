package com.nxnative;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

public final class AssetDeployTest {
    private static void check(boolean yes, String message) { if (!yes) throw new AssertionError(message); }
    private static void write(File file, String value) throws Exception {
        file.getParentFile().mkdirs(); Files.write(file.toPath(), value.getBytes(StandardCharsets.UTF_8));
    }
    private static String row(String name, String text) {
        byte[] bytes = text.getBytes(StandardCharsets.UTF_8);
        return name + "\t" + bytes.length + "\t=d\t" + GameAssets.hex(GameAssets.digest().digest(bytes)) + "\n";
    }
    private static final String TEXT = row("dm/main.patched.bin", "new code") + row("romfs/file", "new data") + row("vk/empty", "");
    private static GameAssets.Manifest manifest() throws Exception {
        return GameAssets.Manifest.read(new ByteArrayInputStream(TEXT.getBytes(StandardCharsets.UTF_8)));
    }
    private static void upload(File root) throws Exception {
        File stage = new File(root, ".nx-deploy");
        AssetInstall.remove(stage);
        write(new File(stage, "dm/main.patched.bin"), "new code");
        write(new File(stage, "romfs/file"), "new data");
        write(new File(stage, "vk/empty"), "");
    }
    public static void main(String[] args) throws Exception {
        File root = Files.createTempDirectory("nx-deploy").toFile();
        try {
            write(new File(root, "dm/old"), "old code"); write(new File(root, "romfs/file"), "old data");
            write(new File(root, "vk/old"), "old shader"); write(new File(root, "aoc/old"), "stale DLC");
            write(new File(root, "env.txt"), "settings"); write(new File(root, "save/progress"), "save");
            write(new File(root, ".unpacked"), "a".repeat(64));
            for (String damage : new String[]{"wrong", "missing", "extra", "symlink", "root symlink"}) {
                upload(root); File stage = new File(root, ".nx-deploy");
                if (damage.equals("wrong")) write(new File(stage, "romfs/file"), "bad data");
                if (damage.equals("missing")) Files.delete(new File(stage, "romfs/file").toPath());
                if (damage.equals("extra")) write(new File(stage, "romfs/unlisted"), "extra");
                if (damage.equals("symlink")) {
                    Files.delete(new File(stage, "romfs/file").toPath());
                    Files.createSymbolicLink(new File(stage, "romfs/file").toPath(), new File(root, "env.txt").toPath());
                }
                if (damage.equals("root symlink")) {
                    AssetInstall.remove(new File(stage, "romfs"));
                    Files.createSymbolicLink(new File(stage, "romfs").toPath(), new File(root, "romfs").toPath());
                }
                try { AssetDeploy.install(root, manifest(), AssetInstall::move); throw new AssertionError("accepted " + damage); }
                catch (IOException expected) { }
                check(new File(root, "dm/old").isFile() && Files.readString(new File(root, "romfs/file").toPath()).equals("old data"), "rejected deployment changed live data");
                check(AssetInstall.identity(root).equals("a".repeat(64)), "rejected deployment changed marker");
            }
            upload(root); AssetDeploy.install(root, manifest(), AssetInstall::move);
            check(AssetInstall.identity(root).equals(manifest().identity), "identity not committed");
            check(!new File(root, "aoc/old").exists() && !new File(root, "dm/old").exists(), "stale files survived");
            check(Files.readString(new File(root, "env.txt").toPath()).equals("settings") && Files.readString(new File(root, "save/progress").toPath()).equals("save"), "user files changed");
            upload(root); AssetDeploy.install(root, manifest(), (from, to) -> { throw new AssertionError("unchanged deployment moved roots"); });
            write(new File(root, "romfs/file"), "bad data");
            upload(root); AssetDeploy.install(root, manifest(), AssetInstall::move);
            check(Files.readString(new File(root, "romfs/file").toPath()).equals("new data"), "same-identity corruption not repaired");
            File apk = new File(root, "fixture.apk");
            try (ZipOutputStream zip = new ZipOutputStream(new FileOutputStream(apk))) {
                zip.putNextEntry(new ZipEntry("assets/game/external-files.txt")); zip.write(TEXT.getBytes(StandardCharsets.UTF_8)); zip.closeEntry();
            }
            check(AssetDeploy.readManifest(apk).identity.equals(manifest().identity), "APK manifest differs");
            try (ZipOutputStream zip = new ZipOutputStream(new FileOutputStream(apk))) { zip.putNextEntry(new ZipEntry("other")); zip.closeEntry(); }
            try { AssetDeploy.readManifest(apk); throw new AssertionError("accepted APK without external contract"); }
            catch (IOException expected) { }
        } finally { AssetInstall.remove(root); }
        System.out.println("External deployment: APK binding, content rejection, atomic install, unchanged reuse and corruption repair passed");
    }
}
