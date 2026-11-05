"""Split uniform handles retain both source words for textures and samplers."""
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
from test_txq_type import base, shader

ROOT = Path(__file__).resolve().parents[3]


class SplitHandleExport(unittest.TestCase):
    def test_combined_descriptor_keeps_each_half_independently(self):
        binding = dict(binding=0, type='cis', count=1, kind='tex_ubo', reg=7, off=12, sreg=8, soff=20,
                       samp=dict(kind='samp_ubo', reg=8, off=20, sreg=7, soff=12))
        text = vk.vk_txt(dict(stage='ps', bindings=[binding]))
        self.assertIn('tex_ubo 7 12 samp_ubo 8 soff=20', text)
        self.assertIn('twin_tex=8:20', text)
        self.assertIn('twin_samp=7:12', text)

    def test_invalid_second_sources_are_rejected_and_array_semantics_unchanged(self):
        binding = dict(binding=0, type='tex', count=1, kind='tex_ubo', reg=7, off=12, sreg=8, soff=20)
        for changes in ({'sreg': -1}, {'sreg': 16}, {'soff': -1}, {'soff': 2**31}, {'soff': None}, {'sreg': True}):
            with self.subTest(changes=changes), self.assertRaisesRegex(ValueError, 'second uniform'):
                vk.vk_txt(dict(stage='ps', bindings=[dict(binding, **changes)]))
        self.assertNotIn('twin_', vk.vk_txt(dict(stage='ps', bindings=[dict(binding, count=32)])))


@unittest.skipUnless(shutil.which('cc'), 'requires C compiler')
class SplitHandleReader(unittest.TestCase):
    def test_native_parser_preserves_zero_offsets_and_rejects_bad_sources(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp); source = work / 'reader.c'; library = work / 'reader.so'
            source.write_text('#include "dm/gfx/nvn/vulkan/split_handle.h"\n'
                              'int read_fields(const char *s,int *out){dm_vk_split_handle h;'
                              'if(!dm_vk_split_handle_parse(s,&h))return 0;'
                              'out[0]=h.texture.reg;out[1]=h.texture.off;out[2]=h.sampler.reg;out[3]=h.sampler.off;return 1;}')
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC', '-I', str(ROOT),
                            source, '-o', library], capture_output=True, check=True)
            read = ctypes.CDLL(str(library)).read_fields
            read.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_int)]; read.restype = ctypes.c_int
            out = (ctypes.c_int*4)()
            for text, expected in [('b 0 tex 1', (-1,-1,-1,-1)),
                                   ('b twin_tex=0:0 twin_samp=15:2147483647', (0,0,15,2147483647))]:
                self.assertEqual(read(text.encode(), out), 1); self.assertEqual(tuple(out), expected)
            for bad in ('-1:0', '16:0', '1:-1', '1:2147483648', '1:0junk', '1:', 'x:1',
                        '1:99999999999999999999999999', '1:0 twin_tex=2:0'):
                with self.subTest(source=bad): self.assertEqual(read(('b twin_tex='+bad).encode(), out), 0)


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE') and all(shutil.which(t) for t in ('spirv-opt','spirv-val')),
                     'requires the locked translator and SPIRV-Tools')
class TranslatedSplitHandle(unittest.TestCase):
    def test_maxwell_or_handle_keeps_both_shared_sources(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp); export = work / 'export'; export.mkdir(); key = '00000000feed3000'
            def load(reg, slot, offset, width=4):
                return base('Ldc') | (width << 48) | (slot << 36) | (offset << 20) | (7 << 16) | (255 << 8) | reg
            code = shader(load(10,10,12), load(11,11,20), base('Lop') | (1 << 41) | (11 << 20) | (7 << 16) | (10 << 8) | 6,
                          base('TexB') | (1 << 37) | (1 << 31) | (2 << 28) | (6 << 20) | (7 << 16) | 8,
                          load(4,3,56,5), base('Stg') | (4 << 48) | (1 << 45) | (7 << 16) | (4 << 8) | 8)
            path = work / 'shader.bin'; path.write_bytes(code)
            job = work / 'jobs.json'; job.write_text(json.dumps([dict(key=key,stage='cs',code=str(path),local=[1,1,1],host_target='vulkan')]))
            subprocess.run([os.environ['DMSH_TEST_XLATE'], job, work], check=True)
            binary = work / (key+'.spv')
            source = subprocess.run([ROOT/'dm/gfx/tools/spirv_cross_bin/spirv-cross', '--msl', '--msl-version', '20300',
                                     '--msl-argument-buffers', binary], capture_output=True, text=True, check=True).stdout
            resources = json.loads((work/(key+'.res.json')).read_text())
            bindings = NS['reflect'](key,'cs',source,resources)
            texture, = [b for b in bindings if b['kind'] == 'tex_ubo']
            sampler, = [b for b in bindings if b['kind'] == 'samp_ubo']
            self.assertEqual((texture['reg'],texture['off'],texture['sreg'],texture['soff']), (7,12,8,20))
            self.assertEqual((sampler['reg'],sampler['off'],sampler['sreg'],sampler['soff']), (8,20,7,12))
            vk.write_stage(key,binary.read_bytes(),dict(stage='cs',bindings=bindings),export,
                           shutil.which('spirv-val'),shutil.which('spirv-opt'))
            text = (export/(key+'.vk.txt')).read_text()
            self.assertIn('twin_tex=8:20',text); self.assertIn('twin_samp=7:12',text)

    def test_separate_sampler_keeps_primary_and_other_word(self):
        binding = dict(binding=1, type='samp', count=1, kind='samp_ubo', reg=8, off=20, sreg=7, soff=12)
        text = vk.vk_txt(dict(stage='ps', bindings=[binding]))
        self.assertIn('samp_ubo 8 20 - -1', text)
        self.assertIn('twin_samp=7:12', text)


if __name__ == '__main__': unittest.main()
