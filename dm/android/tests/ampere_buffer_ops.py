"""Guest instruction sequences for compute fixtures with one buffer record per lane."""
from dm.android.tests.nvn2_layout_fixture import load
from test_dmsh_ampere_alu import s2r_tid, waiting
from test_dmsh_control_flow import bits


def record_address(stride):
    # S2R R9, TID.X; LDC.64 R4:R5, c3[56]; IMAD R4, R9, stride, R4.
    return [s2r_tid(9), load(4, 3, 56, 5),
            waiting(bits((0, 12, 0x824), (12, 3, 7), (16, 8, 4), (24, 8, 9),
                         (32, 32, stride), (64, 8, 4), (81, 3, 7), (110, 3, 7)))]


def read_word(reg, word):
    return bits((0, 12, 0x381), (12, 3, 7), (16, 8, reg), (24, 8, 4), (40, 24, word * 4),
                (72, 1, 1), (73, 3, 4), (110, 3, 7))
