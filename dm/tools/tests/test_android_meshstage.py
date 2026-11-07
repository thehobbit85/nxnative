"""NVN2 stage memory must survive the shared translator's Android capture."""
import json
import ctypes
import os
from pathlib import Path
import struct
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import copy

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'android/tools'))
import spv_android as vk


class MeshMetadata(unittest.TestCase):
    def test_invalid_output_maps_and_allocations_fail_before_lowering(self):
        mesh = dict(kind='ms', threads=32, payload=32, task=True, max_v=33, max_p=35,
                    vertex=[[None, c] for c in range(4)] + [[2, 0]], primitive=[[0, 1]])
        self.assertEqual(vk.spv_meshstage.layout(mesh), ([0, 16, 48, 1584], 1780))
        for key, value in (('threads', 0), ('threads', True), ('threads', 1025), ('payload', -1),
                           ('payload', 65536), ('payload', 0), ('task', 1), ('max_v', 257), ('max_p', 513),
                           ('vertex', [[0, 0]]), ('vertex', [[None, 4]]), ('vertex', [[None, 0], [None, 0]]),
                           ('vertex', [[None, 1], [None, 0]]), ('primitive', [[None, 0]]),
                           ('primitive', [[32, 0]]), ('primitive', [[0, True]]), ('refused', 'own shared memory')):
            bad = copy.deepcopy(mesh); bad[key] = value
            with self.subTest(key=key, value=value), self.assertRaises(ValueError): vk.spv_meshstage.layout(bad)
        with self.assertRaisesRegex(ValueError, 'offsets'):
            vk.spv_meshstage.layout(mesh, [0, 16, 48, 1580])
        task = dict(kind='ts', threads=1, payload=9)
        self.assertEqual(vk.spv_meshstage.layout(task), ([0, 16, 32, 41], 44))
        with self.assertRaisesRegex(ValueError, 'launch count'): vk.spv_meshstage.layout(dict(task, payload=7))
        with self.assertRaisesRegex(ValueError, 'reference limit'): vk.spv_meshstage.layout(dict(task, payload=32768))


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'requires the locked translator')
class MeshCapture(unittest.TestCase):
    def test_capture_keeps_the_control_decoder_output_map_and_memory(self):
        # ISBERD workgroup -> task output +0x10 -> EXIT, using the shared
        # Ampere stage-memory instructions already tested on the Apple path.
        code = b''.join(struct.pack('<QQ', lo, hi) for hi, lo in (
            (0x000fe20000007800, 0x00000000ff007923),
            (0x0041e8000000b800, 0x00001000ff007927),
            (0x000fea0003800000, 0x000000000000794d)))
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp); captured = work / 'capture'; captured.mkdir()
            source = work / 'task.bin'; source.write_bytes(code)
            key = '00000000feed0006'; jobs = work / 'jobs.json'
            mesh = dict(kind='ts', threads=32, payload=256)
            jobs.write_text(json.dumps([dict(key=key, stage='cs', arch='sm86', code=str(source),
                tex={}, img={}, attr={}, fout={}, local=[32, 1, 1], shared=528,
                mesh_stage=mesh, stage_memory=[0, 16, 272, 528])]))
            with mock.patch.object(vk, 'XLATE', Path(os.environ['DMSH_TEST_XLATE'])), \
                    mock.patch.dict(os.environ, {'DMSH_SPV_CAPTURE': str(captured)}):
                self.assertEqual(vk.capture([str(jobs), str(work)]), 0)
            control = json.loads((captured / (key + '.control.json')).read_text())
            self.assertEqual(control, {'mesh_stage': {'kind': 'ts', 'threads': 32, 'payload': 256},
                                       'stage_memory': [0, 16, 272, 528]})
            self.assertTrue((captured / (key + '.spv')).is_file())


@unittest.skipUnless(all(shutil.which(t) for t in ('glslc', 'spirv-val', 'spirv-opt')), 'requires shader tools')
class MeshExport(unittest.TestCase):
    def test_portable_task_export_keeps_its_logical_stage_and_executable_memory(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp); prepared = work / 'prepared'; prepared.mkdir()
            key = '00000000feed0006'; raw = prepared / (key + '.spv')
            subprocess.run(['glslc', '-fshader-stage=compute', '--target-env=vulkan1.1', '-', '-o', raw],
                input='#version 450\nlayout(local_size_x=8) in; shared uint memory[20];\n'
                      'void main() { if (gl_LocalInvocationID.x != 0) return; memory[16] = memory[0]; }',
                text=True, check=True)
            reflection = dict(stage='ts', bindings=[], threadgroup_size=[8, 1, 1],
                              mesh_stage=dict(kind='ts', threads=8, payload=32), stage_memory=[0, 16, 48, 80])
            (prepared / (key + '.reflection.json')).write_text(json.dumps(reflection))
            with mock.patch.dict(os.environ, {'DM_GAME': '0100633007D48000'}):
                vk.capture_manifest(prepared, [key], vk.shader_contract())
                try: vk.export_prepared(prepared, work / 'export', [key])
                except ValueError as error: self.fail(f'valid task stage must export: {error}')
            sidecar = (work / 'export' / (key + '.vk.txt')).read_text()
            self.assertIn('stage ts\n', sidecar)
            self.assertIn('tg 8 1 1\n', sidecar)
            self.assertIn('mesh 8 32 0 0 0 20 12 20 0 1 2\n', sidecar)
            self.assertIn('b 1 ssbo 1 mesh 1 ', sidecar)
            self.assertIn('b 2 ubo 1 mesh 2 ', sidecar)
            self.assertFalse((work / 'export' / (key + '.vc.spv')).exists())
            root = Path(__file__).resolve().parents[3]
            (work / 'check.c').write_text('#include "dm/gfx/nvn/vulkan/shader_contract.h"\n'
                'const char *check(const char *t, size_t nt, const uint32_t *w, size_t n) {'
                'return dm_vk_shader_contract(t, nt, w, n); }')
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                            '-I', str(root), work / 'check.c', '-o', work / 'check.so'], check=True)
            check = ctypes.CDLL(str(work / 'check.so')).check
            check.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t]
            check.restype = ctypes.c_char_p
            data = (work / 'export' / (key + '.spv')).read_bytes()
            text = sidecar.encode()
            self.assertIsNone(check(text, len(text), data, len(data) // 4))
            for bad in (sidecar.replace('stage ts', 'stage cs'), sidecar.replace('stage ts', 'stage ms'),
                        sidecar.replace('mesh 8', 'mesh 9'), sidecar.replace('20 12 20', '21 12 20'),
                        sidecar + 'meshv -1 0\n', sidecar + 'mesh 8 32 0 0 0 20 12 20 0 1 2\n'):
                text = bad.encode()
                self.assertIsNotNone(check(text, len(text), data, len(data) // 4), bad)


if __name__ == '__main__': unittest.main()
