"""Actual Maxwell full dimension queries for all supported texture shapes."""
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


SHAPES = ('1d', '1darray', '2d', '2darray', '3d', 'cube', 'cubearray')


def program(variant):
    def load(dst, offset, width=4):
        return base('Ldc') | (width << 48) | (3 << 36) | (offset << 20) | (7 << 16) | (255 << 8) | dst
    # Native Metal 1D textures have only mip zero and require a constant LOD.
    operations = handle(6, SLOT_128, SAMPLER_1) + [mov(7, 0) if variant < 2 else load(7, 0)]
    operations += [base('TxqB') | (1 << 22) | (15 << 31) | (6 << 8) | (7 << 16) | 16,
                   load(4, 56, 5), base('Stg') | (6 << 48) | (1 << 45) | (7 << 16) | (4 << 8) | 16]
    return shader(operations)


def generate(work):
    work.mkdir(parents=True, exist_ok=True); jobs = []
    for variant, shape in enumerate(SHAPES):
        key = f'{0xfeed9600 + variant:016x}'; path = work / (key + '.bin'); path.write_bytes(program(variant))
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
    return out


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__); parser.add_argument('directory', type=Path)
    print(generate(parser.parse_args().directory.resolve()))
