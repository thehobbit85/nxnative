"""Execute the numeric-operand NaN behavior shared by guest min, max and saturation."""
import argparse
import ast
from pathlib import Path
import re
import struct
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path[:0] = [str(ROOT), str(ROOT / 'dm/tools/tests'), str(ROOT / 'dm/gfx/tests')]
from dm.android.tests.ampere_buffer_ops import read_word, record_address
from dm.android.tests.ampere_integer_fixture import alu
from dm.android.tests.compute_words_fixture import GUARD, export_cases
from dm.android.tests.nvn2_layout_fixture import store
from test_dmsh_control_flow import EXIT, assemble


def apple_precise():
    # dmsh_build is a script. Run the same source transformation it applies
    # before compiling Metal, without starting its command-line build.
    path = ROOT / 'dm/gfx/tools/dmsh_build.py'
    function = next(f for f in ast.parse(path.read_text()).body if isinstance(f, ast.FunctionDef) and f.name == 'precise')
    ns = {'re': re}
    exec(compile(ast.Module(body=[function], type_ignores=[]), str(path), 'exec'), ns)
    return ns['precise']


def number(word):
    return struct.unpack('<f', struct.pack('<I', word))[0]


def minmax_program():
    ops = record_address(16) + [read_word(16, 0), read_word(17, 1),
                                alu(0x209, 18, 16, 17, 0, (87, 3, 7)),
                                alu(0x209, 19, 16, 17, 0, (87, 3, 7), (90, 1, 1)),
                                store(18, 8), store(19, 12), EXIT]
    return assemble(ops)


def tiny_inputs(work, preserve):
    # Independent native M2 Metal checks show the compare sees zeros but
    # returns the second register's original bits. ARM64 Lavapipe orders the
    # subnormal numbers. On both devices an equal zero selects the RHS.
    values = (0, 0x80000000, 1, 0x80000001, 0x007fffff, 0x807fffff, 0x00400000, 0x80400000)
    seed, expected = [], []
    for a in values:
        for b in values:
            lo = hi = b
            if preserve and number(a) != number(b):
                lo, hi = (a, b) if number(a) < number(b) else (b, a)
            seed.extend((a, b, GUARD, GUARD))
            expected.extend((a, b, lo, hi))
    key = '00000000feedbb00'
    case = (key, [64, 1, 1], minmax_program(), expected + [GUARD] * 8, {'arch': 'sm86'})
    export_cases(work, [case], initial_words={key: seed + [GUARD] * 8}, metal_transform=apple_precise())


def generate(work):
    values = (0x00000000, 0x80000000, 1, 0x807fffff, 0x00800000, 0x80800000, 0x3e800000,
              0xbf000000, 0x3f800000, 0xc0000000, 0x7f800000, 0xff800000,
              0x7fc01234, 0xffc02345, 0x7f804567, 0xff803456)
    pairs = []
    for a in values:
        for b in values:
            ma, mb = a & 0x7fffffff, b & 0x7fffffff
            if min(ma, mb) > 0x7f800000 or (max(ma, mb) < 0x00800000 and a != b):
                # Two NaNs have no single required payload. Metal can compare
                # subnormals as zero; equal zero operands may have either sign.
                continue
            if ma > 0x7f800000:
                lo = hi = b
            elif mb > 0x7f800000:
                lo = hi = a
            elif number(a) <= number(b):
                lo, hi = a, b
            else:
                lo, hi = b, a
            pairs.append((a, b, lo, hi))
    binary = minmax_program()
    cases, initial = [], {}
    for i in range(0, len(pairs), 64):
        chunk = pairs[i:i + 64]
        key = f'{0xfeedb200 + i // 64:016x}'
        initial[key] = [w for a, b, _, _ in chunk for w in (a, b, GUARD, GUARD)] + [GUARD] * 8
        expected = [w for row in chunk for w in row] + [GUARD] * 8
        cases.append((key, [len(chunk), 1, 1], binary, expected, {'arch': 'sm86'}))
    # FADD.SAT of A + RZ exercises the shared compiler's floating clamp. The
    # lighting case needs NaN to saturate to zero. All other finite inputs are
    # either exactly representable normal values or outside the clamp interval.
    clamp = record_address(16) + [read_word(16, 0), alu(0x221, 17, 16, 255, 0, (77, 1, 1)), store(17, 4), EXIT]
    key = '00000000feedb210'
    inputs = [v for v in values if v == 0 or (v & 0x7fffffff) >= 0x00800000]
    initial[key] = [w for v in inputs for w in (v, GUARD, GUARD, GUARD)] + [GUARD] * 8
    expected = []
    for word in inputs:
        value = number(word)
        result = 0
        if value >= 1:
            result = 0x3f800000
        elif value > 0:
            result = word
        expected.extend((word, result, GUARD, GUARD))
    cases.append((key, [len(inputs), 1, 1], assemble(clamp), expected + [GUARD] * 8, {'arch': 'sm86'}))
    export_cases(work, cases, initial_words=initial, metal_transform=apple_precise())


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--tiny', choices=('preserve', 'compare-as-zero'),
                        help='check the device-specific min/max model for pairs of zeros and subnormals')
    args = parser.parse_args()
    if args.tiny:
        tiny_inputs(args.directory.resolve(), args.tiny == 'preserve')
    else:
        generate(args.directory.resolve())
