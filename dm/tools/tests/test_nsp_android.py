"""Android target routing and target-product isolation; no SDK or game data."""
import importlib.util
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('nsp_android', REPO / 'scripts/build_ipa_from_nsp.py')
assert spec and spec.loader
nsp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(nsp)


class AndroidTarget(unittest.TestCase):
    def test_cli_selects_android_and_rejects_xcode(self):
        args = nsp.parse_args(['--platform', 'android', '--prepare-xcode'])
        with self.assertRaisesRegex(nsp.BuildError, 'prepare-xcode'):
            nsp.main(args)

    def test_export_reuses_only_complete_unchanged_products(self):
        with tempfile.TemporaryDirectory() as tmp:
            sh = Path(tmp) / 'shader'; sh.mkdir()
            profile = Path(tmp) / 'profile'; profile.mkdir()
            key = '0123456789abcdef'
            (profile / 'shader_map.tsv').write_text(f'p.bin\t0\tvertex\t{key}\tsource\n')
            g = types.SimpleNamespace(table=lambda name: profile / name, dir=profile)
            calls = []
            def export(cmd, log, env=None):
                out = Path(cmd[3]); out.mkdir()
                (out / (key + '.spv')).write_bytes(b'shader')
                (out / (key + '.vk.txt')).write_text('stage vs\n')
                calls.append(cmd)
            with mock.patch.object(nsp, 'run', export), mock.patch.object(nsp, 'PROG', mock.Mock()), mock.patch.object(nsp, 'profile_files', return_value=[]):
                out = nsp.android_shaders(sys.executable, REPO, sh, {}, sh, g, 'source1')
                self.assertTrue((out / (key + '.spv')).is_file())
                nsp.android_shaders(sys.executable, REPO, sh, {}, sh, g, 'source1')
                self.assertEqual(len(calls), 1)
                (out / (key + '.spv')).write_bytes(b'corrupt')
                nsp.android_shaders(sys.executable, REPO, sh, {}, sh, g, 'source1')
                self.assertEqual(len(calls), 2)
                nsp.android_shaders(sys.executable, REPO, sh, {}, sh, g, 'source2')
                self.assertEqual(len(calls), 3)


if __name__ == '__main__':
    unittest.main()
