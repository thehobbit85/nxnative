"""Read back guest per-thread local memory through differently scaled address registers."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path[:0] = [str(ROOT), str(ROOT / 'dm/tools/tests'), str(ROOT / 'dm/gfx/tests')]
from dm.android.tests.ampere_buffer_ops import read_word, record_address
from dm.android.tests.compute_words_fixture import GUARD, export_cases
from dm.android.tests.nvn2_layout_fixture import store
from test_dmsh_control_flow import EXIT, assemble, bits


def local(write, reg, address, size, offset=0, stride=0):
    return bits((0, 12, 0x387 if write else 0x983), (12, 3, 7), (32 if write else 16, 8, reg),
                (24, 8, address), (40, 24, offset), (73, 3, size), (78, 2, stride), (110, 3, 7))


def program():
    ops = record_address(80)
    ops.extend(read_word(12 + word, word) for word in range(8))
    # Different encodings address the same 16 bytes. Alternate signed and
    # unsigned loads, then replace a single byte and halfword in those words.
    ops += [local(True, 16, 12, 6, stride=3), local(False, 20, 13, 0, 1), local(False, 21, 13, 1, 1),
            local(False, 22, 14, 2, 2, 1), local(False, 23, 14, 3, 2, 1), local(False, 24, 15, 5, 8, 2),
            local(True, 18, 13, 0, 5), local(True, 19, 13, 2, 10), local(False, 26, 12, 6, stride=3)]
    return assemble(ops + [store(20+i, 4*(8+i)) for i in range(10)] + [EXIT])


def generate(work):
    programs, initial = [], {}
    for width in (2, 32, 64, 128):
        for varying in (False, True):
            seed, expected = [], []
            for tid in range(width):
                index = (tid*7 % 11) if varying else 5
                a, b, c, d = [(v + tid*0x00110317) & 0xffffffff for v in (0x80caff11, 0x62398942, 0x0958ac3b, 0x3157ce8a)]
                inputs = [index, index*16, index*4, index*2, a, b, c, d]
                seed += inputs + [GUARD]*12
                byte, half = (a >> 8) & 255, a >> 16
                results = [byte, byte - (256 if byte & 128 else 0), half, half - (65536 if half & 32768 else 0),
                           c, d, a, (b & 0xffff00ff) | ((c & 255) << 8), (c & 65535) | ((d & 65535) << 16), d]
                expected += inputs + [x & 0xffffffff for x in results] + [GUARD]*2
            key = f'{0xfeedb500 + len(programs):016x}'
            initial[key] = seed + [GUARD]*8
            programs.append((key, [width, 1, 1], program(), expected + [GUARD]*8, {'arch': 'sm86'}))
    export_cases(work, programs, initial_words=initial)


if __name__ == '__main__':
    generate(Path(sys.argv[1]).resolve())
