"""Actual guest loads keep their distinct bank source and serialized inline bytes."""
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
from dm.android.tests.constant_banks_fixture import BANKS, generate, pattern


@unittest.skipUnless(shutil.which('spirv-val') and shutil.which('spirv-opt'), 'requires SPIR-V tools')
class ConstantBankExports(unittest.TestCase):
    def test_actual_widths_retain_bank_sources_and_inline_payload(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp); export = generate(work)
            self.assertEqual(len(list(export.glob('*.spv'))), 36)
            for variant in range(36):
                bank = BANKS[variant//6]; key = f'{0xfeed9000+variant:016x}'
                text = (export / (key + '.vk.txt')).read_text()
                source = f'ubo 1 cbuf {bank-3} -1' if bank in (4,16) else 'ubo 1 c1 -1 1' if bank == 1 else f'ubo 1 drvcb -1 {bank}'
                self.assertIn(source, text, (bank, variant%6))
                payload = next(line[3:] for line in text.splitlines() if line.startswith('c1 '))
                self.assertEqual(bytes.fromhex(payload), bytes(pattern(1,i) for i in range(256)))
                reflection = json.loads((work / (key + '.reflection.json')).read_text())
                self.assertEqual(reflection['c1'], payload)


if __name__ == '__main__': unittest.main()
