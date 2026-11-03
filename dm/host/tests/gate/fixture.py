"""Build linked NSOs and rewrite them for the selected ARM64 host."""
from pathlib import Path
import os
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT))
from dm.tools.tests.test_nso import make_nso

# void guest(uint64_t results[5]), with the thread tag in results[0]. The BRK delivers a signal while the guest has a spill below SP.
ENTRY = (
    0x14000002, 0,                  # b start; MOD0 in the read-only segment
    0xaa0003e1, 0xf9400020,              # mov x1,x0; ldr x0,[x1]
    0xd51bd040, 0xaa0003f2,              # msr tpidr_el0,x0; mov x18,x0
    0xd53bd042, 0xd53bd063,              # mrs x2,tpidr_el0; mrs x3,tpidrro_el0
    0x9e67001f,                         # fmov d31,x0 (signal context must preserve the tag)
    0xf81c03e0, 0xd42000e0, 0xf85c03e4,  # stur x0,[sp,#-64]; brk #7; ldur x4,[sp,#-64]
    0xd40003c1,                          # svc #0x1e
    0xf9000020, 0xf9000432,              # str x0,[x1]; str x18,[x1,#8]
    0xd53bd042, 0xf9000822, 0xf9000c31,  # mrs x2,tpidr_el0; str x2,[x1,#16]; str x17,[x1,#24]
    0xd53be030, 0xf9001030,              # mrs x16,cntpct_el0; str x16,[x1,#32]
    0xd65f03c0,                          # ret
)
CALLBACK = (
    0xaa0003f2, 0xd53bd061,              # mov x18,x0; mrs x1,tpidrro_el0
    0xb9411021, 0x8b010240,              # ldr w1,[x1,#0x110]; add x0,x18,x1
)


def module(dest, target, name, code, exports, imported, align):
    """The last two instructions load a relocated GOT entry and tail-call it."""
    ro_off, data_off = align, 2 * align
    load_pc = 4 * len(code)
    code += (0x58000010 | ((data_off - load_pc) // 4 << 5), 0xd61f0200)  # ldr x16,got; br x16
    ro = bytearray(512)
    ro[:4] = b'MOD0'
    struct.pack_into('<i', ro, 4, 0x80)   # MOD0-relative dynamic table
    struct.pack_into('<i', ro, 16, 0x30)  # MOD0-relative EH header
    starts = sorted({8, *exports.values()})
    struct.pack_into('<4BII', ro, 0x30, 1, 3, 3, 3, 0, len(starts))
    for i, start in enumerate(starts):
        struct.pack_into('<II', ro, 0x3c + i * 8, start, 0)

    symbols = [('', 0), *exports.items(), (imported, 0)]
    strings = bytearray(b'\0')
    symtab = 0x100
    strtab = symtab + 24 * len(symbols)
    for i, (symbol, pc) in enumerate(symbols[1:], 1):
        struct.pack_into('<IBBHQQ', ro, symtab + 24 * i, len(strings), 0x12, 0, bool(pc), pc, 0)
        strings += symbol.encode() + b'\0'
    ro[strtab:strtab + len(strings)] = strings
    rela = 0x1d0
    struct.pack_into('<QQq', ro, rela, data_off, (len(symbols) - 1) << 32 | 1026, 0)  # JUMP_SLOT
    for i, (kind, value) in enumerate(((6, ro_off + symtab), (5, ro_off + strtab),
                                       (23, ro_off + rela), (2, 24))):
        struct.pack_into('<QQ', ro, 0x80 + i * 16, kind, value)

    text = bytearray(struct.pack(f'<{len(code)}I', *code))
    struct.pack_into('<I', text, 4, ro_off)  # MOD0 pointer
    nso = make_nso((text, ro, bytes(64)), compress=(0, 0, 0))
    struct.pack_into('<I', nso, 0x24, ro_off)
    struct.pack_into('<I', nso, 0x34, data_off)
    source = dest / (name + '.nso')
    source.write_bytes(nso)
    subprocess.run([sys.executable, ROOT / 'dm/tools/patch_nso.py', source, dest, name, '--thread-slots', target],
                   check=True, env=dict(os.environ, DM_REWRITE_J='1'))


def main():
    target, output = sys.argv[1:3]
    align = int(sys.argv[3]) if len(sys.argv) > 3 else 16384
    assert align in (4096, 16384)
    dest = Path(output)
    # main.callback -> sdk.adjust (+3) -> registered host import test_sdk_call (+13).
    module(dest, target, 'main', ENTRY + CALLBACK, {'guest_entry': 8, 'guest_callback': 4 * len(ENTRY)}, 'adjust', align)
    module(dest, target, 'sdk', (0x14000002, 0, 0x91000c00), {'adjust': 8}, 'test_sdk_call', align)
    # A guest import resolved from the production service registry.
    module(dest, target, 'clockprobe', (0x14000002, 0), {'read_steady': 8},
           '_ZN2nn4time19StandardSteadyClock19GetCurrentTimePointEPNS0_20SteadyClockTimePointE', align)


if __name__ == '__main__':
    main()
