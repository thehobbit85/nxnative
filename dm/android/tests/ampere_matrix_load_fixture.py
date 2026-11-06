"""Check Ampere matrix loads and register transpose against ordinary 8×8 matrices."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path[:0] = [str(ROOT), str(ROOT / 'dm/tools/tests'), str(ROOT / 'dm/gfx/tests')]
from dm.android.tests.ampere_buffer_ops import read_word, record_address
from dm.android.tests.compute_words_fixture import GUARD, export_cases
from dm.android.tests.nvn2_layout_fixture import store
from test_dmsh_control_flow import EXIT, assemble, bits


def program(count, transpose):
    # Each thread supplies an entire eight-halfword row in shared memory. The
    # warp's first 8, 16 or 32 lanes supply the addresses used by LDSM.
    ops = record_address(48)
    for word in range(4):
        ops += [read_word(16 + word, word),
                bits((0, 12, 0x388), (12, 3, 7), (24, 8, 9), (32, 8, 16 + word), (40, 24, 4 * word),
                     (73, 3, 4), (78, 2, 3), (110, 3, 7))]  # STS [tid * 16 + offset]
    ops += [bits((0, 12, 0xb1d), (12, 3, 7), (110, 3, 7)),
            bits((0, 12, 0x824), (12, 3, 7), (16, 8, 12), (24, 8, 9), (32, 32, 16),
                 (64, 8, 255), (81, 3, 7), (110, 3, 7)),  # IMAD R12, R9, 16, RZ
            bits((0, 12, 0x83b), (12, 3, 7), (16, 8, 20), (24, 8, 12), (72, 2, count.bit_length()-1),
                 (78, 2, transpose), (110, 3, 7))]
    ops += [store(20 + matrix, (4 + matrix) * 4) for matrix in range(count)]
    for src, dst in ((20, 24), (24, 25)):
        ops += [bits((0, 12, 0x23a), (12, 3, 7), (16, 8, dst), (24, 8, src), (110, 3, 7)),
                store(dst, (dst - 16) * 4)]
    return assemble(ops + [EXIT])


def words(width, count, transpose):
    seed, expected = [], []
    for warp in range(width // 32):
        matrices = [[[((0x8203 + (256*warp + 64*m + 8*r + c)*97) & 0xffff) for c in range(8)]
                     for r in range(8)] for m in range(4)]
        def pack(a, b):
            return a | (b << 16)
        for lane in range(32):
            source = matrices[lane // 8][lane % 8]
            record = [pack(source[c], source[c+1]) for c in range(0, 8, 2)] + [GUARD] * 8
            seed += record
            output = record.copy()
            row, col = lane // 4, 2 * (lane % 4)
            def pair(m, transposed):
                return pack(m[col][row], m[col+1][row]) if transposed else pack(m[row][col], m[row][col+1])
            output[4:4+count] = [pair(m, transpose) for m in matrices[:count]]
            output[8] = pair(matrices[0], not transpose)
            output[9] = pair(matrices[0], transpose)
            expected += output
    return seed + [GUARD]*8, expected + [GUARD]*8


def generate(work):
    programs, initial = [], {}
    for width in (32, 64, 128):
        for count in (1, 2, 4):
            for transpose in (False, True):
                key = f'{0xfeedb400 + len(programs):016x}'
                initial[key], expected = words(width, count, transpose)
                programs.append((key, [width, 1, 1], program(count, transpose), expected, dict(arch='sm86', shared=width*16)))
    export_cases(work, programs, initial_words=initial)


if __name__ == '__main__':
    generate(Path(sys.argv[1]).resolve())
