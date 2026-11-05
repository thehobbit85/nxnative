"""Actual Maxwell depth-compare sample and gather instructions."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path[:0] = [str(ROOT), str(ROOT / 'dm/tools/tests')]
from test_txq_type import base, shader, handle, mov, SLOT_128, SAMPLER_1
from test_dmsh_pool_arrays import NS
from dm.android.tools import spv_android as vk


def native_metal(array, gather):
    """Independent native control with the fixture's fixed argument-buffer ABI."""
    shape = 'depth2d_array' if array else 'depth2d'
    coord = 'float2(as_type<float>(a.u[1]),as_type<float>(a.u[2]))' if array else 'float2(as_type<float>(a.u[0]),as_type<float>(a.u[1]))'
    layer = ', a.u[0]' if array else ''
    offsets = 'int2(int(a.u[4]<<26)>>26,int(a.u[4]<<18)>>26)' if gather else 'int2(int(a.u[4]<<28)>>28,int(a.u[4]<<24)>>28)'
    call = f'a.image.{"gather_compare" if gather else "sample_compare"}(a.sampler_state, {coord}{layer}, as_type<float>(a.u[5])'
    call += f', {offsets})' if gather else f', level(as_type<float>(a.u[3])), {offsets})'
    result = f'uint4 value=as_type<uint4>({call});' if gather else f'uint4 value=uint4(as_type<uint>({call}),0x76543210u,0x12345678u,0xabcdef01u);'
    return f'''#include <metal_stdlib>
using namespace metal;
struct Arguments {{
    constant uint *u [[id(0)]];
    device uint *out [[id(1)]];
    {shape}<float> image [[id(2)]];
    sampler sampler_state [[id(3)]];
}};
kernel void main0(constant Arguments &a [[buffer(0)]]) {{
    {result}
    for(uint c=0;c<4;c++)a.out[c]=value[c];
}}
'''


def program(array, gather):
    def load(dst, offset, width=4):
        return base('Ldc') | (width << 48) | (3 << 36) | (offset << 20) | (7 << 16) | (255 << 8) | dst
    operations = handle(6, SLOT_128, SAMPLER_1) + [load(0, 0), load(1, 4), load(2, 8)]
    if gather: operations += [load(7, 16), load(8, 20)]
    else: operations += [load(7, 12), load(8, 16), load(9, 20)]
    operations += [mov(17, 0x76543210), mov(18, 0x12345678), mov(19, 0xabcdef01)]
    instruction = base('Tld4B') if gather else base('TexB') | (3 << 37)
    instruction |= (1 << 50) | (1 << 36) | ((15 if gather else 1) << 31) | ((3 if array else 2) << 28) | (6 << 20) | (7 << 16) | 16
    operations += [instruction, load(4, 56, 5), base('Stg') | (6 << 48) | (1 << 45) | (7 << 16) | (4 << 8) | 16]
    return shader(operations)


def generate(work):
    work.mkdir(parents=True, exist_ok=True); jobs = []
    for array in (False, True):
        for gather in (False, True):
            key = f'{0xfeed9400 + len(jobs):016x}'; path = work / (key + '.bin'); path.write_bytes(program(array, gather))
            jobs.append(dict(key=key, stage='cs', code=str(path), tex={'128': '2darray' if array else '2d'}, local=[1, 1, 1]))
    translator = os.environ.get('DMSH_TEST_XLATE', str(ROOT / 'dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate'))
    for target in ('apple', 'vulkan'):
        out = work / target; out.mkdir(exist_ok=True); path = out / 'jobs.json'
        path.write_text(json.dumps(jobs if target == 'apple' else [dict(j, host_target='vulkan') for j in jobs]))
        subprocess.run([translator, path, out], check=True, capture_output=True)
    out = work / 'export'; out.mkdir(exist_ok=True)
    for job in jobs:
        key = job['key']; raw = (work / 'vulkan' / (key + '.spv')).read_bytes()
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
        variant = int(key, 16) - 0xfeed9400
        (native / (key + '.metal')).write_text(native_metal(bool(variant // 2), bool(variant % 2)))
        (native / (key + '.reflection.json')).write_text(json.dumps(reflection, indent=2))
    return out


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__); parser.add_argument('directory', type=Path)
    print(generate(parser.parse_args().directory.resolve()))
