"""Compact guest texture families retain both destination groups and half packing."""
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT))
from dm.android.tests.compact_texture_fixture import VARIANTS,generate
from dm.android.tools import spv_android as vk


@unittest.skipUnless(shutil.which('spirv-val') and shutil.which('spirv-opt'),'requires SPIR-V tools')
class CompactTexture(unittest.TestCase):
    def test_masks_and_half_packing_of_actual_guest_exports(self):
        self.assertEqual(len(VARIANTS),68)
        with tempfile.TemporaryDirectory() as tmp:
            out=generate(Path(tmp))
            for variant,case in enumerate(VARIANTS):
                key=f'{0xfeed9900+variant:016x}';code=vk.decode((out/(key+'.spv')).read_bytes())[1]
                operation={'Texs':88,'Tlds':95,'Tld4s':96}[case['op']]
                self.assertEqual(sum(vk.opcode(i)==operation for i in code),1)
                raw=vk.decode((Path(tmp)/'vulkan'/(key+'.spv')).read_bytes())[1]
                packs=[i for i in raw if vk.opcode(i)==12 and i[4]==58]
                self.assertEqual(len(packs),(2 if case['dual'] else 1) if case['half'] else 0)
                # Vulkan's native pack instruction has driver-defined rounding.
                # Android must explicitly preserve the Apple RNE conversion.
                self.assertFalse(any(vk.opcode(i)==12 and i[4]==58 for i in code))
                text=(out/(key+'.vk.txt')).read_text()
                self.assertIn('sbuf_ubo 0 56',text)
                self.assertIn('tex 0 -1 samp_tex 0 view=1 num=0',text)


if __name__=='__main__':unittest.main()
