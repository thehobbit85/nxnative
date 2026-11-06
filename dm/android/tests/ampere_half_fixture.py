"""Compare packed-half arithmetic, operand routing and result modes on Metal and Vulkan."""
import json
import os
from pathlib import Path
import random
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path[:0] = [str(ROOT), str(ROOT / 'dm/tools/tests'), str(ROOT / 'dm/gfx/tests')]
from dm.android.tests.ampere_buffer_ops import read_word, record_address
from dm.android.tests.ampere_integer_fixture import alu
from dm.android.tests.compute_words_fixture import GUARD, export_cases
from dm.android.tests.nvn2_layout_fixture import store
from test_dmsh_control_flow import EXIT, assemble, bits


def packed(lo, hi):
    return int.from_bytes(struct.pack('<ee', lo, hi), 'little')


def scalar(value):
    return int.from_bytes(struct.pack('<f', value), 'little')


def operations(a, b, c):
    # Each row gives a guest encoding and the two results expected from its
    # assembly operands. Use binary fractions whose FP32 intermediates are exact.
    add = alu(0x230, 30, 16, 17, 18)
    mul = alu(0x232, 30, 16, 17, 18)
    fma = alu(0x231, 30, 16, 17, 18)
    def flagged(inst, *fields):
        hi, lo = bits(*fields)
        return inst[0] | hi, inst[1] | lo
    def immediate(op):
        return bits((0, 12, op), (12, 3, 7), (16, 8, 30), (24, 8, 16),
                    (32, 32, packed(1.5, -0.5)), (110, 3, 7))
    x, y = a
    u, v = b
    m, n = c
    return [
        (add, packed(x + u, y + v)),
        (mul, packed(x * u, y * v)),
        (fma, packed(x * u + m, y * v + n)),
        (flagged(add, (74, 2, 2)), packed(x + u, x + v)),  # A.H0_H0
        (flagged(mul, (60, 2, 3)), packed(x * v, y * v)),  # B.H1_H1
        (flagged(fma, (81, 2, 2)), packed(x * u + m, y * v + m)),
        (flagged(fma, (86, 1, 1)), packed(-x * v + m, y * u + n)),
        (flagged(add, (72, 1, 1), (73, 1, 1)), packed(-abs(x) + u, -abs(y) + v)),
        (flagged(fma, (63, 1, 1), (84, 1, 1)), packed(-x * u - m, -y * v - n)),
        (immediate(0x430), packed(x + 1.5, y - 0.5)),
        (immediate(0x432), packed(x * 1.5, y * -0.5)),
        (flagged(add, (77, 1, 1)), packed(min(1, max(0, x + u)), min(1, max(0, y + v)))),
        (flagged(mul, (79, 1, 1)), packed(max(0, x * u), max(0, y * v))),
        (flagged(fma, (79, 1, 1)), packed(max(0, x * u + m), max(0, y * v + n))),
        (flagged(fma, (78, 1, 1)), scalar(x * u + m)),
    ]


def check_bfloat_rejection(work):
    ops = record_address(16) + [read_word(16 + i, i) for i in range(3)]
    ops += [alu(0x231, 30, 16, 17, 18, (85, 1, 1)), store(30, 12), EXIT]
    translator = os.environ.get('DMSH_TEST_XLATE', ROOT / 'dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate')
    for target in ('apple', 'vulkan'):
        out = work / ('unsupported-' + target)
        out.mkdir(parents=True, exist_ok=True)
        src = out / 'bfloat.bin'
        src.write_bytes(assemble(ops))
        job = dict(key='bfloat', arch='sm86', host_target=target, stage='cs', local=[32, 1, 1], code=str(src))
        jobs = out / 'jobs.json'
        jobs.write_text(json.dumps([job]))
        run = subprocess.run([translator, jobs, out], capture_output=True, text=True)
        if run.returncode == 0 or 'BF16 half pair' not in run.stdout + run.stderr or (out / 'bfloat.spv').exists():
            raise ValueError(f'{target} did not reject the unsupported BF16 instruction')


def generate(work):
    check_bfloat_rejection(work)
    rng = random.Random(207)
    grid = (-3.5, -1.25, -0.75, -0.5, -0.125, 0.125, 0.5, 0.75, 1.25, 3.5,
            1.0009765625, 1.001953125)
    records = [tuple((rng.choice(grid), rng.choice(grid)) for _ in range(3)) for _ in range(96)]
    example = operations(*records[0])
    # Each result is consumed before the destination is reused. Prefix, padding
    # and tail words catch an incorrectly sized record or half-word write.
    stride = 32
    program = record_address(stride * 4) + [read_word(16 + i, i) for i in range(3)]
    for i, (op, _) in enumerate(example, start=4):
        program.extend((op, store(30, i * 4)))
    seed, expected = [], []
    for record in records:
        inputs = [packed(*pair) for pair in record] + [GUARD]
        outputs = [value for _, value in operations(*record)]
        seed.extend(inputs + [GUARD] * (stride - len(inputs)))
        expected.extend(inputs + outputs + [GUARD] * (stride - len(inputs) - len(outputs)))
    key = '00000000feeda900'
    export_cases(work, [(key, [len(records), 1, 1], assemble(program + [EXIT]),
                         expected + [GUARD] * 8, {'arch': 'sm86'})], initial_words={key: seed + [GUARD] * 8})


if __name__ == '__main__':
    generate(Path(sys.argv[1]).resolve())
