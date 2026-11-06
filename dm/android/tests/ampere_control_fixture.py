"""Check lane-dependent branches, loops and return paths in translated Ampere programs."""
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path[:0] = [str(ROOT), str(ROOT / 'dm/tools/tests'), str(ROOT / 'dm/gfx/tests')]
from dm.android.tests.ampere_buffer_ops import read_word, record_address
from dm.android.tests.ampere_integer_fixture import alu
from dm.android.tests.compute_words_fixture import GUARD, export_cases
from dm.android.tests.nvn2_layout_fixture import store
from test_dmsh_control_flow import EXIT, SIZE, assemble, bits, bra, branch, ldc


def move(rd, value):
    return bits((0, 12, 0x802), (12, 3, 7), (16, 8, rd), (32, 32, value), (110, 3, 7))


def compare(pred, ra, rb, op):
    return alu(0x20c, 255, ra, rb, 0, (76, 3, op), (81, 3, pred), (84, 3, 7), (87, 3, 7))


def add(rd, a, b):
    return alu(0x210, rd, a, b, 255, (81, 3, 7), (84, 3, 7))


def loop():
    # while (i < count) sum += i + 1. Counts include zero and vary by lane.
    ops = record_address(16) + [read_word(16, 0), move(17, 0), move(18, 0), move(19, 1)]
    head = len(ops)
    ops += [compare(0, 17, 16, 6), bra(head + 1, head + 5, pred=0),
            add(17, 17, 19), add(18, 18, 17), bra(head + 4, head),
            store(18, 4), store(17, 8), EXIT]
    return assemble(ops)


def joined():
    # Forward edges converge at two different points. The last case returns
    # after its first store and must leave the second output untouched.
    ops = record_address(16) + [read_word(16, 0), move(17, 3), move(18, 7),
                                compare(0, 16, 17, 1), compare(1, 16, 18, 1)]
    k = len(ops)
    ops += [bra(k, k + 5, pred=0), bra(k + 1, k + 7, pred=1), move(20, 13),
            store(20, 4), EXIT, move(20, 31), bra(k + 6, k + 8), move(20, 71),
            store(20, 4), add(21, 20, 16), store(21, 8), EXIT]
    return assemble(ops)


def table():
    # The fourth c1 word points outside the shader and bounds the jump table.
    ops = record_address(16) + [read_word(16, 0), alu(0x824, 12, 16, 4, 255, (81, 3, 7)),
                                ldc(1, 12, 0), branch(0x949, 0, ra=1)]
    anchor = len(ops)
    for i, value in enumerate((117, 255, 143)):
        ops.extend((move(20, value), bra(anchor + 2 * i + 1, anchor + 6)))
    return assemble(ops + [store(20, 4), EXIT])


def generate(work):
    inputs = [(i * 13) % 23 for i in range(96)]
    seed = [word for n in inputs for word in (n, GUARD, GUARD, GUARD)] + [GUARD] * 8
    cases, initial = [], {}
    for index, program in enumerate((loop, joined)):
        words = []
        for n in inputs:
            if program == loop:
                first, second = n * (n + 1) // 2, n
            else:
                first = 31 if n < 3 else 71 if n < 7 else 13
                second = n + first if n < 7 else GUARD
            words.extend((n, first, second, GUARD))
        key = f'{0xfeeda910 + index:016x}'
        cases.append((key, [len(inputs), 1, 1], program(), words + [GUARD] * 8, {'arch': 'sm86'}))
        initial[key] = seed
    key = '00000000feeda912'
    indices = [i % 3 for i in inputs]
    constants = struct.pack('<4i', 0, 2 * SIZE, 4 * SIZE, 0x100000)
    initial[key] = [word for index in indices for word in (index, GUARD, GUARD, GUARD)] + [GUARD] * 8
    words = [word for index in indices for word in (index, (117, 255, 143)[index], GUARD, GUARD)] + [GUARD] * 8
    cases.append((key, [len(indices), 1, 1], table(), words, {'arch': 'sm86', 'c1': constants.hex()}))
    export_cases(work, cases, initial_words=initial)


if __name__ == '__main__':
    generate(Path(sys.argv[1]).resolve())
