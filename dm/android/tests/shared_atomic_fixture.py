"""Cross-warp shared loads, stores, barriers and returning Maxwell atomics."""
import argparse
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path[:0] = [str(ROOT), str(ROOT / 'dm/tools/tests'), str(ROOT / 'dm/gfx/tests')]
from dm.android.tests.compute_words_fixture import GUARD, export_cases
from dm.android.tests.nvn2_layout_fixture import load, store
from dm.android.tests.warp_vote_fixture import PT, store_pair
from test_dmsh_ampere_mem import Shared, EXIT, inst, mov as ampere_mov, sts
from test_dmsh_ampere_alu import waiting
from test_dmsh_control_flow import bits
from test_txq_type import base, mov, shader

SEED = 0x81234690
OPERATIONS = ((0, False), (1, False), (2, False), (1, True), (2, True),
              (5, False), (6, False), (7, False), (8, False))


def result(op, signed, old, operand):
    if op == 0:
        return (old + operand) & 0xffffffff
    if op in (1, 2):
        def order(value):
            return value - (1 << 32) if signed and value >> 31 else value
        return (min if op == 1 else max)((old, operand), key=order)
    return {5: old & operand, 6: old | operand, 7: old ^ operand, 8: operand}[op]


def case(width, op, signed, operand):
    # Each lane initializes a word. After the first barrier it updates a
    # different warp's word. The second barrier exposes that update to its owner.
    def shift(dest, src):
        return base('Shl') | (15 << 20) | PT | (src << 8) | dest

    peer_bit = width // 2
    sync = base('Bar') | (1 << 44) | (1 << 43) | (7 << 39) | PT
    ops = [base('S2r') | (33 << 20) | PT | 14, mov(15, 2), shift(2, 14),
           mov(10, SEED), base('Iadd') | (10 << 20) | PT | (14 << 8) | 4,
           base('Sts') | (4 << 48) | PT | (2 << 8) | 4, sync,
           base('Lop32i') | (2 << 53) | (peer_bit << 20) | PT | (14 << 8) | 3,
           shift(3, 3), mov(6, operand),
           base('Atoms') | (op << 52) | (int(signed) << 28) | (6 << 20) | PT | (3 << 8) | 8,
           sync, base('Lds') | (4 << 48) | PT | (2 << 8) | 9]
    words = []
    for tid in range(width):
        words.extend((SEED + (tid ^ peer_bit), result(op, signed, SEED + tid, operand)))
    return shader(ops + store_pair()), words + [GUARD] * 8


def contended_add(width):
    # Each lane must receive a unique old value from the same counter. Use
    # the returned ticket as the destination, so the result is independent
    # of the order the GPU chooses for contending lanes.
    sync = base('Bar') | (1 << 44) | (1 << 43) | (7 << 39) | PT
    ops = [base('S2r') | (33 << 20) | PT | 14, mov(15, 2),
           base('Shl') | (15 << 20) | PT | (14 << 8) | 2,
           mov(4, GUARD), base('Sts') | (4 << 48) | PT | (2 << 8) | 4,
           base('Isetp') | (2 << 49) | (7 << 39) | (255 << 20) | PT | (14 << 8) | 7,
           base('Sts') | (4 << 48) | ((width + 1) * 4 << 20) | (255 << 8) | 255,  # @P0, lane zero
           sync, mov(6, 1), base('Atoms') | ((width + 1) << 30) | (6 << 20) | PT | (255 << 8) | 8,
           mov(10, SEED), base('Iadd') | (10 << 20) | PT | (8 << 8) | 4,
           base('Shl') | (15 << 20) | PT | (8 << 8) | 3,
           base('Sts') | (4 << 48) | PT | (3 << 8) | 4, sync,
           base('Lds') | (4 << 48) | PT | (2 << 8) | 8,
           base('Lds') | (4 << 48) | ((width + 1) * 4 << 20) | PT | (255 << 8) | 9]
    words = [value for tid in range(width) for value in (SEED + tid, width)]
    return shader(ops + store_pair()), words + [GUARD] * 8


def generate(work):
    programs = []
    for width in (64, 128):
        for op, signed in OPERATIONS:
            for operand in (0x1a50a539, SEED + width // 2):
                binary, words = case(width, op, signed, operand)
                key = f'{0xfeed9f00 + len(programs):016x}'
                programs.append((key, [width, 1, 1], binary, words, {'shared': width * 4}))
    # The Apple compiler implements Ampere shared CAS as a success flag.
    # Compare both that flag and the resulting word, including equal old/new.
    for seed, expected, new in ((5, 5, 9), (5, 6, 9), (0, 0, 0), (7, 8, 7),
                                (0xffffffff, 0xffffffff, 0), (3, 0, 0xffffffff)):
        load_result = bits((0, 12, 0x984), (12, 3, 7), (16, 8, 3), (24, 8, 255),
                           (40, 24, 4), (73, 3, 4), (110, 3, 7))
        binary = b''.join((ampere_mov(2, seed), sts(4, 2), ampere_mov(0, expected),
                           ampere_mov(1, new), inst(*Shared.ATOMS_CAS), inst(*load_result),
                           inst(*load(4, 3, 56, 5)), inst(*waiting(store(1, 0))), inst(*store(3, 4)), inst(*EXIT)))
        swapped = seed == expected
        words = [int(swapped), new if swapped else seed] + [GUARD] * 8
        key = f'{0xfeed9f00 + len(programs):016x}'
        programs.append((key, [1, 1, 1], binary, words, {'arch': 'sm86', 'shared': 8}))
    for width in (36, 64, 128):
        binary, words = contended_add(width)
        key = f'{0xfeed9f00 + len(programs):016x}'
        programs.append((key, [width, 1, 1], binary, words, {'shared': (width + 2) * 4}))
    return export_cases(work, programs)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    print(generate(parser.parse_args().directory.resolve()))
