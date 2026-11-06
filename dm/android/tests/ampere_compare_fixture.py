"""Exercise all Ampere FSET/FSETP conditions, with and without input flush-to-zero."""
import argparse
import math
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path[:0] = [str(ROOT), str(ROOT / 'dm/tools/tests'), str(ROOT / 'dm/gfx/tests')]
from dm.android.tests.ampere_buffer_ops import read_word, record_address
from dm.android.tests.ampere_integer_fixture import alu, predicate_word
from dm.android.tests.compute_words_fixture import GUARD, export_cases
from dm.android.tests.nvn2_layout_fixture import store
from test_dmsh_control_flow import EXIT, assemble

STRIDE = 64
VALUES = (0, 0x80000000, 1, 0x80000001, 0x007fffff, 0x807fffff, 0x00800000, 0x80800000,
          0x3f800000, 0xbf800000, 0x7f800000, 0xff800000, 0x7fc01234, 0xff803456)


def operand(word, flush):
    if flush and (word & 0x7fffffff) < 0x00800000:
        return 0.0
    return struct.unpack('<f', struct.pack('<I', word))[0]


def compare(condition, a, b):
    if math.isnan(a) or math.isnan(b):
        return bool(condition & 8)
    return (False, a < b, a == b, a <= b, a > b, a != b, a >= b, True)[condition & 7]


def program(conditions, flush):
    ops = record_address(STRIDE * 4) + [read_word(16, 0), read_word(17, 1)]
    for condition in conditions:
        flags = [(76, 4, condition), (80, 1, int(flush)), (87, 3, 7)]
        ops += [alu(0x20a, 18, 16, 17, 0, *flags),
                alu(0x20b, 255, 16, 17, 0, *flags, (81, 3, 0), (84, 3, 1)),
                predicate_word(19, 0), predicate_word(20, 1)]
        word = 2 + 3 * condition
        ops.extend(store(18 + n, (word + n) * 4) for n in range(3))
    return assemble(ops + [EXIT])


def generate(work, common_only):
    cases, initial = [], {}
    for flush in (True, False):
        # Without an explicit FTZ instruction, device-native subnormal behavior
        # belongs to the separate tiny-value probe. Signed zeros are exact here.
        values = [v for v in VALUES if flush or (v & 0x7fffffff) == 0 or (v & 0x7fffffff) >= 0x00800000]
        groups = {}
        for a in values:
            for b in values:
                nan = max(a & 0x7fffffff, b & 0x7fffffff) > 0x7f800000
                # The locked Apple export spells ordered NE as plain MSL !=.
                # The default corpus checks guest semantics for that case too.
                conditions = tuple(c for c in range(16) if not (common_only and nan and c == 5))
                groups.setdefault(conditions, []).append((a, b))
        for conditions, pairs in groups.items():
            binary = program(conditions, flush)
            for start in range(0, len(pairs), 64):
                lanes = pairs[start:start + 64]
                seed, expected = [], []
                for a, b in lanes:
                    row = [a, b] + [GUARD] * (STRIDE - 2)
                    seed.extend(row)
                    for condition in conditions:
                        yes = compare(condition, operand(a, flush), operand(b, flush))
                        offset = 2 + 3 * condition
                        row[offset:offset + 3] = [0x3f800000 if yes else 0, int(yes), int(not yes)]
                    expected.extend(row)
                key = f'{0xfeedbc00 + len(cases):016x}'
                initial[key] = seed + [GUARD] * 8
                cases.append((key, [len(lanes), 1, 1], binary, expected + [GUARD] * 8, {'arch': 'sm86'}))
    export_cases(work, cases, initial_words=initial)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--common-only', action='store_true', help='isolate the cross-backend comparisons outside ordered NE with NaN')
    args = parser.parse_args()
    generate(args.directory.resolve(), args.common_only)
