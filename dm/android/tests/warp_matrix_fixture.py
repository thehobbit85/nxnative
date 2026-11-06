"""Run Ampere warp-cooperative matrix multiply on independently packed PTX fragments."""
from pathlib import Path
import random
import struct
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path[:0] = [str(ROOT), str(ROOT / 'dm/tools/tests'), str(ROOT / 'dm/gfx/tests')]
from dm.android.tests.compute_words_fixture import GUARD, export_cases
from dm.android.tests.ampere_buffer_ops import read_word, record_address
from dm.android.tests.nvn2_layout_fixture import store
from test_dmsh_control_flow import EXIT, assemble, bits
from test_dmsh_hmma import hmma_msl


def half_pair(lo, hi):
    return struct.unpack('<I', struct.pack('<ee', lo, hi))[0]


def float_word(value):
    return struct.unpack('<I', struct.pack('<f', value))[0]


def matrices(width, k, fp32, iterations):
    # Small signed integers keep tensor rounding exact on both hardware paths.
    rng = random.Random(1234 + k)
    inputs, expected = [], []
    for _ in range(width // 32):
        a = [[rng.randrange(-3, 4) for _ in range(k)] for _ in range(16)]
        b = [[rng.randrange(-3, 4) for _ in range(8)] for _ in range(k)]
        c = [[rng.randrange(-5, 6) for _ in range(8)] for _ in range(16)]
        d = [[c[r][n] + iterations * sum(a[r][i] * b[i][n] for i in range(k)) for n in range(8)] for r in range(16)]
        for lane in range(32):
            g, t = divmod(lane, 4)
            av = []
            for reg in range(k // 4):
                row = g + 8 * (reg & 1)
                col = 2 * t + 8 * (reg // 2)
                av.append(half_pair(a[row][col], a[row][col + 1]))
            bv = [half_pair(b[2 * t + 8 * reg][g], b[2 * t + 8 * reg + 1][g]) for reg in range(k // 8)]
            def result(matrix):
                values = [matrix[g + 8 * row][2 * t + j] for row in range(2) for j in range(2)]
                return list(map(float_word, values)) if fp32 else [half_pair(*values[:2]), half_pair(*values[2:])]
            first = av + [GUARD] * (4 - len(av)) + bv + [GUARD] * (2 - len(bv)) + result(c)
            first += [GUARD] * (16 - len(first))
            inputs += first
            answer = first.copy()
            output = result(d)
            answer[10:10 + len(output)] = output
            expected += answer
    return inputs + [GUARD] * 8, expected + [GUARD] * 8


def program(k, fp32, iterations):
    # Each lane owns 16 words: A0..3, B0..1, C0..3, D0..3 and guard words.
    ops = record_address(64)
    for reg, word in ([(16 + i, i) for i in range(k // 4)] + [(20 + i, 4 + i) for i in range(k // 8)]
                      + [(24 + i, 6 + i) for i in range(4 if fp32 else 2)]):
        ops.append(read_word(reg, word))
    for i in range(iterations):
        ops.append(bits((0, 12, 0x23c), (12, 3, 7), (16, 8, 28), (24, 8, 16), (32, 8, 20),
                        (64, 8, 28 if i else 24), (75, 1, k == 16), (76, 1, fp32), (110, 3, 7)))
    ops.extend(store(28 + i, (10 + i) * 4) for i in range(4 if fp32 else 2))
    return assemble(ops + [EXIT])


def generate(work):
    programs, initial = [], {}
    for width in (32, 64, 128):
        for k in (8, 16):
            for fp32 in (False, True):
                for iterations in (1, 2):
                    key = f'{0xfeeda300 + len(programs):016x}'
                    initial[key], expected = matrices(width, k, fp32, iterations)
                    programs.append((key, [width, 1, 1], program(k, fp32, iterations), expected, {'arch': 'sm86'}))
    return export_cases(work, programs, initial_words=initial, metal_transform=hmma_msl)


if __name__ == '__main__':
    generate(Path(sys.argv[1]).resolve())
