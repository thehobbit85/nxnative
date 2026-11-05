"""Feedback layout must survive offline capture and have a bounded native reader."""
import ctypes
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

from test_android_geometry_export import passthrough_shader

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'dm/android/tools'))
import spv_android as vk

LAYOUT = [{'stride': 20, 'outputs': [[0, 2], [1, 3]]},
          {'stride': 4, 'outputs': [[1, 1]]}, {'stride': 4, 'outputs': [[1, 1]]}, []]


class FeedbackMetadata(unittest.TestCase):
    @unittest.skipUnless(all(shutil.which(t) for t in ('glslc', 'spirv-val', 'spirv-opt')), 'requires shader tools')
    def test_prepared_export_keeps_complete_control_layout(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp); prepared = work / 'prepared'; prepared.mkdir()
            key = '0123456789abcdef'; binary = prepared / (key + '.spv')
            subprocess.run(['glslc', '-fshader-stage=vertex', '--target-env=vulkan1.1', '-', '-o', binary],
                           input='#version 450\nlayout(location=0) out vec4 a; layout(location=1) out vec4 b; '
                                 'void main(){ a=vec4(1); b=vec4(2); gl_Position=vec4(0,0,0,1); }', text=True, check=True)
            (prepared / (key + '.reflection.json')).write_text(json.dumps(dict(stage='vs', bindings=[], tf=LAYOUT)))
            with mock.patch.dict(os.environ, {'DM_GAME': '0100633007D48000'}):
                vk.capture_manifest(prepared, [key], vk.shader_contract())
                vk.export_prepared(prepared, work / 'export')
            text = (work / 'export' / (key + '.vk.txt')).read_text()
            self.assertIn('tf 0 20 0 2 1 3\n', text)
            self.assertIn('tf 2 4 1 1\n', text)

    def test_interleaved_and_duplicate_capture_outputs_survive(self):
        text = vk.vk_txt(dict(stage='vs', bindings=[], tf=LAYOUT))
        self.assertIn('tf 0 20 0 2 1 3\n', text)
        self.assertIn('tf 1 4 1 1\n', text)
        self.assertIn('tf 2 4 1 1\n', text)

    def test_invalid_feedback_layout_is_not_published(self):
        for layout in (LAYOUT[:3], [dict(stride=16, outputs=[[0, 3]]), [], [], []],
                       [dict(stride=4, outputs=[[32, 1]]), [], [], []],
                       [dict(stride=4, outputs=[[0, 0]]), [], [], []],
                       [dict(stride=4, outputs=[[0, True]]), [], [], []]):
            with self.subTest(layout=layout), self.assertRaisesRegex(ValueError, 'feedback'):
                vk.vk_txt(dict(stage='vs', bindings=[], tf=layout))
        with self.assertRaisesRegex(ValueError, 'feedback'):
            vk.vk_txt(dict(stage='cs', bindings=[], tf=LAYOUT))

    @unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'requires the offline translator')
    def test_capture_preserves_shared_jobs_control_metadata(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp); captured = work / 'capture'; captured.mkdir()
            code = work / 'gs.bin'; code.write_bytes(passthrough_shader())
            jobs = work / 'jobs.json'
            jobs.write_text(json.dumps([dict(key='0123456789abcdef', stage='gs', code=str(code),
                                            tex={}, img={}, attr={}, fout={}, tf=LAYOUT)]))
            original = jobs.read_bytes()
            with mock.patch.object(vk, 'XLATE', Path(os.environ['DMSH_TEST_XLATE'])), \
                 mock.patch.dict(os.environ, {'DMSH_SPV_CAPTURE': str(captured)}):
                self.assertEqual(vk.capture([str(jobs), str(work)]), 0)
            self.assertEqual(jobs.read_bytes(), original)
            metadata = json.loads((captured / '0123456789abcdef.control.json').read_text())
            self.assertEqual(metadata['tf'], LAYOUT)
            self.assertNotIn('code', metadata)

    @unittest.skipUnless(shutil.which('cc'), 'requires a C compiler')
    def test_native_reader_retains_buffer_order_and_rejects_malformed_rows(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp); source = work / 'reader.c'; library = work / 'reader.so'
            source.write_text('#include "dm/gfx/nvn/vulkan/shader_contract.h"\n'
                              'int check(const char *s, size_t n, unsigned *out) { dm_vk_feedback f = {0}; '
                              'const char *e = dm_vk_feedback_line(s, n, &f); '
                              'if(e) return -1; out[0]=f.stride[0]; out[1]=f.count[0]; '
                              'out[2]=f.location[0][1]; out[3]=f.components[0][1]; '
                              'return dm_vk_feedback_line(s, n, &f) ? 0 : -2; }')
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                            '-shared', '-fPIC', '-I', str(ROOT), source, '-o', library], check=True, capture_output=True)
            check = ctypes.CDLL(str(library)).check
            check.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_uint)]; check.restype = ctypes.c_int
            out = (ctypes.c_uint * 4)()
            row = b'tf 0 20 0 2 1 3'
            self.assertEqual(check(row, len(row), out), 0)
            self.assertEqual(list(out), [20, 2, 1, 3])
            for bad in (b'tf 0 4 0 0', b'tf 0 4 32 1', b'tf 4 4 0 1', b'tf 0 8 0 1',
                        b'tf 0 4 0', b'tf 0 4 0 -1', b'tf 0 4 0 1junk', b'tf 0 4294967296 0 1',
                        b'tf 0 4 0 1 ', b'tf 0 4 0 1\0', b'tf 0 2064' + b' 0 4' * 129):
                with self.subTest(row=bad): self.assertEqual(check(bad, len(bad), out), -1)


if __name__ == '__main__': unittest.main()
