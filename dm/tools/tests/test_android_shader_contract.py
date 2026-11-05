"""The exported shader contract must reject stale consumers and retain feature requirements."""
from pathlib import Path
import struct
import ctypes
import shutil
import subprocess
import tempfile
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'android/tools'))
import spv_android as vk


class ShaderContract(unittest.TestCase):
    def test_geometry_helper_metadata_distinguishes_execution_and_logical_stages(self):
        geometry = dict(role='compute', topology=3, stride=134, vertices=3, invocations=1, output=7, index_stride=6)
        self.assertIn('geometry compute 3 134 3 1 7 6\n', vk.vk_txt(dict(stage='cs', bindings=[], geometry=geometry)))
        self.assertIn('geometry reader 3 134 3 1 7 6\n', vk.vk_txt(dict(stage='vs', bindings=[], geometry=dict(geometry, role='reader'))))
        captured = dict(stage='vs', bindings=[], geometry=dict(geometry, role='capture'),
                        capture=dict(words=144, control_words=6, output_binding=1, control_binding=2))
        self.assertIn('geometry capture 3 134 3 1 7 6\n', vk.vk_txt(captured))
        with self.assertRaisesRegex(ValueError, 'geometry'):
            vk.vk_txt({k: v for k, v in captured.items() if k != 'capture'})
        for field, value in [('role', 'unknown'), ('topology', 5), ('stride', 145), ('stride', 4),
                             ('vertices', 0), ('vertices', 4096), ('invocations', 256), ('output', 5),
                             ('index_stride', 5), ('topology', True)]:
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, 'geometry'):
                vk.vk_txt(dict(stage='cs', bindings=[], geometry=dict(geometry, **{field: value})))
        with self.assertRaisesRegex(ValueError, 'geometry'):
            vk.vk_txt(dict(stage='vs', bindings=[], geometry=geometry))
        for malformed in (None, [], 'compute'):
            with self.subTest(geometry=malformed), self.assertRaisesRegex(ValueError, 'geometry'):
                vk.vk_txt(dict(stage='cs', bindings=[], geometry=malformed))
        with self.assertRaisesRegex(ValueError, 'geometry'):
            vk.vk_txt(dict(stage='vs', bindings=[], geometry=dict(geometry, role='reader'),
                           capture=dict(words=144, control_words=6, output_binding=1, control_binding=2)))

    def test_capture_interface_is_bounded_and_vertex_only(self):
        valid = dict(words=144, control_words=6, output_binding=7, control_binding=8)
        self.assertIn('capture 144 6 7 8\n', vk.vk_txt(dict(stage='vs', bindings=[], capture=valid)))
        for key, value in [('words', 143), ('control_words', 5), ('output_binding', 8),
                           ('output_binding', -1), ('control_binding', 0x80000000), ('words', True)]:
            with self.subTest(field=key, value=value), self.assertRaisesRegex(ValueError, 'capture'):
                vk.vk_txt(dict(stage='vs', bindings=[], capture=dict(valid, **{key: value})))
        with self.assertRaisesRegex(ValueError, 'capture'):
            vk.vk_txt(dict(stage='cs', bindings=[], capture=valid))

    def test_sidecar_declares_version_and_target(self):
        text = vk.vk_txt(dict(stage='cs', bindings=[]))
        self.assertTrue(text.startswith('schema 2\ntarget vulkan1.1\n'), text)

    def test_optional_requirements_survive_export(self):
        text = vk.vk_txt(dict(stage='ps', bindings=[], capabilities=[1, 10, 5378],
                              extensions=['SPV_EXT_fragment_shader_interlock']))
        self.assertIn('cap 10\n', text)
        self.assertIn('cap 5378\n', text)
        self.assertIn('ext SPV_EXT_fragment_shader_interlock\n', text)

    def test_unknown_metadata_versions_are_rejected(self):
        with self.assertRaisesRegex(ValueError, 'schema'):
            vk.vk_txt(dict(stage='cs', bindings=[], schema=999))

    def test_metadata_cannot_inject_shader_records(self):
        with self.assertRaisesRegex(ValueError, 'extension'):
            vk.vk_txt(dict(stage='ps', bindings=[], extensions=['SPV_EXT_bad\nstage cs']))

    def test_binary_target_version_is_bounded(self):
        for version in (0x100, 0x10600, 0x10301):
            with self.subTest(version=version), self.assertRaisesRegex(ValueError, 'version'):
                vk.decode(struct.pack('<5I', 0x07230203, version, 0, 1, 0))


@unittest.skipUnless(shutil.which('cc') and shutil.which('glslc'), 'requires C compiler and glslc')
class NativeShaderContract(unittest.TestCase):
    def test_runtime_rejects_incompatible_or_incomplete_requirements(self):
        root = Path(__file__).resolve().parents[3]
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            (work / 'check.c').write_text('#include "dm/gfx/nvn/vulkan/shader_contract.h"\n'
                                        'const char *check(const char *t, size_t nt, const uint32_t *w, size_t n) {'
                                        'return dm_vk_shader_contract(t, nt, w, n); }')
            lib = work / 'check.so'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                            '-I', str(root), work / 'check.c', '-o', lib], check=True, capture_output=True)
            check = ctypes.CDLL(str(lib)).check
            check.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t]
            check.restype = ctypes.c_char_p
            spv = work / 'test.spv'
            subprocess.run(['glslc', '-fshader-stage=compute', '--target-env=vulkan1.1', '-', '-o', spv],
                           input='#version 450\nlayout(local_size_x=1) in; void main() {}', text=True, check=True)
            data = spv.read_bytes()
            # Hand-checked requirements: one compute entry point and Shader capability.
            good = b'schema 2\ntarget vulkan1.1\nstage cs\ncap 1\ntg 1 1 1\n'
            self.assertIsNone(check(good, len(good), data, len(data) // 4))
            geometry = good + b'geometry compute 3 134 3 1 7 6\n'
            self.assertIsNone(check(geometry, len(geometry), data, len(data) // 4))
            feedback_geometry = geometry + b'tf 0 4 0 1\n'
            self.assertIsNone(check(feedback_geometry, len(feedback_geometry), data, len(data) // 4))
            for bad in (geometry + b'geometry compute 3 134 3 1 7 6\n', geometry.replace(b'compute 3', b'reader 3'),
                        geometry.replace(b'3 134', b'5 134'), geometry.replace(b'134', b'145'),
                        geometry.replace(b'3 1 7 6', b'3 1 7 5'), geometry.replace(b'3 1 7 6', b'3 0 7 6'),
                        geometry.replace(b'3 1 7 6', b'4096 1 7 8192'), geometry.replace(b'3 1 7 6', b'3 256 7 6')):
                self.assertIsNotNone(check(bad, len(bad), data, len(data) // 4))
            exported = vk.vk_txt(dict(stage='cs', bindings=[], **vk.requirements(data))).encode()
            self.assertIsNone(check(exported, len(exported), data, len(data) // 4))
            for bad in (good.replace(b'2', b'9', 1), good.replace(b'vulkan1.1', b'vulkan9.9'),
                        good.replace(b'cs', b'vs'), good.replace(b'cap 1\n', b''),
                        good + b'cap 10\n', good + b'cap 1\n', good + b'schema 2\n',
                        good + b'ext SPV_EXT_bad\n', good + b'cap -1\n',
                        good + b'required_unknown 1\n', good.replace(b'cap 1', b'cap 1junk'),
                        good + b'\x00stage ps\n', good + b'tf 0 4 0 1\n'):
                with self.subTest(sidecar=bad):
                    self.assertIsNotNone(check(bad, len(bad), data, len(data) // 4))
            for broken in (data[:16], data[:-4], data[:20] + b'\0\0\0\0' + data[24:]):
                self.assertIsNotNone(check(good, len(good), broken, len(broken) // 4))
            subprocess.run(['glslc', '-fshader-stage=vertex', '--target-env=vulkan1.1', '-', '-o', spv],
                           input='#version 450\nvoid main(){gl_Position=vec4(0,0,0,1);}', text=True, check=True)
            data = spv.read_bytes()
            good = b'schema 2\ntarget vulkan1.1\nstage vs\ncap 1\ntf 0 20 0 2 1 3\ntf 2 4 1 1\n'
            self.assertIsNone(check(good, len(good), data, len(data) // 4))
            for bad in (good + b'tf 0 4 0 1\n', good.replace(b'tf 0 20', b'tf 0 16')):
                self.assertIsNotNone(check(bad, len(bad), data, len(data) // 4))
            reader = good + b'geometry reader 3 134 3 1 7 6\n'
            self.assertIsNone(check(reader, len(reader), data, len(data) // 4))
            mixed = reader + b'capture 144 6 0 1\n'
            self.assertIsNotNone(check(mixed, len(mixed), data, len(data) // 4))
            geometry_capture = mixed.replace(b'geometry reader ', b'geometry capture ')
            self.assertIsNone(check(geometry_capture, len(geometry_capture), data, len(data) // 4))
            missing = geometry_capture.replace(b'capture 144 6 0 1\n', b'')
            self.assertIsNotNone(check(missing, len(missing), data, len(data) // 4))
            capture = good + b'capture 144 6 0 1\n'
            self.assertIsNone(check(capture, len(capture), data, len(data) // 4))
            for bad in (capture + b'capture 144 6 0 1\n', capture.replace(b'144 6', b'143 6'),
                        capture.replace(b'144 6', b'144 5'), capture.replace(b'6 0 1', b'6 0 0'),
                        capture.replace(b'6 0 1', b'6 -1 1'), capture.replace(b'6 0 1', b'6 2147483648 1')):
                self.assertIsNotNone(check(bad, len(bad), data, len(data) // 4))


if __name__ == '__main__': unittest.main()
