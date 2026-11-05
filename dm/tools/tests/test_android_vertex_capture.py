"""Portable vertex capture uses the same 144 raw words as Metal geometry."""
import argparse
import ast
import importlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from typing import Any
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

SOURCE = '''#version 450
out gl_PerVertex { vec4 gl_Position; float gl_PointSize; float gl_ClipDistance[8]; };
layout(location=0) out vec4 a;
layout(location=7) out vec4 partial;
layout(location=30) out uvec4 partial_bits;
layout(location=31) out uvec4 b;
void main() {
    float n = float(gl_VertexIndex);
    a = vec4(n, n+10, n+20, n+30);
    partial.x = 42;
    partial_bits.x = 42;
    b = uvec4(1, 0xdeadbeef, 0x7fc01234, gl_InstanceIndex);
    gl_Position = vec4(0,0,0,1); gl_PointSize = 64;
    gl_ClipDistance[0] = -0.5; gl_ClipDistance[7] = 0.5;
}
'''


def metal_fixture(raw):
    """Run the existing Apple output initialization and capture on this input."""
    geometry = importlib.import_module('dm.gfx.tools.dmsh_geometry')
    with tempfile.TemporaryDirectory(prefix='metal_vertex_capture_') as tmp:
        source = Path(tmp) / 'source.spv'; source.write_bytes(raw)
        msl = subprocess.check_output([str(ROOT / 'dm/gfx/tools/spirv_cross_bin/spirv-cross'), str(source),
                                       '--msl', '--msl-version', '24000', '--rename-entry-point', 'main', 'main0', 'vert'], text=True)
    # Load the production pure helper without the builder's CLI/profile setup,
    # just as the existing Metal geometry tests do for the depth helper.
    builder = (ROOT / 'dm/gfx/tools/dmsh_build.py').read_text()
    node = next(n for n in ast.parse(builder).body if isinstance(n, ast.FunctionDef) and n.name == 'vs_init')
    namespace: dict[str, Any] = {'re': re}
    helper = ast.get_source_segment(builder, node)
    assert helper is not None
    exec(helper, namespace)
    msl = namespace['vs_init'](msl)
    return msl + geometry.capture(msl)


@unittest.skipUnless(all(shutil.which(t) for t in ('glslc', 'spirv-val')), 'requires shader tools')
class VertexCapture(unittest.TestCase):
    @unittest.skipUnless(shutil.which('spirv-opt'), 'requires SPIRV-Tools')
    def test_export_derives_capture_without_modifying_the_ordinary_vertex_stage(self):
        exporter = importlib.import_module('dm.android.tools.spv_android')
        raw = subprocess.run(['glslc', '--target-env=vulkan1.1', '-fshader-stage=vertex', '-', '-o', '-'],
                             input=SOURCE.encode(), stdout=subprocess.PIPE, check=True).stdout
        key = '0123456789abcdef'
        with tempfile.TemporaryDirectory(prefix='vertex_capture_export_') as tmp:
            work = Path(tmp)
            exporter.write_stage(key, raw, dict(stage='vs', bindings=[]), work, shutil.which('spirv-val'), shutil.which('spirv-opt'))
            self.assertEqual((work / (key + '.spv')).read_bytes(), exporter.export_spirv(raw, shutil.which('spirv-opt')))
            self.assertTrue((work / (key + '.vc.spv')).is_file())
            text = (work / (key + '.vc.vk.txt')).read_text()
            self.assertIn('capture 144 6 0 1\n', text)
            self.assertIn('b 0 ssbo 1 geometry 0 -1 - -1\n', text)
            self.assertIn('b 1 ubo 1 geometry 5 -1 - -1\n', text)
            self.assertNotIn('capture ', (work / (key + '.vk.txt')).read_text())

    def test_unused_builtin_declarations_keep_defaults_but_live_unknown_builtins_fail(self):
        capture = importlib.import_module('dm.android.tools.spv_capture')
        source = '''#version 450
out gl_PerVertex { vec4 gl_Position; float gl_PointSize; float gl_CullDistance[1]; };
void main(){ gl_Position=vec4(0,0,0,1); }
'''
        def compile_shader(text):
            return subprocess.run(['glslc', '--target-env=vulkan1.1', '-fshader-stage=vertex', '-', '-o', '-'],
                                  input=text.encode(), stdout=subprocess.PIPE, check=True).stdout
        raw = compile_shader(source)
        self.assertEqual(set(capture.Module(raw).output_words()), {0, 1, 2, 3})
        data, _ = capture.geometry_capture(raw)
        result = subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', '-'], input=data, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        with self.assertRaisesRegex(ValueError, 'unsupported geometry capture builtin'):
            capture.geometry_capture(compile_shader(source.replace('gl_Position=', 'gl_CullDistance[0]=1; gl_Position=')))

    def test_unsupported_output_width_is_rejected(self):
        capture = importlib.import_module('dm.android.tools.spv_capture')
        source = '#version 450\nlayout(location=0) out dvec4 x; void main(){ x=dvec4(1); gl_Position=vec4(0,0,0,1); }'
        raw = subprocess.run(['glslc', '--target-env=vulkan1.1', '-fshader-stage=vertex', '-', '-o', '-'],
                             input=source.encode(), stdout=subprocess.PIPE, check=True).stdout
        with self.assertRaisesRegex(ValueError, '32-bit'):
            capture.geometry_capture(raw)

    @unittest.skipUnless(all(shutil.which(t) for t in ('spirv-as', 'spirv-dis')), 'requires SPIRV-Tools')
    def test_extended_instruction_number_is_not_an_output_pointer(self):
        capture = importlib.import_module('dm.android.tools.spv_capture')
        source = '''#version 450
out gl_PerVertex { vec4 gl_Position; float gl_CullDistance[1]; };
void main(){ gl_Position=fma(vec4(float(gl_VertexIndex)),vec4(2),vec4(1)); }
'''
        raw = subprocess.run(['glslc', '--target-env=vulkan1.1', '-fshader-stage=vertex', '-', '-o', '-'],
                             input=source.encode(), stdout=subprocess.PIPE, check=True).stdout
        code = capture.decode(raw)[1]
        variable, = [i[2] for i in code if capture.opcode(i) == 59 and i[3] == 3]
        self.assertTrue(any(capture.opcode(i) == 12 and i[4] == 50 for i in code))  # GLSL Fma
        assembly = subprocess.check_output(['spirv-dis', '--raw-id', '-'], input=raw).decode()
        # Force the legal ID collision seen in the commercial shader corpus.
        assembly = re.sub(r'%(\d+)', lambda m: '%50' if int(m[1]) == variable else '%id' + m[1], assembly)
        raw = subprocess.run(['spirv-as', '--preserve-numeric-ids', '--target-env', 'spv1.3', '-', '-o', '-'],
                             input=assembly.encode(), stdout=subprocess.PIPE, check=True).stdout
        self.assertEqual(set(capture.Module(raw).output_words()), {0, 1, 2, 3})
        data, _ = capture.geometry_capture(raw)
        result = subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', '-'], input=data, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr.decode())

    @unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE') and shutil.which('spirv-opt'), 'requires offline translator')
    def test_shared_maxwell_vertex_translation_can_be_captured(self):
        from test_txq_type import base, mov, shader
        capture = importlib.import_module('dm.android.tools.spv_capture')
        exporter = importlib.import_module('dm.android.tools.spv_android')
        header = bytearray(0x50); struct.pack_into('<I', header, 0, (1 << 10) | (3 << 5) | 1)
        code = header + shader(mov(0, 0x3f800000), base('Ast') | (7 << 16) | (255 << 8) | (0x80 << 20))
        with tempfile.TemporaryDirectory(prefix='maxwell_vertex_capture_') as tmp:
            work = Path(tmp); (work / 'vertex.bin').write_bytes(code)
            job = dict(key='vertex', stage='vs', code=str(work / 'vertex.bin'), tex={}, img={}, attr={}, fout={}, host_target='vulkan')
            (work / 'jobs.json').write_text(json.dumps([job]))
            subprocess.run([os.environ['DMSH_TEST_XLATE'], str(work / 'jobs.json'), str(work)], check=True, capture_output=True)
            raw = exporter.export_spirv((work / 'vertex.spv').read_bytes(), shutil.which('spirv-opt'))
            self.assertIn(16, capture.Module(raw).output_words())
            data, _ = capture.geometry_capture(raw)
            result = subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', '-'], input=data, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr.decode())

    def test_existing_descriptors_and_early_returns_are_preserved(self):
        capture = importlib.import_module('dm.android.tools.spv_capture')
        source = SOURCE.replace('void main() {', 'layout(set=0,binding=7) uniform Params { uint stop; } p;\nvoid main() { if(p.stop != 0) return;')
        raw = subprocess.run(['glslc', '--target-env=vulkan1.1', '-fshader-stage=vertex', '-', '-o', '-'],
                             input=source.encode(), stdout=subprocess.PIPE, check=True).stdout
        data, metadata = capture.geometry_capture(raw)
        self.assertEqual((metadata['output_binding'], metadata['control_binding']), (8, 9))
        result = subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', '-'], input=data, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertEqual(sum(capture.opcode(i) == 253 for i in capture.decode(data)[1]),
                         1 + sum(capture.opcode(i) == 253 for i in capture.decode(raw)[1]))

    def test_output_arrays_and_blocks_keep_location_spacing(self):
        capture = importlib.import_module('dm.android.tools.spv_capture')
        source = '''#version 450
out gl_PerVertex { vec4 gl_Position; };
layout(location=3) out vec2 values[2];
layout(location=8) out Outputs { vec4 a; vec2 b; } block;
void main(){ gl_Position=vec4(0,0,0,1); values[0]=vec2(1); values[1]=vec2(2);
             block.a=vec4(3); block.b=vec2(4); }
'''
        raw = subprocess.run(['glslc', '--target-env=vulkan1.1', '-fshader-stage=vertex', '-', '-o', '-'],
                             input=source.encode(), stdout=subprocess.PIPE, check=True).stdout
        words = capture.Module(raw).output_words()
        self.assertEqual(set(words), {0, 1, 2, 3, 28, 29, 32, 33, 48, 49, 50, 51, 52, 53})
        data, _ = capture.geometry_capture(raw)
        result = subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', '-'], input=data, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr.decode())

    def test_generated_capture_is_valid_and_describes_its_host_interface(self):
        capture = importlib.import_module('dm.android.tools.spv_capture')
        with tempfile.TemporaryDirectory(prefix='vk_vertex_capture_') as tmp:
            work = Path(tmp); source = work / 'vertex.spv'
            subprocess.run(['glslc', '--target-env=vulkan1.1', '-fshader-stage=vertex', '-', '-o', source],
                           input=SOURCE, text=True, check=True)
            original = source.read_bytes()
            data, metadata = capture.geometry_capture(original)
            self.assertEqual(source.read_bytes(), original)
            self.assertEqual(metadata['words'], 144)
            self.assertEqual(metadata['control_words'], 6)
            self.assertEqual(metadata['output_binding'], 0)
            self.assertEqual(metadata['control_binding'], 1)
            output = work / 'capture.spv'; output.write_bytes(data)
            result = subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', output], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    if '--fixture' in sys.argv:
        parser = argparse.ArgumentParser()
        parser.add_argument('--fixture', type=Path, required=True)
        parser.add_argument('--metal-fixture', type=Path)
        args = parser.parse_args()
        capture = importlib.import_module('dm.android.tools.spv_capture')
        output = args.fixture; output.parent.mkdir(parents=True, exist_ok=True)
        raw = subprocess.run([os.environ.get('GLSLC', 'glslc'), '--target-env=vulkan1.1', '-fshader-stage=vertex', '-', '-o', '-'],
                             input=SOURCE.encode(), stdout=subprocess.PIPE, check=True).stdout
        data, metadata = capture.geometry_capture(raw)
        output.write_bytes(data); output.with_suffix('.json').write_text(json.dumps(metadata))
        if args.metal_fixture: args.metal_fixture.write_text(metal_fixture(raw))
    else: unittest.main()
