"""Preserve every logical driver handle and its optional fixed sampler source."""
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]; sys.path.insert(0, str(ROOT))
from dm.android.tools import spv_android as vk
from dm.android.tests.c2_array_fixture import generate


class DriverArrayMetadata(unittest.TestCase):
    def test_array_and_fixed_sampler_source_survive(self):
        binding = dict(binding=2, type='cis', count=301, kind='tex_c2a', off=0,
                       samp=dict(kind='samp_c2a', off=0, soff=0x570))
        try: text = vk.vk_txt(dict(stage='cs', bindings=[binding]))
        except ValueError as error: self.fail(f'driver arrays must export: {error}')
        self.assertIn('b 2 cis 301 tex_c2a -1 0 samp_c2a -1 soff=0 c2_or=1392', text)

    def test_separate_sampler_and_invalid_offsets(self):
        binding = dict(binding=3, type='samp', count=301, kind='samp_c2a', off=8, soff=0)
        self.assertIn('b 3 samp 301 samp_c2a -1 8 - -1 c2_or=0', vk.vk_txt(dict(stage='cs', bindings=[binding])))
        for field in ('off', 'soff'):
            for bad in (None, -1, 2**31, True):
                with self.subTest(field=field, value=bad), self.assertRaisesRegex(ValueError, 'offset'):
                    vk.vk_txt(dict(stage='cs', bindings=[dict(binding, **{field: bad})]))


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE') and all(shutil.which(t) for t in ('spirv-opt', 'spirv-val')),
                     'requires the locked translator and SPIRV-Tools')
class TranslatedDriverArrays(unittest.TestCase):
    def test_both_maxwell_forms_keep_all_301_elements(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            try: export = generate(work)
            except ValueError as error: self.fail(f'real Maxwell driver arrays must export: {error}')
            for separate in (0, 1):
                key = f'{0xfeed4000+separate:016x}'
                reflection = json.loads((work / (key + '.reflection.json')).read_text())
                array, = [b for b in vk.vk_reflect((export / (key + '.spv')).read_bytes(), reflection) if b['kind'] == 'tex_c2a']
                self.assertEqual((array['count'], array['off'], array['samp']['off']), (301, 0, 0))
                self.assertEqual(array['samp'].get('soff'), 0x570 if separate else None)
                text = (export / (key + '.vk.txt')).read_text()
                self.assertIn('cis 301 tex_c2a -1 0 samp_c2a -1 soff=0', text)
                self.assertEqual('c2_or=1392' in text, bool(separate))


if __name__ == '__main__': unittest.main()
