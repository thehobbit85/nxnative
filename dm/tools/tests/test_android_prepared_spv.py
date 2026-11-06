"""A verified capture can be exported without Apple tools; damaged captures never publish."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
from dm.android.tools.target_contract import shader_contract


class CaptureLibrary(unittest.TestCase):
    def test_reflection_survives_packed_compiler_output(self):
        from dm.android.tools import spv_android
        from dm.tools import shader_lib

        with tempfile.TemporaryDirectory() as tmp:
            cache = Path(tmp)
            library = cache / 'b-0123456789ab'
            library.mkdir()
            name = '0123456789abcdef_b.json'
            (library / name).write_text('{"stage":"cs"}')
            self.assertEqual((spv_android.capture_library(cache) / name).read_text(), '{"stage":"cs"}')
            self.assertTrue(shader_lib.pack(library, say=lambda *_: None))
            self.assertEqual((spv_android.capture_library(cache) / name).read_text(), '{"stage":"cs"}')
            library.mkdir()  # A subsequent partial compiler batch has started staging files.
            self.assertEqual((spv_android.capture_library(cache) / name).read_text(), '{"stage":"cs"}')


@unittest.skipUnless(all(shutil.which(t) for t in ('glslc', 'spirv-val', 'spirv-opt')), 'requires shader tools')
class PreparedExport(unittest.TestCase):
    def fixture(self, root):
        folder = root / 'capture'; folder.mkdir()
        key = '0123456789abcdef'
        binary = folder / (key + '.spv')
        subprocess.run(['glslc', '-fshader-stage=compute', '--target-env=vulkan1.1', '-', '-o', binary],
                       input='#version 450\nlayout(local_size_x=1) in; void main() {}', text=True, check=True)
        (folder / (key + '.reflection.json')).write_text(json.dumps(dict(stage='cs', bindings=[], threadgroup_size=[1, 1, 1])))
        manifest = dict(schema=1, target='vulkan1.1', producer=hashlib.sha256(shader_contract('0100633007D48000')).hexdigest(),
                        keys=[key], files={p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in folder.iterdir()})
        (folder / 'capture.json').write_text(json.dumps(manifest))
        tools = root / 'tools'; tools.mkdir()
        for name in ('spirv-val', 'spirv-opt'): (tools / name).symlink_to(shutil.which(name))
        env = dict(os.environ, PATH=str(tools), DM_GAME='0100633007D48000')
        for name in ('DM_GAMES_DIR', 'DM_GAMES_FIRST'): env.pop(name, None)
        return folder, key, env

    def export(self, folder, out, env):
        return subprocess.run([sys.executable, ROOT / 'dm/android/tools/spv_android.py', folder, out, '--from-prepared'],
                              env=env, text=True, capture_output=True)

    def test_portable_export_without_apple_preparation_tools(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); folder, key, env = self.fixture(root); out = root / 'export'
            result = self.export(folder, out, env)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertTrue((out / 'export-contract.json').is_file())
            sidecar = (out / (key + '.vk.txt')).read_text().splitlines()
            self.assertTrue({'stage cs', 'cap 1', 'warp independent'} <= set(sidecar))
            subprocess.run([str(Path(env['PATH']) / 'spirv-val'), '--target-env', 'vulkan1.1', out / (key + '.spv')], check=True)

    def test_corruption_or_stale_producer_never_publishes(self):
        for damage in ('binary', 'metadata', 'producer', 'missing', 'path', 'symlink', 'extra', 'json', 'shape', 'stage'):
            with self.subTest(damage=damage), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp); folder, key, env = self.fixture(root); manifest = folder / 'capture.json'
                data = json.loads(manifest.read_text())
                if damage == 'binary': (folder / (key + '.spv')).write_bytes(b'broken')
                elif damage == 'metadata': (folder / (key + '.reflection.json')).write_text('{}')
                elif damage == 'producer': data['producer'] = '0' * 64
                elif damage == 'missing': (folder / (key + '.spv')).unlink()
                elif damage == 'path': data['keys'] = ['../escape']
                elif damage == 'symlink':
                    binary = folder / (key + '.spv'); binary.rename(root / 'outside'); binary.symlink_to(root / 'outside')
                elif damage == 'extra': data['files']['unclaimed.spv'] = '0' * 64
                elif damage in ('json', 'shape', 'stage'):
                    metadata = folder / (key + '.reflection.json')
                    metadata.write_text('{broken' if damage == 'json' else '[]' if damage == 'shape' else json.dumps(dict(stage='vs', bindings=[])))
                    data['files'][metadata.name] = hashlib.sha256(metadata.read_bytes()).hexdigest()
                manifest.write_text(json.dumps(data))
                out = root / 'export'; result = self.export(folder, out, env)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('capture', result.stderr.lower())
                self.assertFalse(out.exists())


if __name__ == '__main__': unittest.main()
