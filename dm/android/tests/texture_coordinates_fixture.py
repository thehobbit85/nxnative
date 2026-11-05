"""Actual Maxwell normalized 1D and cube-coordinate samples."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path[:0] = [str(ROOT), str(ROOT / 'dm/tools/tests')]
from test_txq_type import base, shader, handle, SLOT_128, SAMPLER_1
from test_dmsh_pool_arrays import NS
from dm.android.tools import spv_android as vk


SHAPES = ('1d', '1darray', 'cube', 'cubearray')


def native_metal(variant):
    cube, array = variant >= 2, bool(variant % 2)
    shape = ('texturecube' if cube else 'texture2d') + ('_array' if array else '')
    start = 1 if array else 0
    coord = f'float3(as_type<float>(a.u[{start}]),as_type<float>(a.u[{start+1}]),as_type<float>(a.u[{start+2}]))' if cube else f'float2(as_type<float>(a.u[{start}]),0)'
    layer = ',a.u[0]' if array else ''
    return f'''#include <metal_stdlib>
using namespace metal;
struct Arguments {{
    constant uint *u [[id(0)]];
    device uint *out [[id(1)]];
    {shape}<float> image [[id(2)]];
    sampler sampler_state [[id(3)]];
}};
kernel void main0(constant Arguments &a [[buffer(0)]]) {{
    uint4 value=as_type<uint4>(a.image.sample(a.sampler_state,{coord}{layer},level(as_type<float>(a.u[4]))));
    for(uint c=0;c<4;c++)a.out[c]=value[c];
}}
'''


def program(variant):
    def load(dst, offset, width=4):
        return base('Ldc') | (width << 48) | (3 << 36) | (offset << 20) | (7 << 16) | (255 << 8) | dst
    operations = handle(6, SLOT_128, SAMPLER_1) + [load(i, 4*i) for i in range(4)] + [load(7, 16)]
    operations += [base('TexB') | (3 << 37) | (15 << 31) | ((0,1,6,7)[variant] << 28) | (6 << 20) | (7 << 16) | 16,
                   load(4, 56, 5), base('Stg') | (6 << 48) | (1 << 45) | (7 << 16) | (4 << 8) | 16]
    return shader(operations)


def generate(work):
    work.mkdir(parents=True, exist_ok=True); jobs = []
    for variant, shape in enumerate(SHAPES):
        key = f'{0xfeed9500 + variant:016x}'; path = work / (key + '.bin'); path.write_bytes(program(variant))
        jobs.append(dict(key=key, stage='cs', code=str(path), tex={'128': shape}, local=[1, 1, 1]))
    translator = os.environ.get('DMSH_TEST_XLATE', str(ROOT / 'dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate'))
    for target in ('apple', 'vulkan'):
        out = work / target; out.mkdir(exist_ok=True); path = out / 'jobs.json'
        path.write_text(json.dumps(jobs if target == 'apple' else [dict(j, host_target='vulkan') for j in jobs]))
        subprocess.run([translator, path, out], check=True, capture_output=True)
    out = work / 'export'; out.mkdir(exist_ok=True)
    for job in jobs:
        key = job['key']; raw = (work / 'vulkan' / (key + '.spv')).read_bytes()
        if raw != (work / 'apple' / (key + '.spv')).read_bytes(): raise ValueError('texture translation differs by target')
        source = subprocess.run([ROOT / 'dm/gfx/tools/spirv_cross_bin/spirv-cross', '--msl', '--msl-version',
                                 '20300', '--msl-argument-buffers', work / 'apple' / (key + '.spv')],
                                check=True, capture_output=True, text=True).stdout
        (work / (key + '.metal')).write_text(source)
        res = json.loads((work / 'apple' / (key + '.res.json')).read_text())
        if res['notes']: raise ValueError(res['notes'])
        reflection = dict(stage='cs', threadgroup_size=[1, 1, 1], bindings=NS['reflect'](key, 'cs', source, res))
        (work / (key + '.reflection.json')).write_text(json.dumps(reflection, indent=2))
        vk.write_stage(key, raw, reflection, out, shutil.which('spirv-val'), shutil.which('spirv-opt'))
        if [(b['kind'], b['arg_id']) for b in reflection['bindings']] != [('cbuf', 0), ('sbuf_ubo', 1), ('tex', 2), ('samp', 3)]:
            raise ValueError('native control requires the fixed fixture argument layout')
        native = work / 'native'; native.mkdir(exist_ok=True)
        (native / (key + '.metal')).write_text(native_metal(int(key, 16) - 0xfeed9500))
        (native / (key + '.reflection.json')).write_text(json.dumps(reflection, indent=2))
    return out


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__); parser.add_argument('directory', type=Path)
    print(generate(parser.parse_args().directory.resolve()))
