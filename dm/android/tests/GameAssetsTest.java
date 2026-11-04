package com.nxnative;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.nio.file.Files;
import java.util.Map;

public final class GameAssetsTest {
    public static void main(String[] args) throws Exception {
        File root = Files.createTempDirectory("nx-assets").toFile();
        File image = GameAssets.path(root, "romfs/romfs.img");
        image.getParentFile().mkdirs(); Files.write(image.toPath(), new byte[]{9});
        GameAssets.finishRomfs(root, false);
        if (image.exists()) throw new AssertionError("stale image shadows directory update");
        GameAssets.DataImport data = new GameAssets.DataImport(root, Map.of("romfs/romfs.img", 5L));
        data.copy("romfs/romfs.img.nxpart/0000000000000000", new ByteArrayInputStream(new byte[]{1,2,3}));
        if (data.complete()) throw new AssertionError("accepted partial image");
        try {
            data.copy("romfs/romfs.img.nxpart/0000000000000004", new ByteArrayInputStream(new byte[]{4,5}));
            throw new AssertionError("accepted gap");
        } catch (java.io.IOException expected) { }
        data.copy("romfs/romfs.img.nxpart/0000000000000003", new ByteArrayInputStream(new byte[]{4,5}));
        if (!data.complete() || !java.util.Arrays.equals(Files.readAllBytes(image.toPath()), new byte[]{1,2,3,4,5}))
            throw new AssertionError("image reconstruction differs");
        GameAssets.finishRomfs(root, true);
        if (!image.exists()) throw new AssertionError("removed current image");
        for (String bad : new String[]{"../save/user.dat", "romfs/../../save/user.dat", "romfs\\bad"}) {
            try { GameAssets.path(root, bad); throw new AssertionError("accepted " + bad); }
            catch (java.io.IOException expected) { }
        }
        if (args.length > 0) {
            GameAssets.DataImport archive = new GameAssets.DataImport(root, Map.of("romfs/romfs.img", 5000000L));
            try (java.util.zip.ZipInputStream zip = new java.util.zip.ZipInputStream(new java.io.FileInputStream(args[0]))) {
                java.util.zip.ZipEntry entry;
                while ((entry = zip.getNextEntry()) != null) archive.copy(entry.getName(), zip);
            }
            if (!archive.complete()) throw new AssertionError("ZIP64 import incomplete");
            byte[] result = Files.readAllBytes(image.toPath());
            for (int i = 0; i < result.length; i++) if (result[i] != "abcde".charAt(i % 5)) throw new AssertionError("ZIP64 data differs");
        }
        System.out.println("GameAssets reconstruction, bounds and image/directory replacement passed");
    }
}
