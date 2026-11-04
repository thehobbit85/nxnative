package com.nxnative;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.security.DigestInputStream;
import java.security.MessageDigest;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.Map;

/** Asset paths and streaming data imports, independent of the Android UI. */
final class GameAssets {
    private GameAssets() { }

    private static void checkName(String name) throws IOException {
        if (!name.matches("(?:programs/[1-9][0-9]{0,9}/)?(?:dm|romfs|vk|font|aoc|html)/.+") || name.contains("\\"))
            throw new IOException("Invalid game asset path: " + name);
        for (String part : name.split("/", -1)) if (part.isEmpty() || part.equals(".") || part.equals(".."))
            throw new IOException("Invalid game asset path: " + name);
    }
    static File path(File root, String name) throws IOException {
        checkName(name);
        File result = new File(root, name).getCanonicalFile();
        if (!result.getPath().startsWith(root.getCanonicalPath() + File.separator))
            throw new IOException("Game asset leaves its storage folder: " + name);
        return result;
    }

    static MessageDigest digest() {
        try { return MessageDigest.getInstance("SHA-256"); }
        catch (java.security.NoSuchAlgorithmException e) { throw new AssertionError(e); }
    }
    static String hex(byte[] bytes) {
        StringBuilder result = new StringBuilder(bytes.length * 2);
        for (byte b : bytes) { result.append(Character.forDigit((b & 255) >> 4, 16)); result.append(Character.forDigit(b & 15, 16)); }
        return result.toString();
    }
    static final class Entry {
        final String name, hash;
        final String[] pieces;
        final long size;
        final boolean data;
        Entry(String[] fields) throws IOException {
            if (fields.length != 4) throw new IOException("Invalid game asset manifest row");
            name = fields[0]; checkName(name); hash = fields[3];
            try { size = Long.parseLong(fields[1]); }
            catch (NumberFormatException e) { throw new IOException("Invalid game asset size", e); }
            if (size < 0 || !hash.matches("[0-9a-f]{64}")) throw new IOException("Invalid game asset size or digest");
            data = fields[2].equals("=d");
            if (!data && !fields[2].isEmpty() && !fields[2].matches("[0-9a-f]{24}( [0-9a-f]{24})*"))
                throw new IOException("Invalid game asset pieces");
            pieces = data || fields[2].isEmpty() ? new String[0] : fields[2].split(" ");
            if (!data && size != 0 && pieces.length == 0) throw new IOException("Missing game asset pieces");
        }
    }
    static final class Manifest {
        final Map<String, Entry> entries = new LinkedHashMap<>();
        String identity;
        long total;
        static Manifest read(InputStream input) throws IOException {
            Manifest result = new Manifest(); MessageDigest hash = digest();
            try (BufferedReader reader = new BufferedReader(new InputStreamReader(new DigestInputStream(input, hash), StandardCharsets.UTF_8))) {
                String line;
                while ((line = reader.readLine()) != null) {
                    Entry entry = new Entry(line.split("\t", -1));
                    if (result.entries.put(entry.name, entry) != null) throw new IOException("Duplicate game asset: " + entry.name);
                    if (entry.size > Long.MAX_VALUE - result.total) throw new IOException("Game asset sizes overflow");
                    result.total += entry.size;
                }
            }
            result.identity = hex(hash.digest());
            return result;
        }
    }

    static void finishRomfs(File root, boolean image) throws IOException {
        File previous = path(root, "romfs/romfs.img");
        if (!image && previous.exists() && !previous.delete())
            throw new IOException("Could not remove the previous RomFS image");
    }

    static final class DataImport {
        private final File root;
        private final Map<String, Long> left;
        private final Map<String, Long> written = new HashMap<>();
        private final Map<String, String> hashes;
        private final Map<String, MessageDigest> digests = new HashMap<>();
        private final byte[] buffer = new byte[1 << 20];

        DataImport(File root, Map<String, Long> expected, Map<String, String> hashes) {
            if (!expected.keySet().equals(hashes.keySet())) throw new IllegalArgumentException("Every game asset needs a digest");
            this.root = root; left = new HashMap<>(expected); this.hashes = new HashMap<>(hashes);
        }

        boolean complete() { return left.isEmpty(); }

        long copy(String entry, InputStream in) throws IOException {
            int split = entry.lastIndexOf(".nxpart/");
            String name = split < 0 ? entry : entry.substring(0, split);
            Long size = left.get(name);
            if (size == null) return 0; // Unlisted entries cannot write files.
            long offset = 0;
            if (split >= 0) {
                try { offset = Long.parseLong(entry.substring(split + 8), 16); }
                catch (NumberFormatException e) { throw new IOException("Invalid data chunk offset", e); }
            }
            if (offset < 0 || offset != written.getOrDefault(name, 0L))
                throw new IOException("Out-of-order data chunk: " + name);
            File to = path(root, name); to.getParentFile().mkdirs();
            long end = offset;
            try (RandomAccessFile out = new RandomAccessFile(to, "rw")) {
                if (offset == 0) out.setLength(0);
                out.seek(offset);
                int n;
                while ((n = in.read(buffer)) != -1) {
                    if (n > size - end) throw new IOException("Oversized game asset: " + name);
                    out.write(buffer, 0, n); digests.computeIfAbsent(name, key -> digest()).update(buffer, 0, n); end += n;
                }
            }
            if (split < 0 && end != size) throw new IOException("Incomplete game asset: " + name);
            written.put(name, end);
            if (end == size) {
                String actual = hex(digests.computeIfAbsent(name, key -> digest()).digest());
                if (!actual.equals(hashes.get(name))) throw new IOException("Game asset digest differs: " + name);
                left.remove(name); digests.remove(name);
            }
            return end - offset;
        }
    }
}
