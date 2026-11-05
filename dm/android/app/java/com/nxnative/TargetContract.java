package com.nxnative;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/** The immutable APK binds its real host producer to the verified asset set. */
final class TargetContract {
    private TargetContract() { }
    private static String hash(byte[] data) { return GameAssets.hex(GameAssets.digest().digest(data)); }
    private static void need(boolean condition, String detail) throws IOException {
        if (!condition) throw new IOException("Incompatible Android target: " + detail + "; rebuild the APK and matching data");
    }
    private static boolean digest(String value) { return value.matches("[0-9a-f]{64}"); }
    private static boolean index(String value) { return value.matches("0|[1-9][0-9]{0,9}") && Long.parseLong(value) <= Integer.MAX_VALUE; }
    private static String root(String index) { return index.equals("0") ? "" : "programs/" + index + "/"; }
    static byte[] entry(ZipFile apk, String name) throws IOException {
        ZipEntry entry = apk.getEntry(name);
        if (entry == null) throw new IOException("APK has no " + name + "; rebuild it with the current Android builder");
        try (java.io.InputStream in = apk.getInputStream(entry); java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream()) {
            byte[] buffer = new byte[1 << 16]; int n;
            while ((n = in.read(buffer)) != -1) out.write(buffer, 0, n);
            return out.toByteArray();
        }
    }
    static void checkApk(File file, GameAssets.Manifest assets, String packageName) throws IOException {
        try (ZipFile apk = new ZipFile(file)) {
            verify(new String(entry(apk, "assets/game/target.tsv"), StandardCharsets.UTF_8),
                   entry(apk, "lib/arm64-v8a/libdmhost.so"), assets, packageName);
        }
    }
    static void verify(String target, byte[] library, GameAssets.Manifest assets, String packageName) throws IOException {
        String binary = new String(library, StandardCharsets.UTF_8);
        String start = "NXNATIVE_ANDROID_HOST_V1\n", end = "NXNATIVE_ANDROID_HOST_END\n";
        int a = binary.indexOf(start), b = binary.indexOf(end);
        need(a >= 0 && b > a && binary.lastIndexOf(start) == a && binary.lastIndexOf(end) == b, "native producer record");
        String producer = binary.substring(a + start.length(), b);
        need(target.startsWith(producer), "native producer differs from target manifest");
        Map<String, String> scalar = new HashMap<>();
        Map<String, String> nativePrograms = new HashMap<>(), programs = new HashMap<>(), shaderExports = new HashMap<>();
        Map<String, String[]> modules = new HashMap<>();
        Set<String> names = new HashSet<>(), inputs = new HashSet<>(), features = new HashSet<>();
        Set<String> known = Set.of("schema", "platform", "arch", "host_abi", "tp_slot", "shader_schema", "shader_chain",
                                 "exporter", "profile", "profile_header", "title", "host_sha256", "shader_export");
        for (String row : target.split("\n")) {
            String[] f = row.split("\t", -1);
            need(f.length >= 2, "manifest row");
            switch (f[0]) {
                case "asset": {
                    need(f.length == 4 && names.add(f[1]), "asset row");
                    GameAssets.Entry asset = assets.entries.get(f[1]);
                    need(asset != null && Long.toString(asset.size).equals(f[2]) && asset.hash.equals(f[3]), "asset " + f[1]);
                    break;
                }
                case "module":
                    need(f.length == 5 && f[1].matches("(?:[1-9][0-9]{0,9}/)?[A-Za-z0-9_-]+") && f[2].matches("[0-9a-f]{16,64}") && digest(f[3]) && digest(f[4]) && modules.put(f[1], f) == null, "module row");
                    break;
                case "program_profile":
                    need(f.length == 3 && index(f[1]) && f[2].matches("[0-9A-F]{16}") && producer.contains(row + "\n") && nativePrograms.put(f[1], f[2]) == null, "native program profile");
                    break;
                case "program":
                    need(f.length == 4 && index(f[1]) && f[2].matches("[0-9A-F]{16}") && f[3].equals(f[1].equals("0") ? "." : "programs/" + f[1]) && programs.put(f[1], f[2]) == null, "program resources");
                    break;
                case "program_shader":
                    need(f.length == 3 && index(f[1]) && !f[1].equals("0") && shaderExports.put(f[1], f[2]) == null, "program shader export");
                    break;
                case "profile_input":
                    need(f.length == 3 && f[1].matches("[0-9A-F]{16}/[A-Za-z0-9_.-]+") && digest(f[2]) && inputs.add(f[1]), "profile input");
                    break;
                case "feature":
                    need(f.length == 2 && f[1].equals("guest-nvn") && features.add(f[1]), "unsupported guest backend");
                    break;
                default:
                    need(f.length == 2 && known.contains(f[0]) && scalar.put(f[0], f[1]) == null, "unknown or duplicate field " + f[0]);
            }
        }
        need(scalar.keySet().equals(known), "missing manifest fields");
        for (String key : new String[]{"schema", "host_abi"}) need("1".equals(scalar.get(key)), key);
        need("2".equals(scalar.get("shader_schema")), "shader_schema");
        need("android".equals(scalar.get("platform")) && "arm64-v8a".equals(scalar.get("arch")) && "2".equals(scalar.get("tp_slot")), "platform or thread ABI");
        need(scalar.get("shader_chain").matches("[0-9a-f]{12}") && digest(scalar.get("exporter")) && digest(scalar.get("profile_header")), "shader/profile producer identity");
        need(scalar.get("profile").matches("[0-9A-F]{16}") && scalar.get("title").matches("[0-9A-F]{16}"), "title/profile identity");
        String pkg = "com.nxnative.t" + scalar.get("title").toLowerCase(java.util.Locale.ROOT);
        need(packageName == null || packageName.equals(pkg) || packageName.equals(pkg + ".test"), "application identity");
        need(hash(library).equals(scalar.get("host_sha256")), "native library hash");
        need(!inputs.isEmpty() && features.size() == 1 && names.equals(assets.entries.keySet()), "profile/features or asset inventory");
        need(programs.equals(nativePrograms), "prepared programs differ from native profiles");
        if (programs.isEmpty()) programs.put("0", scalar.get("profile"));
        need(scalar.get("profile").equals(programs.get("0")), "primary program profile");
        shaderExports.put("0", scalar.get("shader_export"));
        need(shaderExports.keySet().equals(programs.keySet()), "program shader inventory");
        Set<String> binaries = new HashSet<>();
        for (String program : programs.keySet()) {
            String prefix = root(program), modulePrefix = program.equals("0") ? "" : program + "/";
            boolean shaders = names.stream().anyMatch(n -> n.startsWith(prefix + "vk/"));
            need(shaders ? digest(shaderExports.get(program)) : "empty".equals(shaderExports.get(program)), "shader export provenance for program " + program);
            need(modules.containsKey(modulePrefix + "main"), "main module for program " + program);
            for (String name : names) if (name.startsWith(prefix + "dm/") && name.endsWith(".patched.bin"))
                binaries.add(modulePrefix + name.substring(prefix.length() + 3, name.length() - 12));
        }
        for (String name : names) if (name.startsWith("programs/")) need(programs.containsKey(name.split("/")[1]), "undeclared program asset");
        need(binaries.equals(modules.keySet()), "module inventory");
        for (String[] module : modules.values()) {
            String[] parts = module[1].split("/");
            String path = parts.length == 1 ? "dm/" + parts[0] : root(parts[0]) + "dm/" + parts[1];
            GameAssets.Entry code = assets.entries.get(path + ".patched.bin");
            GameAssets.Entry fixups = assets.entries.get(path + ".fixups.tsv");
            need(code != null && fixups != null && module[3].equals(code.hash) && module[4].equals(fixups.hash), "module " + module[1]);
        }
    }
}
