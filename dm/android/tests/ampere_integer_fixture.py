"""Compare guest IADD3 and IMAD results and carry predicates against full-width Python integers."""
from pathlib import Path
import random
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path[:0] = [str(ROOT), str(ROOT / 'dm/tools/tests'), str(ROOT / 'dm/gfx/tests')]
from dm.android.tests.ampere_buffer_ops import read_word, record_address
from dm.android.tests.compute_words_fixture import GUARD, export_cases
from dm.android.tests.nvn2_layout_fixture import store
from test_dmsh_control_flow import EXIT, assemble, bits

U32 = (1 << 32) - 1
U64 = (1 << 64) - 1


def alu(op, rd, ra, rb, rc, *flags):
    return bits((0, 12, op), (12, 3, 7), (16, 8, rd), (24, 8, ra), (32, 8, rb), (64, 8, rc), (110, 3, 7), *flags)


def predicate_word(rd, pred):
    # IMAD.X Rd, RZ, RZ, RZ, Pn materializes the predicate as 0 or 1.
    return alu(0x224, rd, 255, 255, 255, (74, 1, 1), (81, 3, 7), (87, 3, pred))


def integer_program(signed):
    ops = record_address(64) + [read_word(16 + i, i) for i in range(4)]
    ops += [alu(0x210, 30, 16, 17, 18, (81, 3, 0), (84, 3, 1)),  # P0:P1 encode 0, 1 or 2 carries
            predicate_word(31, 0), predicate_word(32, 1),
            alu(0x210, 33, 16, 17, 18, (74, 1, 1), (81, 3, 7), (84, 3, 7), (87, 3, 0), (77, 3, 1)),
            alu(0x224, 34, 16, 17, 18, (81, 3, 7)),
            alu(0x224, 35, 16, 17, 18, (74, 1, 1), (75, 1, 1), (81, 3, 7), (87, 3, 0)),
            alu(0x225, 36, 16, 17, 18, (73, 1, signed), (74, 1, 1), (81, 3, 2), (87, 3, 0)),
            predicate_word(38, 2),
            alu(0x227, 39, 16, 17, 18, (73, 1, signed), (74, 1, 1), (81, 3, 3), (87, 3, 0)),
            predicate_word(40, 3)]
    return assemble(ops + [store(30 + i, (4 + i) * 4) for i in range(11)] + [EXIT])


def generate(work):
    rng = random.Random(914)
    edges = (0, 1, 2, 0x7fffffff, 0x80000000, 0xfffffffe, U32)
    inputs = [(x, x, x, x) for x in edges]
    inputs += [(0x80000000, 0x80000000, 0, U32), (U32, U32, 2, U32), (U32, 2, 1, U32)]
    inputs += [tuple(rng.choice(edges) for _ in range(4)) for _ in range(54)]
    inputs += [tuple(rng.getrandbits(32) for _ in range(4)) for _ in range(64)]
    programs, initial = [], {}
    for signed in (False, True):
        seed, expected = [], []
        for a, b, c, hi in inputs:
            add = a + b + c
            carry0, carry1 = (add >> 32) & 1, add >> 33
            def operand(v):
                return v - (1 << 32) if signed and v > 0x7fffffff else v
            full = (operand(a) * operand(b) & U64) + ((hi << 32) | c) + carry0
            values = [add, carry0, carry1, add + carry0 + carry1, a*b+c, a*b+(~c)+carry0,
                      full, full >> 32, full >> 64, full >> 32, full >> 64]
            seed.extend([a, b, c, hi] + [GUARD] * 12)
            expected.extend([a, b, c, hi] + [x & U32 for x in values] + [GUARD])
        key = f'{0xfeeda500 + signed:016x}'
        initial[key] = seed + [GUARD] * 8
        programs.append((key, [len(inputs), 1, 1], integer_program(signed), expected + [GUARD] * 8, {'arch':'sm86'}))
    export_cases(work, programs, initial_words=initial)


if __name__ == '__main__':
    generate(Path(sys.argv[1]).resolve())
