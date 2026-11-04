package com.nxnative;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.util.HashMap;
import java.util.Map;

/** Asset paths and streaming data imports, independent of the Android UI. */
final class GameAssets {
    private GameAssets() { }

    static File path(File root, String name) throws IOException {
        if (!name.matches("(?:dm|romfs|vk|font|aoc)/.+") || name.contains("\\"))
            throw new IOException("Invalid game asset path: " + name);
        File result = new File(root, name).getCanonicalFile();
        if (!result.getPath().startsWith(root.getCanonicalPath() + File.separator))
            throw new IOException("Game asset leaves its storage folder: " + name);
        return result;
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
        private final byte[] buffer = new byte[1 << 20];

        DataImport(File root, Map<String, Long> expected) {
            this.root = root; left = new HashMap<>(expected);
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
                    out.write(buffer, 0, n); end += n;
                }
            }
            if (split < 0 && end != size) throw new IOException("Incomplete game asset: " + name);
            written.put(name, end);
            if (end == size) left.remove(name);
            return end - offset;
        }
    }
}
