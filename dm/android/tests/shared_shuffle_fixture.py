"""Exchange live guest shared-memory values through Maxwell shuffles."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path[:0] = [str(ROOT), str(ROOT / 'dm/tools/tests')]
from dm.android.tests.compute_words_fixture import GUARD, export_cases
from dm.android.tests.warp_shuffle_fixture import SEED
from dm.android.tests.warp_vote_fixture import PT, store_pair
from test_txq_type import base, mov, shader


def case(width, xor):
    sync = base('Bar') | (1 << 44) | (1 << 43) | (7 << 39) | PT
    ops = [base('S2r') | (33 << 20) | PT | 14, mov(15, 2),
           base('Shl') | (15 << 20) | PT | (14 << 8) | 2,
           mov(10, SEED), base('Iadd') | (10 << 20) | PT | (14 << 8) | 4,
           base('Sts') | (4 << 48) | PT | (2 << 8) | 4, sync,
           base('Lop32i') | (2 << 53) | (32 << 20) | PT | (14 << 8) | 3,
           base('Shl') | (15 << 20) | PT | (3 << 8) | 3,
           base('Lds') | (4 << 48) | PT | (3 << 8) | 8,
           base('Shfl') | (7 << 48) | (31 << 34) | (3 << 30) | (3 << 28)
           | (xor << 20) | PT | (8 << 8) | 8,
           base('Lds') | (4 << 48) | PT | (2 << 8) | 9]
    words = [w for tid in range(width) for w in (SEED + (tid ^ 32 ^ xor), SEED + tid)]
    return shader(ops + store_pair()), words + [GUARD] * 8


def generate(work):
    programs = []
    for width in (64, 128):
        for xor in (1, 4, 16, 31):
            binary, words = case(width, xor)
            key = f'{0xfeeda200 + len(programs):016x}'
            programs.append((key, [width, 1, 1], binary, words, {'shared': width * 4}))
    export_cases(work, programs)


if __name__ == '__main__':
    generate(Path(sys.argv[1]).resolve())
