"""Array/volume sample offsets retain shape and the complete bounded tuple space."""
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
from dm.android.tests.texture_dimensions_fixture import generate
from dm.android.tools import spv_android as vk


@unittest.skipUnless(shutil.which('spirv-val') and shutil.which('spirv-opt'), 'requires SPIR-V tools')
class TextureDimensions(unittest.TestCase):
    def test_actual_guest_array_and_volume_shapes(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = generate(Path(tmp))
            for variant, view, samples in ((0, 5, 256), (1, 2, 4096)):
                key = f'{0xfeed9300 + variant:016x}'
                code = vk.decode((out / (key + '.spv')).read_bytes())[1]
                operations = [i for i in code if vk.opcode(i) == 88]
                self.assertEqual(len(operations), samples)
                self.assertTrue(all(i[5] == 10 for i in operations))  # Lod | ConstOffset
                self.assertIn(f'view={view} num=0', (out / (key + '.vk.txt')).read_text())


if __name__ == '__main__': unittest.main()
