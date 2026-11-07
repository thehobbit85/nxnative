"""Shared guest resource distinctions must survive Android shader export."""
import json
import ctypes
from pathlib import Path
import shutil
import sys
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
from dm.android.tests.resource_shapes_fixture import generate
from dm.android.tools import spv_android as vk
from dm.android.tools import spv_resources


@unittest.skipUnless(shutil.which('spirv-val') and shutil.which('spirv-opt'), 'requires SPIR-V tools')
class ResourceShapes(unittest.TestCase):
    def test_actual_maxwell_resource_interfaces(self):
        for variant in ('wrapped', 'size', 'slots', 'many', 'many_pool'):
            with self.subTest(variant=variant), tempfile.TemporaryDirectory() as tmp:
                work = Path(tmp); export = generate(work, variant)
                text = next(export.glob('*.vk.txt')).read_text()
                bindings = [line for line in text.splitlines() if line.startswith('b ')]
                reflection = json.loads(next(work.glob('*.reflection.json')).read_text())
                if variant in ('wrapped', 'size'):
                    self.assertTrue(any(b.get('fetch1d') for b in reflection['bindings']))
                    self.assertIn('fetch1d=1', text)
                    self.assertEqual('fetch1d_size=1' in text, variant == 'size')
                elif variant == 'slots':
                    self.assertTrue(any('utb 32 tex_slots ' in b for b in bindings))
                    self.assertTrue(any('samp 32 samp_slots ' in b for b in bindings))
                    self.assertIn('cap 5311\n', text)
                    self.assertIn('cap 5301\n', text)
                    self.assertIn('ext SPV_EXT_descriptor_indexing\n', text)
                elif variant == 'many':
                    self.assertEqual(len(reflection['bindings']), 106)
                    self.assertEqual(len(bindings), 66)
                else:
                    self.assertEqual(len(bindings), 69)
                    self.assertIn('runtime=1 pool_slot=1', text)

    def test_export_binding_capacity_is_bounded(self):
        def meta(n):
            return dict(stage='cs', bindings=[dict(binding=i, type='ubo', count=1, kind='cbuf', reg=0) for i in range(n)])
        self.assertEqual(vk.vk_txt(meta(128)).count('\nb '), 128)
        with self.assertRaisesRegex(ValueError, '128'): vk.vk_txt(meta(129))

    def test_resource_fields_reject_ambiguous_shapes(self):
        good = dict(binding=2, type='cis', count=1, kind='tex', reg=128, view=1, fetch1d=True, fetch1d_size=True)
        for field, value in [('fetch1d', False), ('fetch1d', 1), ('fetch1d_size', 'true'),
                             ('kind', 'sbuf'), ('view', 0), ('type', 'utb')]:
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, 'wrapped fetch'):
                vk.vk_txt(dict(stage='cs', bindings=[dict(good, **{field: value})]))
        for descriptor, element in [('tex', 'texture2d<float>'), ('tex', 'depth2d<float>'),
                                    ('utb', 'texture_buffer<float>'), ('samp', 'sampler')]:
            resource = dict(msl_name='cp_t_cb0_40000000', msl_type=f'array<{element}, 32>')
            actual = spv_resources.slot_array(resource, descriptor, 32)
            self.assertEqual(actual['kind'], 'samp_slots' if descriptor == 'samp' else 'tex_slots')
            for count in (0, 31, 33):
                with self.assertRaisesRegex(ValueError, 'slot array'): spv_resources.slot_array(resource, descriptor, count)

    def test_wrapped_shapes_survive_array_and_table_interfaces(self):
        for kind in ('tex', 'tex_ubo', 'tex_slots', 'tex_c2a', 'tex_htab', 'tex_pool'):
            with self.subTest(kind=kind):
                binding = dict(type='cis', count=3, kind=kind, reg=None, view=1, fetch1d=True, fetch1d_size=True)
                self.assertEqual(spv_resources.fields(binding), ('fetch1d=1', 'fetch1d_size=1'))
        for count in (0, -1, True):
            with self.subTest(count=count), self.assertRaisesRegex(ValueError, 'wrapped fetch'):
                spv_resources.fields(dict(type='tex', count=count, kind='tex_c2a', view=1, fetch1d=True))

    def test_native_reader_retains_and_bounds_resource_shapes(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp); source = work / 'check.c'; library = work / 'check.so'
            source.write_text('#include "dm/gfx/nvn/vulkan/resource_contract.h"\n'
                              'int shape(const char *s,size_t n){dm_vk_resource_shape v;'
                              'return dm_vk_resource_line(s,n,&v)?-1:(v.fetch1d | v.fetch1d_size<<1);}'
                              'int capacity(void){return DM_VK_MAX_BINDINGS;}')
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC', '-I', str(ROOT), source, '-o', library],
                           check=True, capture_output=True)
            module = ctypes.CDLL(str(library)); module.shape.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
            module.shape.restype = ctypes.c_int; self.assertEqual(module.capacity(), spv_resources.MAX_BINDINGS)
            good = b'b 2 cis 1 tex 128 -1 samp_tex 128 view=1 num=0 fetch1d=1 fetch1d_size=1'
            self.assertEqual(module.shape(good, len(good)), 3)
            plain = good.split(b' fetch1d=')[0]; self.assertEqual(module.shape(plain, len(plain)), 0)
            for bad in (good.replace(b'fetch1d=1', b'fetch1d=0'), good.replace(b'fetch1d=1 ', b''),
                        good + b' fetch1d=1', good.replace(b'view=1', b'view=0'), good.replace(b'cis 1', b'cis 0')):
                with self.subTest(line=bad): self.assertEqual(module.shape(bad, len(bad)), -1)
            for kind in ('tex', 'tex_ubo', 'tex_slots', 'tex_c2a', 'tex_htab', 'tex_pool'):
                line = f'b 2 cis 3 {kind} -1 32 samp_tex -1 view=1 fetch1d=1 fetch1d_size=1'.encode()
                with self.subTest(kind=kind): self.assertEqual(module.shape(line, len(line)), 3)
            for descriptor, kind in [('utb', 'tex_slots'), ('samp', 'samp_slots')]:
                line = f'b 36 {descriptor} 32 {kind} -1 -1 - -1'.encode()
                self.assertEqual(module.shape(line, len(line)), 0)
                for bad in (line.replace(b' 32 ', b' 33 '), line.replace(b' -1 -1 ', b' 0 -1 ')):
                    self.assertEqual(module.shape(bad, len(bad)), -1)


if __name__ == '__main__': unittest.main()
