"""Actual Maxwell LOD queries preserve signed fixed-point output conversion."""
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
from dm.android.tests.fragment_lod_query_fixture import generate
from dm.android.tools import spv_android as vk


@unittest.skipUnless(shutil.which('spirv-val') and shutil.which('spirv-opt'), 'requires SPIR-V tools')
class FragmentLodQuery(unittest.TestCase):
    def test_both_directions_keep_native_query_and_signed_conversions(self):
        with tempfile.TemporaryDirectory() as tmp:
            work=generate(Path(tmp))
            for variant in range(2):
                key=f'{0xfeed9700+variant:016x}'
                code=vk.decode((work/'vulkan/export'/(key+'.spv')).read_bytes())[1]
                self.assertTrue(any(vk.opcode(i)==105 for i in code))  # ImageQueryLod
                self.assertEqual(sum(vk.opcode(i)==110 for i in code),2)  # ConvertFToS
                self.assertEqual(sum(vk.opcode(i)==111 for i in code),2)  # ConvertSToF
                self.assertFalse(any(vk.opcode(i) in (87,88) for i in code))  # no texture sampling
                self.assertIn('stage ps\n',(work/'vulkan/export'/(key+'.vk.txt')).read_text())


if __name__ == '__main__': unittest.main()
