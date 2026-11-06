"""Android target routing and target-product isolation; no SDK or game data."""
import importlib.util
import json
import os
from contextlib import nullcontext
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest import mock
from typing import Any

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('nsp_android', REPO / 'scripts/build_ipa_from_nsp.py')
assert spec and spec.loader
nsp: Any = importlib.util.module_from_spec(spec)
spec.loader.exec_module(nsp)


class AndroidTarget(unittest.TestCase):
    def test_geometry_reuse_covers_every_topology_product(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); profile = root / 'profile'; profile.mkdir()
            key = '0123456789abcdef'
            (profile / 'shader_map.tsv').write_text(f'gs.bin\t0\tgeometry\t{key}\tsource\n')
            g = types.SimpleNamespace(table=lambda name: profile / name, dir=profile)
            calls = []
            def export(cmd, log, env=None):
                out = Path(cmd[3]); out.mkdir()
                names = [key] + [key + f'.{kind}{t}' for kind in ('g', 'gc', 'gr') for t in range(5)]
                for name in names + [key + f'.gr{t}.vc' for t in range(5)]:
                    (out / (name + '.spv')).write_bytes(b'shader')
                    (out / (name + '.vk.txt')).write_text('stage gs\n')
                (out / 'export-contract.json').write_text('{}')
                calls.append(cmd)
            with mock.patch.object(nsp, 'run', export), mock.patch.object(nsp, 'PROG', mock.Mock()):
                sh = root / 'shader'
                out = nsp.android_shaders(sys.executable, REPO, sh, {}, sh, g, 'source')
                nsp.android_shaders(sys.executable, REPO, sh, {}, sh, g, 'source')
                self.assertEqual(len(calls), 1)
                (out / (key + '.g3.spv')).write_bytes(b'corrupt')
                nsp.android_shaders(sys.executable, REPO, sh, {}, sh, g, 'source')
                self.assertEqual(len(calls), 2)
                self.assertEqual(len(json.loads((out / 'manifest.json').read_text())['files']), 42)
                (out / (key + '.gc2.spv')).unlink()
                nsp.android_shaders(sys.executable, REPO, sh, {}, sh, g, 'source')
                self.assertEqual(len(calls), 3)
                (out / (key + '.gr4.vk.txt')).write_text('wrong reader layout')
                nsp.android_shaders(sys.executable, REPO, sh, {}, sh, g, 'source')
                self.assertEqual(len(calls), 4)
                (out / (key + '.gr2.vc.spv')).unlink()
                nsp.android_shaders(sys.executable, REPO, sh, {}, sh, g, 'source')
                self.assertEqual(len(calls), 5)

    def test_android_tool_failure_precedes_package_scan_conversion_and_survey(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); keys = root / 'keys'; keys.touch()
            args = nsp.parse_args([tmp, '--platform', 'android', '--keys', str(keys)])
            reader = types.SimpleNamespace(contents=None, check_keys=lambda fam: None)
            with mock.patch.object(nsp, 'check_native_python'), mock.patch.object(nsp, 'configure_dotnet'), \
                 mock.patch.object(nsp, 'package_title_reader', return_value=nullcontext(reader)), \
                 mock.patch.object(nsp, 'scan_nsps', side_effect=AssertionError('package scan began before Android preflight')), \
                 mock.patch.object(nsp, 'run', side_effect=nsp.BuildError('Android SDK unavailable')), \
                 mock.patch.dict(os.environ, {'ANDROID_NDK_HOME': tmp, 'ANDROID_SDK_ROOT': tmp}):
                with self.assertRaisesRegex(nsp.BuildError, 'Android SDK'): nsp.main(args)

    def test_child_dlc_reuses_application_indices_and_prepared_images(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); aoc = root / 'primary/aoc'; aoc.mkdir(parents=True)
            (aoc / '1.img').write_bytes(b'application DLC one')
            first = {'aoc': aoc}
            for index in (1, 2):
                child = root / str(index)
                result = nsp.extract_aoc('unused', 'unused', REPO, child, 0x0100633007D48000 + index,
                                         {'dlc': []}, child / 'logs', first=first)
                self.assertEqual((result / '1.img').read_bytes(), b'application DLC one')
                self.assertFalse(child.exists(), 'child re-extracted application DLC')
            self.assertIsNone(nsp.extract_aoc('unused', 'unused', REPO, root / '3', 3, {'dlc': []}, root, first={'aoc': None}))

    def test_catalog_binds_all_prepared_profiles_without_replacing_primary_inputs(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); profiles = root / 'selected'
            from dm.games import game
            for index in (0, 1, 2):
                profile = profiles / f'{0x0100633007D48000 + index:016X}'; profile.mkdir(parents=True)
                text = (REPO / 'dm/games/0100633007D48000/game.tsv').read_text()
                if not index: text += '\nprograms\t1=0100633007D48001 2=0100633007D48002\n'
                (profile / 'game.tsv').write_text(text)
            g = game.load(str(profiles / '0100633007D48000'))
            args = nsp.parse_args(['--platform', 'android', '--work-dir', tmp])
            first = {'build': root, 'profile': g.id, 'profile_dir': g.dir}
            def prepare(sub):
                index = sub.program; build = Path(sub.work_dir); build.mkdir(parents=True)
                (build / 'romfs').write_bytes(bytes([index]))
                return dict(build=build, profile=f'{0x0100633007D48000 + index:016X}', profile_dir=profiles / f'{0x0100633007D48000 + index:016X}',
                            patched=build / 'patched', romfs=build / 'romfs', aoc=None, gfx='nvn')
            env = {}
            with mock.patch.object(nsp, 'main', prepare): catalog, children = nsp.android_program_catalog(args, g, first, env)
            self.assertEqual([p['index'] for p in json.loads(catalog.read_text())['programs']], [1, 2])
            self.assertEqual([Path(children[i]['romfs']).read_bytes() for i in (1, 2)], [b'\x01', b'\x02'])
            with mock.patch.dict(os.environ, env):
                self.assertEqual(game.load(g.id).dir.resolve(), g.dir.resolve())
                self.assertEqual(game.load('0100633007D48001').dir.resolve(), (profiles / '0100633007D48001').resolve())
                self.assertIn('#define DM_GAME_MULTI 1', game.header(game.load(g.id)))

    def test_child_programs_use_distinct_build_folders_and_restore_program_index(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            args = nsp.parse_args(['--platform', 'android', '--work-dir', tmp, '--no-keep-extracted'])
            g = types.SimpleNamespace(get=lambda *a: '1=x 2=y', pairs=lambda key: [('1', 'x'), ('2', 'y')])
            (root / 'prepared').write_text('primary')
            def prepare(sub):
                nsp.PROGRAM_INDEX = sub.program
                work = Path(sub.work_dir); work.mkdir(parents=True, exist_ok=True)
                (work / 'prepared').write_text(str(sub.program))
                return {'build': work, 'keep': sub.keep_extracted}
            with mock.patch.object(nsp, 'main', prepare):
                parts = nsp.other_programs(args, g, {'build': root})
            self.assertEqual((root / 'prepared').read_text(), 'primary')
            self.assertEqual(len({str(parts[i]['build']) for i in (1, 2)}), 2)
            self.assertEqual(nsp.PROGRAM_INDEX, 0)

    def test_failed_child_preparation_restores_primary_program_selection(self):
        args = nsp.parse_args(['--platform', 'android'])
        g = types.SimpleNamespace(get=lambda *a: '1=x', pairs=lambda key: [('1', 'x')])
        def fail(sub):
            nsp.PROGRAM_INDEX = sub.program
            raise nsp.BuildError('child preparation failed')
        try:
            with mock.patch.object(nsp, 'main', fail), self.assertRaisesRegex(nsp.BuildError, 'child preparation'):
                nsp.other_programs(args, g, {'build': Path('/tmp/unused-fixture')})
            self.assertEqual(nsp.PROGRAM_INDEX, 0)
        finally: nsp.PROGRAM_INDEX = 0

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

    def test_shader_reuse_hashes_lowering_tools_helpers_and_effective_options(self):
        # A cached export is observable output: a changed compiler input must
        # replace it, even when a DLL keeps the same size and mtime.
        cases = ('optimizer', 'validator', 'dll_same_stat', 'helper', 'source', 'debug', 'precision', 'target_env', 'geometry_contract', 'mesh_contract', 'uniform_address', 'split_handle', 'driver_c2', 'driver_c0', 'resource_contract', 'driver_c2_modes', 'warp_width')
        for change in cases:
            with self.subTest(change=change), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp); repo = root / 'repo'; shader = root / 'shader'; shader.mkdir()
                profile = root / 'profile'; profile.mkdir(); key = '0123456789abcdef'
                (profile / 'shader_map.tsv').write_text(f'p.bin\t0\tvertex\t{key}\tsource\n')
                g = types.SimpleNamespace(table=lambda name, profile=profile: profile / name, dir=profile)
                inputs = {
                    'dm/gfx/tools/dmsh_build.py': 'import dmsh_fetch\n',
                    'dm/gfx/tools/dmsh_fetch.py': 'value = 1\n',
                    'dm/gfx/tools/dmsh_chain.lock': 'locked translator\n',
                    'dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate.dll': 'binary0',
                    'dm/gfx/tools/dmsh_xlate/Program.cs': '// translator0',
                    'dm/android/tools/spv_android.py': 'import spv_metadata\n',
                    'dm/android/tools/spv_metadata.py': 'value = 1\n',
                    'dm/android/tools/target_contract.py': 'schema = 2\n',
                    'dm/gfx/nvn/vulkan/geometry_contract.h': '// reader0',
                    'dm/gfx/nvn/vulkan/meshstage_contract.h': '// mesh0',
                    'dm/gfx/nvn/vulkan/uniform_address.h': '// address0',
                    'dm/gfx/nvn/vulkan/split_handle.h': '// split0',
                    'dm/gfx/nvn/vulkan/driver_c2.h': '// c2v0',
                    'dm/gfx/nvn/vulkan/driver_c0.h': '// c0v0',
                    'dm/gfx/nvn/vulkan/resource_contract.h': '// resources0',
                    'dm/gfx/nvn/vulkan/driver_c2_modes.h': '// modes0',
                    'dm/gfx/nvn/vulkan/warp_width.h': '// width0',
                }
                for name, content in inputs.items():
                    path = repo / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_text(content)
                tool_dir = root / 'tools'; tool_dir.mkdir()
                for name in ('spirv-opt', 'spirv-val'):
                    path = tool_dir / name; path.write_text('tool0'); path.chmod(0o700)
                env = dict(PATH=str(tool_dir)); state = {'revision': b'first output'}
                def exporter(cmd, log, env=None, key=key, state=state):
                    out = Path(cmd[3]); out.mkdir()
                    for name in (key, key + '.vc'):
                        (out / (name + '.spv')).write_bytes(state['revision'])
                        (out / (name + '.vk.txt')).write_text('stage vs\n')
                    (out / 'export-contract.json').write_text('{}')
                with mock.patch.object(nsp, 'run', exporter), mock.patch.object(nsp, 'PROG', mock.Mock()):
                    result = nsp.android_shaders(sys.executable, repo, shader, env, shader, g, 'package bytes')
                    self.assertEqual((result / (key + '.spv')).read_bytes(), b'first output')
                    state['revision'] = b'rebuilt output'
                    if change in ('optimizer', 'validator'):
                        (tool_dir / ('spirv-opt' if change == 'optimizer' else 'spirv-val')).write_text('tool1')
                    elif change == 'dll_same_stat':
                        path = repo / 'dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate.dll'; before = path.stat()
                        path.write_text('binary1'); os.utime(path, ns=(before.st_atime_ns, before.st_mtime_ns))
                    elif change == 'helper': (repo / 'dm/android/tools/spv_metadata.py').write_text('value = 2\n')
                    elif change == 'source': (repo / 'dm/gfx/tools/dmsh_xlate/Program.cs').write_text('// translator1')
                    elif change == 'debug': env['XLATE_DEBUG'] = '0'
                    elif change == 'precision': env['XLATE_REDUCED'] = '0'
                    elif change == 'target_env': env['SPIRV_CROSS'] = str(tool_dir / 'alternate-cross')
                    elif change == 'geometry_contract': (repo / 'dm/gfx/nvn/vulkan/geometry_contract.h').write_text('// reader1')
                    elif change == 'mesh_contract': (repo / 'dm/gfx/nvn/vulkan/meshstage_contract.h').write_text('// mesh1')
                    elif change == 'uniform_address': (repo / 'dm/gfx/nvn/vulkan/uniform_address.h').write_text('// address1')
                    elif change == 'split_handle': (repo / 'dm/gfx/nvn/vulkan/split_handle.h').write_text('// split1')
                    elif change == 'driver_c2': (repo / 'dm/gfx/nvn/vulkan/driver_c2.h').write_text('// c2v1')
                    elif change == 'driver_c0': (repo / 'dm/gfx/nvn/vulkan/driver_c0.h').write_text('// c0v1')
                    elif change == 'resource_contract': (repo / 'dm/gfx/nvn/vulkan/resource_contract.h').write_text('// resources1')
                    elif change == 'warp_width': (repo / 'dm/gfx/nvn/vulkan/warp_width.h').write_text('// width1')
                    elif change == 'driver_c2_modes': (repo / 'dm/gfx/nvn/vulkan/driver_c2_modes.h').write_text('// modes1')
                    result = nsp.android_shaders(sys.executable, repo, shader, env, shader, g, 'package bytes')
                    self.assertEqual((result / (key + '.spv')).read_bytes(), b'rebuilt output')

    def test_changed_export_inputs_preserve_previous_products(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); shader = root / 'shader'; shader.mkdir()
            profile = root / 'profile'; profile.mkdir(); key = '0123456789abcdef'
            (profile / 'shader_map.tsv').write_text(f'p.bin\t0\tvertex\t{key}\tsource\n')
            inputs = profile / 'shader_facts.tsv'; inputs.write_text('first facts')
            g = types.SimpleNamespace(table=lambda name: profile / name, dir=profile)
            mutate = False
            def exporter(cmd, log, env=None):
                out = Path(cmd[3]); out.mkdir()
                for name in (key, key + '.vc'):
                    (out / (name + '.spv')).write_bytes(b'unstable' if mutate else b'original')
                    (out / (name + '.vk.txt')).write_text('stage vs\n')
                (out / 'export-contract.json').write_text('{}')
                if mutate: inputs.write_text('changed during translation')
            with mock.patch.object(nsp, 'run', exporter), mock.patch.object(nsp, 'PROG', mock.Mock()):
                result = nsp.android_shaders(sys.executable, REPO, shader, {}, shader, g, 'first package')
                mutate = True
                with self.assertRaisesRegex(nsp.BuildError, 'inputs changed'):
                    nsp.android_shaders(sys.executable, REPO, shader, {}, shader, g, 'new package')
                self.assertEqual((result / (key + '.spv')).read_bytes(), b'original')

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
                for name in (key, key + '.vc'):
                    (out / (name + '.spv')).write_bytes(b'shader')
                    (out / (name + '.vk.txt')).write_text('stage vs\n')
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
                (out / (key + '.g0.spv')).write_bytes(b'unclaimed variant')
                nsp.android_shaders(sys.executable, REPO, sh, {}, sh, g, 'source2')
                self.assertEqual(len(calls), 4)
                self.assertFalse((out / (key + '.g0.spv')).exists())
                (out / (key + '.vc.spv')).unlink()
                nsp.android_shaders(sys.executable, REPO, sh, {}, sh, g, 'source2')
                self.assertEqual(len(calls), 5)
                self.assertEqual(len(json.loads((out / 'manifest.json').read_text())['files']), 4)
                (out / (key + '.vc.vk.txt')).write_text('wrong capture interface')
                nsp.android_shaders(sys.executable, REPO, sh, {}, sh, g, 'source2')
                self.assertEqual(len(calls), 6)


if __name__ == '__main__':
    unittest.main()
