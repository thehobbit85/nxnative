package com.nxnative;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.Reader;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

// Menu entries for the build's bundled cheat file. No Android dependency so the
// name-to-host-state-file contract can be tested with the same JDK as the packer.
final class CheatSelections {
    static List<String> keys(Reader input) throws IOException {
        List<String> out = new ArrayList<>();
        Map<String, Integer> counts = new HashMap<>();
        BufferedReader r = new BufferedReader(input);
        String cur = null; boolean code = false; String line; int headers = 0;
        while ((line = r.readLine()) != null) {
            // Match the host's split of a header glued to a complete code word.
            for (String part : line.replaceAll("(?i)(^|[ \\t\\r])([0-9a-f]{8})(?=[\\[{])", "$1$2\n").split("\n")) {
                String l = part.trim();
                if (l.startsWith("[") || l.startsWith("{")) {
                    if (cur != null && code) out.add(cur);
                    cur = null; code = false;
                    boolean master = l.startsWith("{");
                    int close = l.indexOf(master ? '}' : ']');
                    if (close < 2 || headers >= 256) continue;
                    headers++;
                    // hle_cheats.c retains at most 127 UTF-8 bytes, at a character boundary.
                    byte[] bytes = l.substring(1, close).getBytes(StandardCharsets.UTF_8);
                    int keep = Math.min(127, bytes.length);
                    if (keep < bytes.length) while (keep > 0 && (bytes[keep] & 0xc0) == 0x80) keep--;
                    String name = new String(bytes, 0, keep, StandardCharsets.UTF_8);
                    int count = counts.getOrDefault(name, 0) + 1;
                    counts.put(name, count); // Masters and labels also count in cheat_key().
                    if (!master) cur = name + (count > 1 ? " #" + count : "");
                    l = l.substring(close + 1).trim();
                }
                if (l.matches("(?i)(?:[0-9a-f]{8})+(?:\\s+(?:[0-9a-f]{8})+)*")) code = true;
            }
        }
        if (cur != null && code) out.add(cur);
        return out;
    }
}
