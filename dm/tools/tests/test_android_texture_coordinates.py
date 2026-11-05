"""Sampled 1D embedding and cube-array shapes survive actual guest export."""
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
from dm.android.tests.texture_coordinates_fixture import generate
from dm.android.tools import spv_android as vk


@unittest.skipUnless(shutil.which('spirv-val') and shutil.which('spirv-opt'), 'requires SPIR-V tools')
class TextureCoordinates(unittest.TestCase):
    def test_actual_normalized_and_cube_shapes(self):
        with tempfile.TemporaryDirectory() as tmp:
            out=generate(Path(tmp))
            for variant,view in enumerate((1,5,3,6)):
                key=f'{0xfeed9500+variant:016x}';raw=(out/(key+'.spv')).read_bytes()
                instructions=vk.decode(raw)[1]
                samples=[i for i in instructions if vk.opcode(i)==88]
                self.assertEqual(len(samples),1)
                self.assertEqual(samples[0][5],2)  # explicit Lod without an offset
                text=(out/(key+'.vk.txt')).read_text()
                self.assertIn(f'view={view} num=0',text)
                self.assertNotIn('fetch1d',text)  # normalized samples do not wrap texel-buffer rows


if __name__ == '__main__': unittest.main()
