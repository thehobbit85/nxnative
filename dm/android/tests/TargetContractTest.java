package com.nxnative;

import java.io.ByteArrayInputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;

public final class TargetContractTest {
    static String hash(byte[] data) { return GameAssets.hex(GameAssets.digest().digest(data)); }
    static void rejected(String target, byte[] host, GameAssets.Manifest assets, String pkg) throws Exception {
        try { TargetContract.verify(target, host, assets, pkg); throw new AssertionError("accepted incompatible target"); }
        catch (IOException expected) { }
    }
    public static void main(String[] args) throws Exception {
        String sha = "a".repeat(64), id = "0100633007D48000", pkg = "com.nxnative.t" + id.toLowerCase();
        String producer = "schema\t1\nplatform\tandroid\narch\tarm64-v8a\nhost_abi\t1\ntp_slot\t2\nshader_schema\t1\nshader_chain\tcacaa37ecb55\nexporter\t" + sha + "\nprofile\t" + id + "\nprofile_header\t" + sha + "\nfeature\tguest-nvn\nprofile_input\t" + id + "/game.tsv\t" + sha + "\n";
        byte[] host = ("native bytes NXNATIVE_ANDROID_HOST_V1\n" + producer + "NXNATIVE_ANDROID_HOST_END\n").getBytes(StandardCharsets.UTF_8);
        String rows = "dm/main.patched.bin\t4\t=d\t" + sha + "\ndm/main.fixups.tsv\t5\t=d\t" + sha + "\n";
        GameAssets.Manifest assets = GameAssets.Manifest.read(new ByteArrayInputStream(rows.getBytes(StandardCharsets.UTF_8)));
        String target = producer + "title\t" + id + "\nhost_sha256\t" + hash(host) + "\nshader_export\tempty\nasset\tdm/main.patched.bin\t4\t" + sha + "\nasset\tdm/main.fixups.tsv\t5\t" + sha + "\nmodule\tmain\t" + "b".repeat(32) + "\t" + sha + "\t" + sha + "\n";
        TargetContract.verify(target, host, assets, pkg);
        TargetContract.verify(target, host, assets, pkg + ".test");
        java.nio.file.Path apk = java.nio.file.Files.createTempFile("nx-target", ".apk");
        try {
            try (java.util.zip.ZipOutputStream zip = new java.util.zip.ZipOutputStream(java.nio.file.Files.newOutputStream(apk))) {
                for (String[] part : new String[][]{{"assets/game/target.tsv", target}, {"assets/game/external-files.txt", rows}}) {
                    zip.putNextEntry(new java.util.zip.ZipEntry(part[0])); zip.write(part[1].getBytes(StandardCharsets.UTF_8)); zip.closeEntry();
                }
                zip.putNextEntry(new java.util.zip.ZipEntry("lib/arm64-v8a/libdmhost.so")); zip.write(host); zip.closeEntry();
            }
            TargetContract.checkApk(apk.toFile(), assets, pkg);
            if (!AssetDeploy.readManifest(apk.toFile()).identity.equals(assets.identity)) throw new AssertionError("deployment contract differs");
        } finally { java.nio.file.Files.delete(apk); }

        for (String[] change : new String[][]{{"schema\t1", "schema\t2"}, {"platform\tandroid", "platform\tios"}, {"arch\tarm64-v8a", "arch\tx86_64"}, {"tp_slot\t2", "tp_slot\t3"}, {"host_abi\t1", "host_abi\t2"}, {"shader_schema\t1", "shader_schema\t2"}, {"feature\tguest-nvn", "feature\tguest-vulkan"}}) {
            // Change both producer and target: matching records still cannot opt
            // into an ABI/backend this menu cannot launch.
            byte[] other = new String(host, StandardCharsets.UTF_8).replace(change[0], change[1]).getBytes(StandardCharsets.UTF_8);
            rejected(target.replace(change[0], change[1]).replace(hash(host), hash(other)), other, assets, pkg);
        }
        rejected(target, Arrays.copyOf(host, host.length - 1), assets, pkg);
        rejected(target + "title\t" + id + "\n", host, assets, pkg);
        rejected(target.replace("\t4\t", "\t3\t"), host, assets, pkg);
        rejected(target.replace("module\tmain", "module\tother"), host, assets, pkg);
        rejected(target, host, assets, "com.nxnative.t0000000000000001");
        rejected(target.replace("shader_chain\tcacaa37ecb55", "shader_chain\t000000000000"), host, assets, pkg);
        System.out.println("Target contract: host provenance, schema/ABI/backend gates, app identity and asset/module bindings passed");
    }
}
