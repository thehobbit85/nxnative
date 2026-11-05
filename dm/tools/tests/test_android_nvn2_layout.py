"""The actual Ampere layout retains distinct C0, C2, and rounded push sources."""
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
from dm.android.tests.nvn2_layout_fixture import generate, KEY, OFFSETS


@unittest.skipUnless(shutil.which('spirv-val') and shutil.which('spirv-opt'), 'requires SPIR-V tools')
class Nvn2LayoutExports(unittest.TestCase):
    def test_all_regions_retain_driver_sources_and_push_rounding_changes_code(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp); positive = generate(work / 'positive'); negative = generate(work / 'negative', True)
            self.assertEqual(len(OFFSETS), 56)
            self.assertEqual(OFFSETS[-2:], (0xd8, 0x200))
            reflection = json.loads((work / 'positive' / (KEY + '.reflection.json')).read_text())
            bindings = reflection['bindings']
            self.assertEqual({b['bank'] for b in bindings if b['kind'] == 'drvcb'}, {0, 2, 24})
            self.assertEqual([(b['reg'], b['off']) for b in bindings if b['kind'] == 'sbuf_ubo'], [(0, 56)])
            text = (positive / (KEY + '.vk.txt')).read_text()
            for bank in (0, 2, 24): self.assertIn(f'drvcb -1 {bank}', text)
            self.assertNotEqual((positive / (KEY + '.spv')).read_bytes(), (negative / (KEY + '.spv')).read_bytes())


if __name__ == '__main__': unittest.main()
