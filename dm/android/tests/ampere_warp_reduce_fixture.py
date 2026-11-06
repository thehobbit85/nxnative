"""Compare Ampere peer masks and reductions with each logical warp's input records."""
from functools import reduce
import operator
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path[:0] = [str(ROOT), str(ROOT / 'dm/tools/tests'), str(ROOT / 'dm/gfx/tests')]
from dm.android.tests.ampere_buffer_ops import read_word, record_address
from dm.android.tests.ampere_integer_fixture import predicate_word
from dm.android.tests.compute_words_fixture import GUARD, export_cases
from dm.android.tests.nvn2_layout_fixture import store
from test_dmsh_control_flow import EXIT, assemble, bits

U32 = 0xffffffff


def match(wide):
    ops = record_address(24) + [read_word(16, 0), read_word(17, 1)]
    for any_match, dst in ((True, 20), (False, 21)):
        ops.append(bits((0, 12, 0x3a1), (12, 3, 7), (16, 8, dst), (24, 8, 16), (73, 1, wide),
                        (79, 1, any_match), (81, 3, 0), (110, 3, 7)))
    ops += [predicate_word(22, 0)] + [store(20 + t, 8 + 4*t) for t in range(3)]
    return assemble(ops + [EXIT])


def redux(op, signed):
    ops = record_address(24) + [read_word(16, 0),
          bits((0, 12, 0x3c4), (12, 3, 7), (16, 8, 4), (24, 8, 16), (73, 1, signed),
               (78, 3, op), (110, 3, 7)),
          bits((0, 12, 0xc02), (12, 3, 7), (16, 8, 20), (32, 8, 4), (72, 4, 15), (110, 3, 7)),
          store(20, 8)]
    return assemble(ops + [EXIT])


def generate(work):
    jobs, initial = [], {}
    for width in (36, 64, 100):
        # Some warps agree in the low word alone; others agree on both, or
        # split into several classes. Signed high-bit inputs distinguish MIN/MAX.
        inputs = []
        for tid in range(width):
            warp, lane = divmod(tid, 32)
            lo = [0x80000001, 0xf39b4027, (0x10234191 * (1 + lane%5)) & U32, (0xff00ff00 ^ (lane*0x18279345)) & U32][warp]
            # On the second warp, make low inputs alternate in sign while
            # preserving duplicate 64-bit pairs and one-word collision groups.
            if warp == 1: lo ^= 0xffffffff if lane % 3 == 1 else 0
            hi = 7 if warp == 2 else 0x812ad000 + lane % 4
            if warp == 3: lo, hi = 0x80000001, 7
            inputs.append((lo, hi))
        def add(binary, answers):
            key = f'{0xfeedb600 + len(jobs):016x}'
            seed, expected = [], []
            for idx, record in enumerate(inputs):
                seed.extend((*record, GUARD, GUARD, GUARD, GUARD))
                result = answers(idx)
                expected.extend((*record, *result, *([GUARD] * (4 - len(result)))))
            initial[key] = seed + [GUARD]*8
            jobs.append((key, [width, 1, 1], binary, expected + [GUARD]*8, {'arch': 'sm86'}))
        for wide in (False, True):
            def peers(idx):
                start = idx & ~31
                members = inputs[start:start+32]
                mask = sum(1 << lane for lane, pair in enumerate(members) if pair[:1+wide] == inputs[idx][:1+wide])
                all_match = mask == (1 << len(members)) - 1
                return [mask, mask if all_match else 0, int(all_match)]
            add(match(wide), peers)
        for op, signed in [(n, False) for n in range(6)] + [(4, True), (5, True)]:
            combine = (operator.and_, operator.or_, operator.xor, operator.add, min, max)[op]
            def folded(idx):
                rows = inputs[idx & ~31:(idx & ~31)+32]
                values = [v - (1 << 32) if signed and v > 0x7fffffff else v for v, _ in rows]
                return [reduce(combine, values) & U32]
            add(redux(op, signed), folded)
    export_cases(work, jobs, initial_words=initial)


if __name__ == '__main__':
    generate(Path(sys.argv[1]).resolve())
