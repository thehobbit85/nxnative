"""Execute Maxwell VOTE with a sparse active mask and a partial second warp."""
import argparse
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path[:0] = [str(ROOT), str(ROOT / 'dm/tools/tests')]
from dm.android.tests.compute_words_fixture import GUARD, export_cases
from dm.android.tests.predicated_warp_fixture import less_than
from dm.android.tests.warp_vote_fixture import PT, store_pair
from test_txq_type import base, mov, shader

WIDTH = 48
INACTIVE = 0x4321


def program(mode, inverse, split, early_exit):
    # P0 selects alternating blocks of four. P2 is the predicate being
    # voted on. R8 and P1 keep their seed values when the vote is predicated off.
    ops = [base('S2r') | (33 << 20) | PT | 14, mov(12, 4),
           base('Lop') | (12 << 20) | PT | (14 << 8) | 11,
           less_than(0, 11, 12), mov(10, split), less_than(2, 14, 10),
           mov(8, INACTIVE), mov(10, 0), less_than(1, 14, 10)]
    guard = int(inverse) << 19
    if early_exit:
        ops.append(base('Exit') | (int(not inverse) << 19) | 15)
        guard = PT
    ops += [base('Vote') | (mode << 48) | (1 << 45) | (2 << 39) | guard | 8,
            mov(10, 1), base('Sel') | (1 << 39) | (255 << 20) | PT | (10 << 8) | 9]
    return shader(ops + store_pair())


def expected(mode, inverse, split, early_exit):
    words = []
    for start in range(0, WIDTH, 32):
        warp = range(start, min(start + 32, WIDTH))
        participants = {tid for tid in warp if bool(tid & 4) == inverse}
        votes = [tid < split for tid in participants]
        predicate = (all(votes), any(votes), len(set(votes)) == 1)[mode]
        ballot = sum(1 << (tid % 32) for tid in participants if tid < split)
        for tid in warp:
            if tid in participants:
                words.extend((ballot, int(predicate)))
            elif early_exit:
                words.extend((GUARD, GUARD))
            else:
                words.extend((INACTIVE, 0))
    return words + [GUARD] * 8


def generate(work, form):
    cases = []
    key = 0xfeedb600
    for early_exit in (False, True):
        for mode in range(3):
            for inverse in (False, True):
                for split in (0, 19, 65):
                    if form in ('both', 'exit' if early_exit else 'predicated'):
                        args = (mode, inverse, split, early_exit)
                        cases.append((f'{key:016x}', [WIDTH, 1, 1], program(*args), expected(*args), {}))
                    key += 1
    export_cases(work, cases)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--form', choices=('both', 'exit', 'predicated'), default='both',
                        help='the exit form keeps the sparse vote divergent on M2 Metal')
    args = parser.parse_args()
    generate(args.directory.resolve(), args.form)
