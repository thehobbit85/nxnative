"""Execute the registered ARM64 actor repair against synthetic, guarded memory."""
import importlib.util
from pathlib import Path
import platform
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools'))
from nso_asm import B


@unittest.skipUnless(platform.machine().lower() in ('arm64','aarch64'), 'native ARM64 instruction replay')
class ActorPriority(unittest.TestCase):
    def test_sentinel_fault_and_guard_preserve_valid_priorities(self):
        path = REPO/'dm/games/01006BB00C6F0000/patches.py'
        spec = importlib.util.spec_from_file_location('actor_patch', path)
        assert spec and spec.loader
        module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
        words, sites, puts = [], {}, {}
        ctx = SimpleNamespace(here=lambda: 0x20000+4*len(words), emit=words.append,
                              site=lambda off, old: sites.update({off: bytes.fromhex(old)}),
                              put=lambda off, data, why: puts.update({off: struct.pack('<'+'I'*len(data),*data)}))
        module.CODE_STUBS['909e904af78ac1b8deefe97ab2ccdb51'](ctx)
        code = bytearray(0x30000)
        # Synthetic ABI wrapper: x8=index, x9=stride, x10=table; return W8 in W0.
        struct.pack_into('<4I',code,0,0xaa0003e8,0xd2800b09,0xaa0103ea,B(12,0x1c17c))
        struct.pack_into('<2I',code,0x1c184,0x2a0803e0,0xd65f03c0)
        for off, data in sites.items(): code[off:off+len(data)]=data
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); original=root/'original.bin'; fixed=root/'fixed.bin'; runner=root/'replay'
            original.write_bytes(code)
            for off, data in puts.items(): code[off:off+len(data)]=data
            struct.pack_into('<'+'I'*len(words),code,0x20000,*words); fixed.write_bytes(code)
            subprocess.run(['cc','-std=c11','-D_DEFAULT_SOURCE',str(Path(__file__).with_suffix('.c')),'-o',str(runner)],check=True)
            result=subprocess.run([str(runner),str(original),str(fixed)],capture_output=True,text=True,check=True)
            self.assertIn('original sentinel faults; guarded sentinel = 3; 40 valid priorities unchanged',result.stdout)


if __name__ == '__main__': unittest.main()
