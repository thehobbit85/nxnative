"""Producer contracts bind prepared Android inputs to their actual host/exporter."""
import importlib.util
from pathlib import Path
import json
import os
import shutil
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location('target_contract', ROOT / 'dm/android/tools/target_contract.py')
contract = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(contract)


class TargetContract(unittest.TestCase):
    def multi_fixture(self, root):
        a, files = self.fixture(root)
        child = root / '0100633007D48001'; shutil.copytree(a.profile, child)
        with (Path(a.profile) / 'game.tsv').open('a') as stream: stream.write('programs\t1=0100633007D48001\n')
        child_build = root / 'child'; child_build.mkdir()
        (child_build / 'shader/vk').mkdir(parents=True)
        copied = []
        for name, src in files:
            dest = child_build / Path(src).name; shutil.copyfile(src, dest)
            copied.append(('programs/1/' + name, str(dest)))
        catalog = root / 'programs.json'
        catalog.write_text(json.dumps({'schema': 1, 'programs': [{'index': 1, 'profile': child.name, 'build': 'child', 'gfx': 'nvn'}]}))
        a.programs = str(catalog)
        return a, files + copied, child

    def test_every_declared_program_has_its_own_modules_and_portable_identity(self):
        with tempfile.TemporaryDirectory() as tmp, patch.dict(os.environ, {'DM_GAMES_DIR': tmp, 'DM_GAMES_FIRST': '1'}):
            root = Path(tmp); a, files, child = self.multi_fixture(root)
            native = contract.host_contract(a.profile)
            lib = root / 'lib.so'; lib.write_bytes(contract.START + native + contract.END)
            target = contract.target_manifest(a, lib, files).decode()
            self.assertIn('program_profile\t1\t' + child.name + '\n', native.decode())
            self.assertIn('program\t1\t' + child.name + '\tprograms/1\n', target)
            self.assertIn('module\t1/main\t', target)
            self.assertNotIn(tmp, target)
            with self.assertRaisesRegex(ValueError, 'main module'):
                contract.target_manifest(a, lib, files[:2])
            a.programs = None
            with self.assertRaisesRegex(ValueError, 'declared programs'):
                contract.target_manifest(a, lib, files)

    def test_shader_producers_are_per_program_but_native_producer_covers_all(self):
        with tempfile.TemporaryDirectory() as tmp, patch.dict(os.environ, {'DM_GAMES_DIR': tmp, 'DM_GAMES_FIRST': '1'}):
            root = Path(tmp); a, _, child = self.multi_fixture(root)
            # Export before child preparation must not be invalidated by a
            # later child profile derivation; the final host must see it.
            contract.stamp_export(root / 'shader/vk', a.profile)
            first = (root / 'shader/vk/export-contract.json').read_bytes()
            native = contract.host_contract(a.profile)
            with (child / 'game.tsv').open('a') as stream: stream.write('# derived child\n')
            contract.stamp_export(root / 'shader/vk', a.profile)
            self.assertEqual(first, (root / 'shader/vk/export-contract.json').read_bytes())
            self.assertNotEqual(native, contract.host_contract(a.profile))

    def test_child_shader_proof_and_catalog_cannot_be_silently_omitted(self):
        with tempfile.TemporaryDirectory() as tmp, patch.dict(os.environ, {'DM_GAMES_DIR': tmp, 'DM_GAMES_FIRST': '1'}):
            root = Path(tmp); a, files, child = self.multi_fixture(root)
            lib = root / 'lib.so'; lib.write_bytes(contract.START + contract.host_contract(a.profile) + contract.END)
            shader = root / 'child/shader/vk/a.spv'; shader.write_bytes(b'child shader')
            files.append(('programs/1/vk/a.spv', str(shader)))
            with self.assertRaisesRegex(ValueError, 'shader.*export'): contract.target_manifest(a, lib, files)
            contract.stamp_export(shader.parent, str(child))
            self.assertIn(b'program_shader\t1\t', contract.target_manifest(a, lib, files))
            shader.write_bytes(b'corrupt')
            with self.assertRaisesRegex(ValueError, 'shader.*export'): contract.target_manifest(a, lib, files)
            catalog = Path(a.programs); original = json.loads(catalog.read_text())
            for changes in ({'index': 0}, {'index': True}, {'index': 2147483648}, {'profile': Path(a.profile).name}, {'gfx': 'gl'}, {'build': ''}):
                with self.subTest(changes=changes):
                    catalog.write_text(json.dumps({'schema': 1, 'programs': [dict(original['programs'][0], **changes)]}))
                    with self.assertRaises(ValueError): contract.target_manifest(a, lib, files[:4])
            catalog.write_text(json.dumps({'schema': 1, 'programs': original['programs'] * 2}))
            with self.assertRaisesRegex(ValueError, 'unique'): contract.target_manifest(a, lib, files[:4])

    def fixture(self, root):
        profile = root / '0100633007D48000'; profile.mkdir()
        (profile / 'game.tsv').write_text((ROOT / 'dm/games/0100633007D48000/game.tsv').read_text().replace('modules\tmain subsdk0 sdk', 'modules\tmain'))
        module = root / 'patched_android'; module.mkdir()
        binary = module / 'main.patched.bin'; binary.write_bytes(b'code')
        fixups = module / 'main.fixups.tsv'; fixups.write_text('# TP_SLOT\t2\n# BUILD_ID\t' + '0123456789abcdef' * 2 + '\n')
        shaders = root / 'shader/vk'; shaders.mkdir(parents=True)
        files = [('dm/main.patched.bin', str(binary)), ('dm/main.fixups.tsv', str(fixups))]
        a = SimpleNamespace(id='0100633007D48000', profile=str(profile), build=str(root))
        return a, files

    def test_manifest_identifies_real_host_modules_and_inputs_without_local_paths(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); a, files = self.fixture(root)
            native = contract.host_contract(a.profile)
            lib = root / 'lib.so'; lib.write_bytes(b'ELF fixture' + contract.START + native + contract.END)
            target = contract.target_manifest(a, lib, files).decode()
            self.assertTrue(target.startswith(native.decode()))
            self.assertIn('title\t' + a.id + '\n', target)
            self.assertIn('module\tmain\t' + '0123456789abcdef' * 2, target)
            self.assertIn('host_sha256\t' + contract.digest(lib), target)
            self.assertIn('asset\tdm/main.patched.bin\t4\t', target)
            self.assertNotIn(tmp, target)
            with (Path(a.profile) / 'game.tsv').open('a') as stream: stream.write('# changed profile input\n')
            with self.assertRaisesRegex(ValueError, 'host.*rebuild'): contract.target_manifest(a, lib, files)

    def test_native_and_capture_producers_track_exporter_helper_changes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); a, _ = self.fixture(root); repo = root / 'repo'
            tools = repo / 'dm/android/tools'; tools.mkdir(parents=True)
            (tools / 'spv_android.py').write_text('import spv_metadata\n')
            helper = tools / 'spv_metadata.py'; helper.write_text('schema = 1\n')
            header = repo / 'dm/gfx/nvn/dmsh_chain.h'; header.parent.mkdir(parents=True)
            header.write_text('#define DMSH_CHAIN "012345abcdef"\n')
            with patch.object(contract, 'ROOT', repo):
                before = contract.host_contract(a.profile)
                helper.write_text('schema = 2\n')
                self.assertNotEqual(before, contract.host_contract(a.profile))
                before = contract.host_contract(a.profile)
                native = repo / 'dm/gfx/nvn/vulkan/geometry_contract.h'; native.parent.mkdir()
                native.write_text('// changed geometry consumer\n')
                self.assertNotEqual(before, contract.host_contract(a.profile))

    def test_exporter_uses_the_selected_environment_profile(self):
        with tempfile.TemporaryDirectory() as tmp:
            a, _ = self.fixture(Path(tmp))
            with patch.dict(os.environ, {'DM_GAME': a.profile}):
                self.assertEqual(contract.host_contract(), contract.host_contract(a.profile))

    def test_profile_contract_is_independent_of_checkout_location(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); a, _ = self.fixture(root)
            copy = root / 'moved checkout' / Path(a.profile).name
            shutil.copytree(a.profile, copy)
            self.assertEqual(contract.host_contract(a.profile), contract.host_contract(str(copy)))

    def test_missing_duplicate_or_wrong_host_contract_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); a, files = self.fixture(root); native = contract.host_contract(a.profile)
            for content in (b'legacy library', contract.START + native.replace(b'tp_slot\t2', b'tp_slot\t3') + contract.END,
                            (contract.START + native + contract.END) * 2):
                lib = root / 'lib.so'; lib.write_bytes(content)
                with self.assertRaisesRegex(ValueError, 'host.*rebuild'): contract.target_manifest(a, lib, files)

    def test_shader_provenance_must_match_contents_and_producer(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); a, files = self.fixture(root); shader = root / 'shader/vk/a.spv'; shader.write_bytes(b'spv')
            sidecar = shader.with_suffix('.vk.txt'); sidecar.write_text('stage vs\n')
            files += [('vk/a.spv', str(shader)), ('vk/a.vk.txt', str(sidecar))]
            lib = root / 'lib.so'; lib.write_bytes(contract.START + contract.host_contract(a.profile) + contract.END)
            with self.assertRaisesRegex(ValueError, 'shader.*export'): contract.target_manifest(a, lib, files)
            contract.stamp_export(shader.parent, a.profile)
            target = contract.target_manifest(a, lib, files)
            self.assertIn(b'shader_export\t', target)
            shader.write_bytes(b'bad')
            with self.assertRaisesRegex(ValueError, 'shader.*export'): contract.target_manifest(a, lib, files)
            shader.write_bytes(b'spv')
            path = shader.parent / 'export-contract.json'; saved = json.loads(path.read_text()); saved['producer'] = '0' * 64; path.write_text(json.dumps(saved))
            with self.assertRaisesRegex(ValueError, 'shader.*export'): contract.target_manifest(a, lib, files)

    def test_required_modules_follow_the_compiled_profile_build_selection(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); a, files = self.fixture(root); profile = Path(a.profile) / 'game.tsv'
            profile.write_text(profile.read_text().replace('modules\tmain\n', 'modules\tmain sdk\n'))
            lib = root / 'lib.so'; lib.write_bytes(contract.START + contract.host_contract(a.profile) + contract.END)
            with self.assertRaisesRegex(ValueError, 'required.*sdk'): contract.target_manifest(a, lib, files)
            # The host narrows its module list when this exact main build is
            # registered with fewer modules, so packaging must use that rule too.
            with profile.open('a') as stream: stream.write('build\tv1\t1.0\tmain=' + '0123456789abcdef' * 2 + '\n')
            lib.write_bytes(contract.START + contract.host_contract(a.profile) + contract.END)
            contract.target_manifest(a, lib, files)

    def test_export_does_not_stamp_changed_profile_inputs_as_the_producer(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); a, _ = self.fixture(root)
            before = contract.host_contract(a.profile)
            with (Path(a.profile) / 'game.tsv').open('a') as stream: stream.write('# input changed during export\n')
            with self.assertRaisesRegex(ValueError, 'inputs changed'):
                contract.stamp_export(root / 'shader/vk', a.profile, producer=before)
            self.assertFalse((root / 'shader/vk/export-contract.json').exists())

    def test_module_identity_and_abi_are_required(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); a, files = self.fixture(root)
            lib = root / 'lib.so'; lib.write_bytes(contract.START + contract.host_contract(a.profile) + contract.END)
            for rows in ('# TP_SLOT\t2\n', '# TP_SLOT\t3\n# BUILD_ID\t' + 'a' * 32 + '\n'):
                Path(files[1][1]).write_text(rows)
                with self.assertRaisesRegex(ValueError, 'module'): contract.target_manifest(a, lib, files)


if __name__ == '__main__': unittest.main()
