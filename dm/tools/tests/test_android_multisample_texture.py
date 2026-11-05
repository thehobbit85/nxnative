"""Guest multisample operations keep sample indexing and array shape."""
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
from dm.android.tests.multisample_texture_fixture import generate
from dm.android.tools import spv_android as vk


@unittest.skipUnless(shutil.which('spirv-val') and shutil.which('spirv-opt'), 'requires SPIR-V tools')
class MultisampleTexture(unittest.TestCase):
    def test_real_fetches_and_sample_count_queries(self):
        with tempfile.TemporaryDirectory() as tmp:
            out=generate(Path(tmp))
            for variant in range(4):
                key=f'{0xfeed9800+variant:016x}';code=vk.decode((out/(key+'.spv')).read_bytes())[1]
                image_types=[i for i in code if vk.opcode(i)==25]
                self.assertEqual(len(image_types),1)
                self.assertEqual((image_types[0][3],image_types[0][5],image_types[0][6]),(1,variant//2,1))
                if variant%2:
                    self.assertEqual(sum(vk.opcode(i)==107 for i in code),1)
                else:
                    fetch=[i for i in code if vk.opcode(i)==95]
                    self.assertEqual(len(fetch),1)
                    self.assertEqual(fetch[0][5],64)  # Sample operand, no LOD operand
                self.assertIn(f'view={5 if variant//2 else 1} num=0',(out/(key+'.vk.txt')).read_text())


if __name__=='__main__': unittest.main()
