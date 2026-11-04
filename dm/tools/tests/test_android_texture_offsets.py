"""Exercise the bundled translator's Vulkan texture-offset lowering with synthetic Maxwell code."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

from test_txq_type import base, handle, keep, shader, SLOT_128, SAMPLER_1, DIM_2D

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'android/tools'))
import spv_android as vk


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE') and all(shutil.which(t) for t in ('spirv-val', 'spirv-opt')),
                     'requires DMSH_TEST_XLATE and SPIRV-Tools')
class TextureOffsets(unittest.TestCase):
    def test_dynamic_offset_lowered_for_vulkan_target(self):
        # TEX.B.LZ.AOFFI: R0/R1 coordinates, R6 handle, R7 packed signed offsets
        # from guest uniform slot 0. Keep its red result with a formatted image atomic.
        load_offset = base('Ldc') | (4 << 48) | (3 << 36) | (7 << 16) | (255 << 8) | 7
        sample = base('TexB') | (1 << 37) | (1 << 36) | (1 << 31) | (DIM_2D << 28) | (6 << 20) | (7 << 16) | 9
        code = shader(handle(6, SLOT_128, SAMPLER_1), load_offset, sample, keep(9))
        with tempfile.TemporaryDirectory(prefix='texture_offset_') as directory:
            work = Path(directory); binary = work / 'shader.bin'; binary.write_bytes(code)
            captured = work / 'captured'; captured.mkdir()
            for target in (None, 'vulkan', 'capture'):
                with self.subTest(target=target):
                    job = dict(key='sample', stage='cs', code=str(binary), tex={'128': '2d'},
                               img={'0': {'type': '2d', 'fmt': 'R32Uint'}}, local=[1, 1, 1], debug=True)
                    if target == 'vulkan': job['host_target'] = target
                    jobs = work / 'jobs.json'; jobs.write_text(json.dumps([job]))
                    if target == 'capture':
                        original = jobs.read_bytes()
                        with mock.patch.object(vk, 'XLATE', Path(os.environ['DMSH_TEST_XLATE'])), \
                             mock.patch.dict(os.environ, {'DMSH_SPV_CAPTURE': str(captured)}):
                            self.assertEqual(vk.capture([str(jobs), str(work)]), 0)
                        self.assertEqual(jobs.read_bytes(), original)
                        self.assertEqual((captured / 'sample.spv').read_bytes(), (work / 'sample.spv').read_bytes())
                    else:
                        result = subprocess.run([os.environ['DMSH_TEST_XLATE'], jobs, work], capture_output=True, text=True)
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    raw = (work / 'sample.spv').read_bytes()
                    samples = [i for i in vk.decode(raw)[1] if vk.opcode(i) in (87, 88)]
                    self.assertTrue(samples)
                    has_offset = any(len(i) > 5 and i[5] & 0x10 for i in samples)
                    self.assertEqual(has_offset, target is None)
                    prepared = work / 'prepared.spv'
                    prepared.write_bytes(vk.export_spirv(raw, shutil.which('spirv-opt')))
                    check = subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', prepared], capture_output=True, text=True)
                    if target:
                        self.assertEqual(check.returncode, 0, check.stdout + check.stderr)
                    else:
                        self.assertNotEqual(check.returncode, 0)
                        self.assertIn('Offset', check.stderr)


if __name__ == '__main__':
    unittest.main()
