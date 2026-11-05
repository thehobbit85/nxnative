"""Full real-guest dimension queries retain the seven reflected image shapes."""
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
from dm.android.tests.texture_query_shapes_fixture import generate
from dm.android.tools import spv_android as vk


@unittest.skipUnless(shutil.which('spirv-val') and shutil.which('spirv-opt'), 'requires SPIR-V tools')
class TextureQueryShapes(unittest.TestCase):
    def test_full_query_shapes_and_sampled_1d_capability(self):
        with tempfile.TemporaryDirectory() as tmp:
            out=generate(Path(tmp))
            for variant,view in enumerate((0,4,1,5,2,3,6)):
                key=f'{0xfeed9600+variant:016x}'
                instructions=vk.decode((out/(key+'.spv')).read_bytes())[1]
                self.assertTrue(any(vk.opcode(i)==103 for i in instructions))  # ImageQuerySizeLod
                self.assertTrue(any(vk.opcode(i)==106 for i in instructions))  # ImageQueryLevels
                text=(out/(key+'.vk.txt')).read_text()
                self.assertIn(f'view={view} num=0',text)
                if variant<2:self.assertIn('cap 43\n',text)


if __name__ == '__main__': unittest.main()
