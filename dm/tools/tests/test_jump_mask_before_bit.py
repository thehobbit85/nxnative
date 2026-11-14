"""Manual Samuel's mask-before-bit-test table, using real ARM64 and NRO rewriting."""
import ast
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
from typing import Any
import unittest

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
from nso_asm import ADRP, ADDI, ADDR, B, BR, NOP

RET = 0xd65f03c0


def mask(dst, src, bit): return 0x12000000 | bit << 10 | src << 5 | dst
def test_bit(src, bit, at, to): return 0x37000000 | bit << 19 | (((to - at) // 4) & 0x3fff) << 5 | src
def load(dst, base, idx): return 0xb8a07800 | idx << 16 | base << 5 | dst
def mov(dst, src, wide=False): return (0xaa0003e0 if wide else 0x2a0003e0) | src << 16 | dst


class MaskBeforeBit(unittest.TestCase):
    TABLE = 0x20d8
    TARGETS = [0, 112, 116, 120]

    def setUp(self):
        self.img = bytearray(0x4000)
        for i, target in enumerate(self.TARGETS): struct.pack_into('<i', self.img, self.TABLE + i * 4, target - self.TABLE)
        struct.pack_into('<i', self.img, self.TABLE + 16, 0x1000)  # no fifth target
        self.scope: dict[str, Any] = dict(re=re, struct=struct, IMG=self.img, tabjumps={}, jumps={})
        tree = ast.parse((TOOLS / 'patch_nso.py').read_text())
        names = {'num', 'written_w', 'writes_reg', 'jump_mask_before_bit'}
        exec(compile(ast.Module(body=[n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in names], type_ignores=[]), str(TOOLS / 'patch_nso.py'), 'exec'), self.scope)

    def words(self, bit=2):
        # The issue's tail has three register writes after ADD, and its base is far back.
        return [ADRP(14, 0, self.TABLE), ADDI(14, 14, 0xd8)] + [NOP] * 17 + [
            mask(13, 17, bit), test_bit(17, bit, 80, 120), load(13, 14, 13), ADDR(18, 13, 14),
            mov(11, 31), 0x52800031, mov(13, 12), BR(18), RET, RET, RET, RET]

    def match(self, words):
        md = Cs(CS_ARCH_ARM64, CS_MODE_ARM); md.detail = True
        ins = [p for i, word in enumerate(words) for p in md.disasm(struct.pack('<I', word), 4 * i)]
        j = next(i for i, p in enumerate(ins) if p.mnemonic == 'br' and p.op_str == 'x18')
        fn = self.scope.get('jump_mask_before_bit')
        found = fn(ins, j) if fn else None
        return found[0] if found else None

    def test_mask_before_test_reads_four_entries_not_eight(self):
        self.assertEqual(self.match(self.words()), self.TARGETS)
        self.assertEqual(self.match(self.words(bit=0)), [0])
        self.assertEqual(self.match(self.words(bit=1)), [0, 112])

    def test_branch_and_mask_must_bound_the_same_value(self):
        for slot, word in [(19, mask(13, 16, 2)), (19, mask(13, 17, 3)), (19, mask(12, 17, 2)),
                           (20, test_bit(17, 1, 80, 120)), (20, test_bit(17, 2, 80, 120) ^ 0x01000000)]:
            with self.subTest(slot=slot, word=hex(word)):
                words = self.words(); words[slot] = word
                self.assertIsNone(self.match(words))

    def test_base_and_loaded_offset_must_reach_the_add_unchanged(self):
        for slot, word in [(2, mov(14, 0)), (21, load(14, 14, 13)), (22, ADDR(18, 13, 15)), (25, mov(18, 12))]:
            words = self.words(); words[slot] = word
            self.assertIsNone(self.match(words))
        for slot, word in [(21, mov(13, 0)), (22, mov(13, 0)), (22, mov(14, 0)), (22, 0x94000000)]:
            words = self.words(); words.insert(slot, word)
            self.assertIsNone(self.match(words))

    def test_gaps_calls_and_control_flow_break_base_provenance(self):
        for word in (0xe7ffdefe, B(8, 12), 0x94000000, 0xd4000001, mov(14, 0), ADDI(14, 14, 1) & 0x7fffffff):
            words = self.words(); words[2] = word
            self.assertIsNone(self.match(words))
        words = self.words(); words[2] = 0xbd4003ee  # ldr s14,[sp], not a write of x14
        self.assertEqual(self.match(words), self.TARGETS)

    def test_incoming_edges_cannot_skip_the_base_or_guard(self):
        for target in (4, 8, 76, 80, 84, 88, 104):
            words = self.words(); words[-1] = B(120, target)
            self.assertIsNone(self.match(words))
            self.scope['tabjumps'] = {0x1000: [target]}
            self.assertIsNone(self.match(self.words()))
            self.scope['tabjumps'] = {}
        for target in (4, 80, 84):
            struct.pack_into('<i', self.img, self.TABLE, target - self.TABLE)
            self.assertIsNone(self.match(self.words()))

    def test_table_bytes_and_instruction_targets_must_exist(self):
        struct.pack_into('<i', self.img, self.TABLE, 113 - self.TABLE)
        self.assertIsNone(self.match(self.words()))
        del self.img[self.TABLE + 15:]
        self.assertIsNone(self.match(self.words()))

    def rewrite(self, incoming=None, later_table=False, older=False):
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        from test_nso_match import image
        words = self.words(); words[0] = ADRP(14, 0x100, self.TABLE)
        if older: words[19:21] = [0x71000dbf, 0x54000148]  # cmp w13,#3; b.hi +40
        functions = {0x100: words}
        if incoming is not None: functions[0x200] = [B(0x200, incoming), RET]
        if later_table: functions[0x200] = [ADRP(10, 0x200, 0x2080), ADDI(10, 10, 0x80), 0x710001bf,
                                           0x54000088, load(13, 10, 13), ADDR(18, 13, 10), BR(18), RET]
        raw = image(functions)
        for i, target in enumerate([0x100, 0x170, 0x174, 0x178]): struct.pack_into('<i', raw, self.TABLE + i * 4, target - self.TABLE)
        struct.pack_into('<i', raw, self.TABLE + 16, 0x1000)
        if later_table: struct.pack_into('<i', raw, 0x2080, 0x150 - 0x2080)
        raw[0x1100:0x111c] = raw[0x20:0x3c]; struct.pack_into('<I', raw, 4, 0x1100)
        struct.pack_into('<ii', raw, 0x1110, 0x3000 - 0x1100, 0x300c + 8 * len(functions) - 0x1100)
        raw[0x10:0x14] = b'NRO0'; struct.pack_into('<I', raw, 0x18, len(raw))
        struct.pack_into('<6I', raw, 0x20, 0, 0x1000, 0x1000, 0x2000, 0x3000, 0x1000); struct.pack_into('<I', raw, 0x38, 0)
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / 'input.nro'; src.write_bytes(raw)
            run = subprocess.run([sys.executable, str(TOOLS / 'patch_nso.py'), str(src), tmp, 'nro0'],
                                 env=os.environ | {'DM_GAME': '010015100B514000', 'DM_REWRITE_J': '1'}, capture_output=True, text=True)
            path = Path(tmp) / 'nro0.sites.tsv'
            rows = [line.split('\t') for line in path.read_text().splitlines()] if path.exists() else []
        return run, [[int(t, 0) for t in row[2].split()] for row in rows if row[1] == 'T']

    def test_complete_rewriter_emits_only_the_four_proven_targets(self):
        for older in (False, True):
            run, targets = self.rewrite(older=older)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertEqual(targets, [[0x100, 0x170, 0x174, 0x178]])

    def test_complete_rewriter_rejects_later_table_and_cross_function_entries(self):
        for run, _ in (self.rewrite(incoming=0x150), self.rewrite(later_table=True)):
            self.assertNotEqual(run.returncode, 0)
            self.assertIn('bypasses mask-before-bit proof', run.stderr)
        run, targets = self.rewrite(incoming=0x100)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertEqual(targets, [[0x100, 0x170, 0x174, 0x178]])


if __name__ == '__main__': unittest.main()
