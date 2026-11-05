"""Uniform-held storage addresses retain their source, width and bounded range."""
import ctypes
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'android/tools'))
import spv_android as vk
from test_dmsh_pool_arrays import NS
from test_txq_type import base, mov, shader

ROOT = Path(__file__).resolve().parents[3]


class UniformAddressExport(unittest.TestCase):
    def test_storage_address_source_survives_sidecar_export(self):
        binding = dict(binding=7, type='ssbo', count=1, kind='sbuf_ubo', reg=2, off=56)
        try: text = vk.vk_txt(dict(stage='cs', bindings=[binding]))
        except ValueError as error: self.fail(f'shared storage-address binding must export: {error}')
        self.assertIn('b 7 ssbo 1 sbuf_ubo 2 56 - -1\n', text)


@unittest.skipUnless(shutil.which('cc'), 'requires C compiler')
class UniformAddressBounds(unittest.TestCase):
    def test_full_eight_byte_field_is_required_without_address_wrap(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / 'bounds.c'; library = source.with_suffix('.so')
            source.write_text('#include "dm/gfx/nvn/vulkan/uniform_address.h"\n'
                              'uint64_t field(uint64_t base, uint64_t size, int64_t off) {'
                              'return dm_vk_uniform_address_field(base, size, off); }')
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                            '-I', str(ROOT), source, '-o', library], check=True, capture_output=True)
            field = ctypes.CDLL(str(library)).field
            field.argtypes = [ctypes.c_uint64, ctypes.c_uint64, ctypes.c_int64]
            field.restype = ctypes.c_uint64
            for address, size, offset, expected in [
                    (0x1000, 32, 12, 0x100c), (0x1000, 32, 24, 0x1018),
                    (0x1000, 32, 25, 0), (0x1000, 32, -1, 0), (0, 32, 0, 0),
                    (0x1000, 7, 0, 0), (0x1000, 8, 0, 0x1000),
                    (2**64-16, 32, 8, 0), (2**64-16, 32, 7, 2**64-9),
                    (2**64-4, 8, 0, 0), (0x1000, 2**64-1, 2**63-1, 2**63+0xfff)]:
                with self.subTest(address=address, size=size, offset=offset):
                    self.assertEqual(field(address, size, offset), expected)


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE') and all(shutil.which(t) for t in ('spirv-opt', 'spirv-val')),
                     'requires the locked translator and SPIRV-Tools')
class TranslatedUniformAddress(unittest.TestCase):
    def test_maxwell_uniform_address_reuses_shared_reflection(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp); out = work / 'export'; out.mkdir()
            jobs = []
            for number, (slot, offset) in enumerate(((3, 56), (5, 40), (16, 4088))):
                # LDC.64 R4,c[slot][offset]; STG.E.32 [R4],R8.
                code = shader(base('Ldc') | (5 << 48) | (slot << 36) | (offset << 20) | (7 << 16) | (255 << 8) | 4,
                              mov(8, 0xabcdef01), base('Stg') | (4 << 48) | (1 << 45) | (7 << 16) | (4 << 8) | 8)
                key = f'{0xfeed2000+number:016x}'; path = work / (key + '.bin'); path.write_bytes(code)
                jobs.append(dict(key=key, stage='cs', code=str(path), local=[1,1,1], host_target='vulkan'))
            path = work / 'jobs.json'; path.write_text(json.dumps(jobs))
            subprocess.run([os.environ['DMSH_TEST_XLATE'], path, work], check=True)
            for job, (slot, offset) in zip(jobs, ((3,56), (5,40), (16,4088))):
                key = job['key']; binary = work / (key + '.spv')
                source = subprocess.run([ROOT / 'dm/gfx/tools/spirv_cross_bin/spirv-cross', '--msl', '--msl-version', '20300',
                                         '--msl-argument-buffers', binary], check=True, text=True, capture_output=True).stdout
                resources = json.loads((work / (key + '.res.json')).read_text())
                bindings = NS['reflect'](key, 'cs', source, resources)
                self.assertEqual(len([b for b in bindings if b['kind'] == 'sbuf_ubo']), 1, (resources, bindings))
                storage, = [b for b in bindings if b['kind'] == 'sbuf_ubo']
                self.assertEqual((storage['reg'], storage['off'], storage['rw']), (slot-3, offset, True))
                vk.write_stage(key, binary.read_bytes(), dict(stage='cs', bindings=bindings), out,
                               shutil.which('spirv-val'), shutil.which('spirv-opt'))
                text = (out / (key + '.vk.txt')).read_text()
                self.assertIn(f'ssbo 1 sbuf_ubo {slot-3} {offset} - -1\n', text)


if __name__ == '__main__': unittest.main()
