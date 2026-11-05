"""Actual Maxwell draw attributes retain their driver data and draw-parameter requirement."""
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
from dm.android.tests.draw_ids_fixture import generate, KEY
from dm.android.tools import spv_android as vk


@unittest.skipUnless(shutil.which('spirv-val') and shutil.which('spirv-opt'), 'requires SPIR-V tools')
class GuestDrawIds(unittest.TestCase):
    def test_actual_attributes_keep_builtins_c0_and_storage_source(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp); out = generate(work / 'positive'); wrong = generate(work / 'negative', True)
            raw = (out / (KEY + '.spv')).read_bytes()
            instructions = vk.decode(raw)[1]
            builtins = {i[3] for i in instructions if vk.opcode(i) == 71 and len(i) == 4 and i[2] == 11}
            self.assertTrue({42, 43, 4425} <= builtins)  # VertexIndex, InstanceIndex, BaseInstance
            reflection = json.loads((work / 'positive' / (KEY + '.reflection.json')).read_text())
            self.assertEqual([(b['kind'], b.get('bank'), b.get('reg'), b.get('off')) for b in reflection['bindings']],
                             [('drvcb', 0, None, None), ('sbuf_ubo', None, 0, 56)])
            text = (out / (KEY + '.vk.txt')).read_text()
            self.assertIn('stage vs\n', text); self.assertIn('cap 4427\n', text)
            self.assertIn('drvcb -1 0', text); self.assertIn('sbuf_ubo 0 56', text)
            self.assertTrue((out / (KEY + '.vc.spv')).is_file())
            self.assertNotEqual(raw, (wrong / (KEY + '.spv')).read_bytes())


if __name__ == '__main__': unittest.main()
