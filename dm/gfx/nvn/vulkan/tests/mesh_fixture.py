"""Export shared stage-memory mesh shaders and their raster readers."""
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[5]
sys.path.insert(0, str(ROOT))
from dm.android.tools import spv_android as vk


def main():
    output = Path(sys.argv[1]).resolve()
    output.mkdir(parents=True, exist_ok=True)
    info = dict(kind='ms', threads=8, payload=0, task=False, max_v=3, max_p=2,
                vertex=[[None, c] for c in range(4)] + [[1, 0], [1, 1]],
                primitive=[[0, c] for c in range(4)])
    offsets, size = vk.spv_meshstage.layout(info)
    vertex, primitive = offsets[2] // 4, offsets[3] // 4
    source = f'''#version 450
layout(local_size_x=8) in;
layout(binding=0,std140) uniform Parameters {{ uint blue; }} parameters;
shared uint memory[{size // 4}];
void main() {{
    uint lane=gl_LocalInvocationID.x, group=memory[0];
    if (lane>=3) return;
    vec2 p=lane==0 ? vec2(-1,-1) : lane==1 ? vec2(3,-1) : vec2(-1,3);
    memory[{vertex}+lane]=floatBitsToUint(p.x);
    memory[{vertex+32}+lane]=floatBitsToUint(p.y);
    memory[{vertex+64}+lane]=0u;
    memory[{vertex+96}+lane]=floatBitsToUint(1.0);
    memory[{vertex+128}+lane]=floatBitsToUint(float(parameters.blue)/255.0);
    memory[{vertex+160}+lane]=floatBitsToUint(1.0);
    if (lane!=0) return;
    for (uint p=0;p<2;p++) {{
        memory[{vertex+192}+p]=floatBitsToUint(float(group%251u)/255.0);
        memory[{vertex+224}+p]=floatBitsToUint(float(p+1u)/255.0);
        memory[{vertex+256}+p]=0u;
        memory[{vertex+288}+p]=floatBitsToUint(1.0);
    }}
    memory[{primitive}]=(group%3u==0u ? 0u : 2u)<<24;
    memory[{primitive+1}]=0x00020100u;
    memory[{primitive+2}]=0x00000201u;
}}
'''
    fragment = '''#version 450
layout(location=0) flat in vec4 primitive;
layout(location=1) in vec4 vertex;
layout(location=0) out vec4 color;
void main() { color=vec4(primitive.xy,vertex.x,vertex.y); }
'''
    validator, optimizer = shutil.which('spirv-val'), shutil.which('spirv-opt')
    for key, stage, code in [('00000000feedab01', 'ms', source), ('00000000feedab02', 'ps', fragment)]:
        raw = subprocess.check_output([os.environ.get('GLSLC', 'glslc'), '--target-env=vulkan1.1',
            '-fshader-stage=' + ('compute' if stage == 'ms' else 'fragment'), '-', '-o', '-'], input=code.encode())
        reflection = dict(stage=stage, bindings=[], vertex_inputs=[])
        if stage == 'ms':
            reflection.update(bindings=[dict(msl_name='parameters', kind='cbuf', reg=0)],
                threadgroup_size=[8, 1, 1], mesh_stage=info, stage_memory=offsets)
        vk.write_stage(key, raw, reflection, output, validator, optimizer)


if __name__ == '__main__':
    main()
