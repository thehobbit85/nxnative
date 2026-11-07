"""Export a real Maxwell geometry stage for the Vulkan replay fixture."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[5]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / 'dm/tools/tests'))
from test_android_geometry_export import passthrough_shader
from dm.android.tools import spv_android as vk


def main():
    output = Path(sys.argv[1]).resolve()
    output.mkdir(parents=True, exist_ok=True)
    validator, optimizer = shutil.which('spirv-val'), shutil.which('spirv-opt')
    with tempfile.TemporaryDirectory(prefix='geometry-fixture-', dir=output.parent) as temporary:
        work = Path(temporary)
        captured = work / 'captured'
        captured.mkdir()
        shader = work / 'geometry.bin'
        shader.write_bytes(passthrough_shader())
        key = '00000000feed9c02'
        jobs = work / 'jobs.json'
        jobs.write_text(json.dumps([dict(key=key, stage='gs', code=str(shader), tex={}, img={}, attr={}, fout={})]))
        previous = os.environ.get('DMSH_SPV_CAPTURE')
        os.environ['DMSH_SPV_CAPTURE'] = str(captured)
        try:
            vk.capture([str(jobs), str(work)])
        finally:
            if previous is None:
                os.environ.pop('DMSH_SPV_CAPTURE', None)
            else:
                os.environ['DMSH_SPV_CAPTURE'] = previous
        for name in vk.spv_geometry.product_family(key):
            control = json.loads((captured / (name + '.control.json')).read_text())
            reflection = vk.spv_geometry.helper_reflection(dict(stage='gs', bindings=[], threadgroup_size=[32, 1, 1]), control)
            vk.write_stage(name, (captured / (name + '.spv')).read_bytes(), reflection, output, validator, optimizer)
        vertex = b'''#version 450
layout(location=0) in vec4 position;
layout(location=0) out vec4 value;
void main() { gl_Position=position; value=vec4(0,0,0,1); }
'''
        fragment = b'''#version 450
layout(location=0) in vec4 value;
layout(location=0) out vec4 color;
void main() { color=value; }
'''
        for name, stage, source in [('00000000feed9c01', 'vs', vertex), ('00000000feed9c03', 'ps', fragment)]:
            data = subprocess.check_output([os.environ.get('GLSLC', 'glslc'), '--target-env=vulkan1.1',
                '-fshader-stage=' + ('vertex' if stage == 'vs' else 'fragment'), '-', '-o', '-'], input=source)
            reflection = dict(stage=stage, bindings=[], vertex_inputs=[])
            vk.write_stage(name, data, reflection, output, validator, optimizer)
            if stage == 'vs':
                for suffix in ('', '.vc'):
                    with (output / (name + suffix + '.vk.txt')).open('a') as sidecar:
                        sidecar.write('vin 0 0\n')


if __name__ == '__main__':
    main()
