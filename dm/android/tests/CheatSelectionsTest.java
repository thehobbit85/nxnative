package com.nxnative;

import java.io.StringReader;
import java.util.Arrays;
import java.util.List;

public final class CheatSelectionsTest {
    private static void expect(String text, String... expected) throws Exception {
        List<String> actual = CheatSelections.keys(new StringReader(text));
        if (!actual.equals(Arrays.asList(expected)))
            throw new AssertionError("expected " + Arrays.toString(expected) + ", got " + actual);
    }
    public static void main(String[] args) throws Exception {
        // hle_cheats.c uses X for the first [X], X #2 for the second.
        expect("[X]\n04000000 00000000 00000001\n[X]\n04000000 00000000 00000002\n", "X", "X #2");
        // Masters and labels still take an ordinal, even though they have no toggle.
        expect("{X}\n04000000 00000000 00000001\n[X]\n[X]\n04000000 00000000 00000002\n", "X #3");
        expect("[Inline]04000000 00000000 00000001\n[Packed]\n040000000000000000000002\n", "Inline", "Packed");
        expect("[X]\n04000000 00000000 00000001[X]\n04000000 00000000 00000002\n", "X", "X #2");
        String longName = "a".repeat(127);
        expect("[" + longName + "one]\n04000000 00000000 00000001\n[" + longName + "two]\n04000000 00000000 00000002\n", longName, longName + " #2");
        String unicodeName = "a".repeat(126) + "\u00e9";
        expect("[" + unicodeName + "]\n04000000 00000000 00000001\n", "a".repeat(126));
        System.out.println("cheat selection keys: PASS");
    }
}
