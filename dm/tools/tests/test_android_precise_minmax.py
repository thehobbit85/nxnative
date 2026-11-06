"""Android exports must select the numeric operand of a floating-point min/max with one NaN."""
from pathlib import Path
import shutil
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
from dm.android.tools.spv_android import export_spirv
from dm.android.tools.spv_module import decode, opcode, string_operand


@unittest.skipUnless(all(shutil.which(t) for t in ('glslc', 'spirv-opt', 'spirv-val')), 'requires SPIR-V tools')
class PreciseMinMax(unittest.TestCase):
    def test_scalar_and_vector_float_minmax_and_clamp(self):
        source = b"""#version 450
layout(local_size_x=1) in;
layout(set=0, binding=0, std430) buffer Data { vec4 a, b, c, lo, hi, sat; float smallest, saturated; uvec4 i, j; } d;
void main() {
    d.lo = min(d.a, d.b);
    d.hi = max(d.a, d.b);
    d.sat = clamp(d.a, d.b, d.c);
    d.smallest = min(d.a.x, d.b.z);
    d.saturated = clamp(d.a.y, 0., 1.);
    d.i = min(d.i, d.j);
}
"""
        raw = subprocess.check_output(['glslc', '-fshader-stage=compute', '--target-env=vulkan1.1', '-', '-o', '-'], input=source)
        def glsl_ops(data):
            _, code = decode(data)
            ids = {i[1] for i in code if opcode(i) == 11 and string_operand(i[2:]) == 'GLSL.std.450'}
            return [i[4] for i in code if opcode(i) == 12 and i[3] in ids]
        self.assertCountEqual(glsl_ops(raw), [37, 40, 43, 37, 43, 38])
        result = export_spirv(raw, 'spirv-opt')
        subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', '-'], input=result, check=True)
        self.assertCountEqual(glsl_ops(result), [79, 80, 80, 79, 79, 81, 38])
        self.assertEqual(export_spirv(result, 'spirv-opt'), result)
