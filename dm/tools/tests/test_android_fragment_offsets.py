"""Real fragment offsets must reach exact lowering without changing derivatives."""
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
from dm.android.tests.fragment_texture_fixture import generate
from dm.android.tools.spv_module import decode, opcode


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE') and all(shutil.which(t) for t in ('spirv-val', 'spirv-opt')),
                     'requires the locked translator and SPIRV-Tools')
class FragmentOffsets(unittest.TestCase):
    def test_implicit_and_bias_samples_keep_the_original_derivative_context(self):
        with tempfile.TemporaryDirectory(prefix='fragment_offsets_') as directory:
            work = generate(Path(directory))
            for target in ('apple', 'vulkan'):
                for bias in (False, True):
                    with self.subTest(target=target, bias=bias):
                        key = f'{0xfeed8000 + bias:016x}'
                        raw = decode((work / target / (key + '.spv')).read_bytes())[1]
                        samples = [i for i in raw if opcode(i) == 87]
                        self.assertEqual(len(samples), 1)
                        self.assertEqual(samples[0][5], 0x10 | int(bias))
                        exported = decode((work / target / 'export' / (key + '.spv')).read_bytes())[1]
                        samples = [i for i in exported if opcode(i) == 87]
                        self.assertEqual(len(samples), 256)
                        self.assertTrue(all(i[5] == 8 | int(bias) for i in samples))
                        self.assertFalse(any(opcode(i) in (105, 250, 251) for i in exported),
                                         'no LOD query or varying branch may replace implicit derivatives')


if __name__ == '__main__': unittest.main()
