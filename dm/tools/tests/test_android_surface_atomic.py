"""Verify both result-carrying image atomics survive production Android export."""
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
from dm.android.tests.surface_atomic_fixture import generate
from dm.android.tools.spv_module import decode, opcode
import json


@unittest.skipUnless(shutil.which('spirv-val') and shutil.which('spirv-opt'), 'requires SPIR-V tools')
class SurfaceAtomic(unittest.TestCase):
    def test_guarded_return_and_cas(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp); out = generate(work)
            cases = json.loads((work/'cases.json').read_text()); self.assertEqual(len(cases), 27)
            for row in cases:
                key = row['key']; code = decode((out/(key+'.spv')).read_bytes())[1]
                ops = [opcode(x) for x in code]
                self.assertIn(239 if row['compare'] is None else 230, ops)  # UMax or CompareExchange
                self.assertIn(234, ops)  # a second atomic consumes the returned texel
                self.assertIn(245, ops)  # the Ign bounds guard returns zero on the invalid edge
                self.assertIn(f'FILE "{key}.spv"', (out/(key+'.amber')).read_text())


if __name__ == '__main__': unittest.main()
