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
    def test_android_metadata_options_are_checked_before_extraction(self):
        for options, error in ((['--platform', 'ios', '--android-keystore', 'key'], 'Android'),
                               (['--platform', 'ios', '--android-version-code', '7'], 'Android'),
                               (['--platform', 'android', '--android-version-code', '0'], 'version code')):
            with self.subTest(options=options):
                args = nsp.parse_args(options)
                with self.assertRaisesRegex(nsp.BuildError, error): nsp.main(args)

    def test_cli_selects_android_and_rejects_xcode(self):
        args = nsp.parse_args(['--platform', 'android', '--prepare-xcode'])
        with self.assertRaisesRegex(nsp.BuildError, 'prepare-xcode'):
            nsp.main(args)

    def test_android_fonts_are_prepared_on_a_fresh_build(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); standard = root / 'source.ttf'; standard.write_bytes(b'standard')
            def generate(cmd, log, env=None):
                Path(cmd[2]).write_bytes(b'icons')
            with mock.patch.object(nsp, 'run', generate):
                font = nsp.android_fonts(sys.executable, REPO, root, {'STD_FONT': str(standard)}, root)
            self.assertEqual((font / 'standard.ttf').read_bytes(), b'standard')
            self.assertEqual((font / 'nintendo_ext.ttf').read_bytes(), b'icons')

    def test_alias_build_keeps_profile_and_application_ids_separate(self):
        spec = importlib.util.spec_from_file_location('android_wrapper', REPO / 'scripts/build_android.py')
        assert spec and spec.loader
        module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(module.subprocess, 'run') as run, mock.patch.object(sys, 'argv',
                ['build_android.py', '0100000000000002', tmp, tmp + '/out.apk', '--profile', '0100000000000001', '--ndk', tmp]):
            host = 'darwin-x86_64' if sys.platform == 'darwin' else 'linux-x86_64'
            native_tools = Path(tmp) / 'toolchains/llvm/prebuilt' / host / 'bin'; native_tools.mkdir(parents=True)
            for name in ('aarch64-linux-android30-clang', 'llvm-ar'):
                path = native_tools / name; path.write_text('#!/bin/sh\nexit 0\n'); path.chmod(0o755)
            module.main()
            preflight, native, apk = [c.args[0] for c in run.call_args_list]
            self.assertIn('--check-tools', preflight)
            self.assertIn('GAME=0100000000000001', native)
            self.assertEqual(apk[2], '0100000000000002')
            self.assertIn('0100000000000001/libdmhost.so', apk[apk.index('--library') + 1])

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
                (out / 'export-contract.json').write_text('{}')
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
