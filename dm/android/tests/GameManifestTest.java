package com.nxnative;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Map;

public final class GameManifestTest {
    private static GameAssets.Manifest parse(String text) throws Exception {
        return GameAssets.Manifest.read(new ByteArrayInputStream(text.getBytes(StandardCharsets.UTF_8)));
    }
    private static String hash(byte[] data) { return GameAssets.hex(GameAssets.digest().digest(data)); }
    private static void check(boolean value, String message) { if (!value) throw new AssertionError(message); }
    public static void main(String[] args) throws Exception {
        byte[] good = new byte[]{1, 2, 3, 4, 5}; String digest = hash(good);
        String row = "romfs/romfs.img\t5\t=d\t" + digest + "\n";
        GameAssets.Manifest manifest = parse(row);
        check(parse(row.replace("romfs/romfs.img", "programs/1/romfs/romfs.img")).entries.containsKey("programs/1/romfs/romfs.img"), "child RomFS omitted");
        for (String path : new String[]{"html/html-document/index.html", "programs/1/html/html0/html-document/index.html"})
            check(parse(row.replace("romfs/romfs.img", path)).entries.containsKey(path), "HTML omitted: " + path);
        check(manifest.identity.equals(hash(row.getBytes(StandardCharsets.UTF_8))), "identity does not cover complete manifest");
        check(manifest.entries.get("romfs/romfs.img").size == 5 && manifest.entries.get("romfs/romfs.img").data, "data entry differs");
        check(parse("romfs/empty\t0\t\t" + hash(new byte[0]) + "\n").entries.get("romfs/empty").pieces.length == 0, "empty file needs no pieces");
        for (String bad : new String[]{row + row, row.replace("\t5\t", "\t-1\t"), row.replace(digest, "bad"),
                row.replace("romfs/romfs.img", "romfs/../env.txt"), row.replace("romfs/romfs.img", "romfs//bad"),
                row.replace("=d", "../piece"), "romfs/file\t5\t=d\n",
                row.replace("romfs/romfs.img", "programs/0/romfs/file"), row.replace("romfs/romfs.img", "programs/1/save/progress"),
                row.replace("romfs/romfs.img", "programs/1/romfs/../../env.txt")}) {
            try { parse(bad); throw new AssertionError("accepted malformed manifest: " + bad); }
            catch (IOException expected) { }
        }
        File root = Files.createTempDirectory("nx-manifest").toFile();
        try {
            GameAssets.DataImport data = new GameAssets.DataImport(root, Map.of("romfs/romfs.img", 5L), Map.of("romfs/romfs.img", digest));
            data.copy("romfs/romfs.img.nxpart/0000000000000000", new ByteArrayInputStream(new byte[]{1,2,3}));
            data.copy("romfs/romfs.img.nxpart/0000000000000003", new ByteArrayInputStream(new byte[]{4,5}));
            check(data.complete(), "valid chunk digest rejected");
            GameAssets.DataImport wrong = new GameAssets.DataImport(root, Map.of("romfs/romfs.img", 5L), Map.of("romfs/romfs.img", digest));
            try {
                wrong.copy("romfs/romfs.img", new ByteArrayInputStream(new byte[]{9,8,7,6,5}));
                throw new AssertionError("same-size wrong companion accepted");
            } catch (IOException expected) { check(!wrong.complete(), "bad digest marked complete"); }
            GameAssets.DataImport empty = new GameAssets.DataImport(root, Map.of("romfs/empty", 0L), Map.of("romfs/empty", hash(new byte[0])));
            empty.copy("romfs/empty", new ByteArrayInputStream(new byte[0]));
            check(empty.complete() && new File(root, "romfs/empty").length() == 0, "empty data file rejected");
        } finally { AssetInstall.remove(root); }
        System.out.println("Game manifest: content identity, paths, fields, duplicate entries, empty files and same-size wrong companion rejection passed");
    }
}
