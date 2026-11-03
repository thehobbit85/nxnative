"""Write a C fixture containing the real rewriter's image, key-page layout and SVC return PC."""
from pathlib import Path
import os
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT))
from dm.tools.tests.test_nso import make_nso

# void guest(uint64_t results[4]), with the thread tag in results[0]. The BRK delivers a signal while the guest has a spill below SP.
WORDS = (
    0x14000002, 0x1000,                  # b start; MOD0 in the read-only segment
    0xaa0003e1, 0xf9400020,              # mov x1,x0; ldr x0,[x1]
    0xd51bd040, 0xaa0003f2,              # msr tpidr_el0,x0; mov x18,x0
    0xd53bd042, 0xd53bd063,              # mrs x2,tpidr_el0; mrs x3,tpidrro_el0
    0xf81c03e0, 0xd42000e0, 0xf85c03e4,  # stur x0,[sp,#-64]; brk #7; ldur x4,[sp,#-64]
    0xd40003c1,                          # svc #0x1e
    0xf9000020, 0xf9000432,              # str x0,[x1]; str x18,[x1,#8]
    0xd53bd042, 0xf9000822, 0xf9000c31,  # mrs x2,tpidr_el0; str x2,[x1,#16]; str x17,[x1,#24]
    0xd65f03c0,                          # ret
)


def main():
    target, dest = sys.argv[1:]
    ro = bytearray(128)
    ro[:4] = b'MOD0'
    struct.pack_into('<i', ro, 16, 48)
    struct.pack_into('<4BIIII', ro, 48, 1, 3, 3, 3, 0, 1, 8, 0)  # one unwind entry for start
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp)
        nso = d / 'input'
        nso.write_bytes(make_nso((struct.pack(f'<{len(WORDS)}I', *WORDS), ro, bytes(64)), compress=(0, 0, 0)))
        subprocess.run([sys.executable, ROOT / 'dm/tools/patch_nso.py', nso, d, 'guest', '--thread-slots', target],
                       check=True, env=dict(os.environ, DM_REWRITE_J='1'))
        fields = dict(line[2:].split('\t') for line in (d / 'guest.fixups.tsv').read_text().splitlines())
        pc, kind, svc, _ = (d / 'guest.gates.tsv').read_text().split()
        assert kind == 'svc' and int(svc) == 0x1e
        image = (d / 'guest.patched.bin').read_bytes()
        words = (f'    [{i}] = {w:#x},' for i, (w,) in enumerate(struct.iter_unpack('<I', image)) if w)
        header = [f'static const uint32_t guest_image[{len(image) // 4}] = {{', *words, '};',
                  f'#define GUEST_GATE_PC {int(pc, 16):#x}', f'#define GUEST_KEY_PAGE {fields["KEY_PAGE_OFF"]}']
        for key in ('K18', 'KTLS', 'KTLR', 'KGATE'):
            header.append(f'#define GUEST_{key} {fields[f"KEY_{key}_IDX"]}')
        Path(dest).write_text('\n'.join(header) + '\n')


if __name__ == '__main__':
    main()
