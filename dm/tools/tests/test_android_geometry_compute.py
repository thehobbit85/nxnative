"""Android geometry helpers preserve integer topology indices across buffer views."""
import argparse
import importlib
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

SOURCE = '''#version 450
layout(local_size_x=1) in;
layout(set=0,binding=0) uniform samplerBuffer trb_data;
layout(set=0,binding=1) uniform samplerBuffer guest_data;
layout(set=0,binding=2,std430) buffer Result { uint values[]; } result;
void main() {
    int i=int(gl_GlobalInvocationID.x);
    result.values[i*3]=floatBitsToUint(texelFetch(trb_data,i).x);
    result.values[i*3+1]=floatBitsToUint(texelFetch(guest_data,i).x);
    result.values[i*3+2]=uint(textureSize(trb_data));
}
'''


@unittest.skipUnless(all(shutil.which(t) for t in ('glslc', 'spirv-val')), 'requires shader tools')
class GeometryCompute(unittest.TestCase):
    def compile(self, source):
        return subprocess.run(['glslc', '--target-env=vulkan1.1', '-fshader-stage=compute', '-', '-o', '-'],
                              input=source.encode(), stdout=subprocess.PIPE, check=True).stdout

    def valid(self, data):
        result = subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', '-'], input=data, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr.decode())

    def test_topology_view_is_unsigned_without_retyping_the_guest_texture(self):
        lower = importlib.import_module('dm.android.tools.spv_geometry')
        export = importlib.import_module('dm.android.tools.spv_android')
        raw = self.compile(SOURCE)
        data = lower.topology_buffer(raw)
        self.valid(data)
        resources = [dict(msl_name='trb_data', kind='geometry', reg=4),
                     dict(msl_name='guest_data', kind='tbuf', reg=0),
                     dict(msl_name='result', kind='sbuf', reg=0)]
        bindings = {b['binding']: b for b in export.vk_reflect(data, dict(bindings=resources))}
        self.assertEqual((bindings[0]['type'], bindings[0]['num']), ('tbuf', 2))
        self.assertEqual((bindings[1]['type'], bindings[1]['num']), ('tbuf', 0))
        self.assertEqual(lower.topology_buffer(data), data)

    def test_unrelated_shader_is_unchanged_and_wrong_helper_shape_is_rejected(self):
        lower = importlib.import_module('dm.android.tools.spv_geometry')
        raw = self.compile('#version 450\nlayout(local_size_x=1) in; void main(){}')
        self.assertEqual(lower.topology_buffer(raw), raw)
        source = '''#version 450
layout(local_size_x=1) in;
layout(binding=0) uniform sampler2D trb_data;
layout(binding=1,std430) buffer Result { vec4 v; } result;
void main(){result.v=texelFetch(trb_data,ivec2(0),0);}
'''
        with self.assertRaisesRegex(ValueError, 'topology.*buffer'):
            lower.topology_buffer(self.compile(source))

    @unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'requires shared translator')
    def test_all_shared_compute_topologies_remain_valid(self):
        from test_android_geometry_export import passthrough_shader
        lower = importlib.import_module('dm.android.tools.spv_geometry')
        shared = importlib.import_module('dm.gfx.tools.dmsh_geometry')
        with tempfile.TemporaryDirectory(prefix='android_geometry_compute_') as tmp:
            work = Path(tmp); code = work / 'geometry.bin'; code.write_bytes(passthrough_shader())
            jobs = [dict(key=f'g{t}', stage='gs', code=str(code), geometry_topology=t,
                         host_target='vulkan', tex={}, img={}, attr={}, fout={}) for t in range(5)]
            (work / 'jobs.json').write_text(json.dumps(jobs))
            subprocess.run([os.environ['DMSH_TEST_XLATE'], work / 'jobs.json', work], check=True, capture_output=True)
            for t in range(5):
                with self.subTest(topology=t):
                    path = work / f'g{t}.spv'; shared.flatten_sets(path)
                    self.valid(lower.topology_buffer(path.read_bytes()))


if __name__ == '__main__':
    if '--fixture' in sys.argv:
        parser = argparse.ArgumentParser()
        parser.add_argument('--fixture', type=Path, required=True)
        args = parser.parse_args()
        lower = importlib.import_module('dm.android.tools.spv_geometry')
        raw = subprocess.run([os.environ.get('GLSLC', 'glslc'), '--target-env=vulkan1.1',
                              '-fshader-stage=compute', '-', '-o', '-'],
                             input=SOURCE.encode(), stdout=subprocess.PIPE, check=True).stdout
        args.fixture.parent.mkdir(parents=True, exist_ok=True)
        args.fixture.write_bytes(lower.topology_buffer(raw))
        args.fixture.with_suffix('.original.spv').write_bytes(raw)
    else: unittest.main()
