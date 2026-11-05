"""Actual Maxwell signed/unsigned loads across uniform, inline and driver banks."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT)); sys.path.insert(0, str(ROOT / 'dm/tools/tests'))
from test_txq_type import base, shader, mov
from test_dmsh_pool_arrays import NS
from dm.android.tools import spv_android as vk

BANKS = (0, 1, 2, 4, 16, 24)


def pattern(bank, index):
    return (index * 73 + (index >> 8) * 29 + bank * 37 + 19) & 255


def load(dst, bank, offset=0, src=255, width=4):
    return base('Ldc') | (width << 48) | (bank << 36) | (offset << 20) | (7 << 16) | (src << 8) | dst


def generate(work, wrong_offset=False):
    work.mkdir(parents=True, exist_ok=True); jobs = []
    for bank in BANKS:
        for width in range(6):
            key = f'{0xfeed9000 + len(jobs):016x}'; binary = work / (key + '.bin')
            binary.write_bytes(shader(load(10, 3), mov(17, 0x76543210),
                                      load(16, bank, offset=8 if wrong_offset else 0, src=10, width=width),
                                      load(4, 3, 56, width=5), base('Stg') | (5 << 48) | (1 << 45) | (7 << 16) | (4 << 8) | 16))
            jobs.append(dict(key=key, stage='cs', code=str(binary), c1=bytes(pattern(1, i) for i in range(256)).hex(), local=[1, 1, 1]))
    translator = os.environ.get('DMSH_TEST_XLATE', str(ROOT / 'dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate'))
    for target in ('apple', 'vulkan'):
        out = work / target; out.mkdir(exist_ok=True); path = out / 'jobs.json'
        path.write_text(json.dumps(jobs if target == 'apple' else [dict(j, host_target='vulkan') for j in jobs]))
        subprocess.run([translator, path, out], check=True, capture_output=True)
    out = work / 'export'; out.mkdir(exist_ok=True)
    for job in jobs:
        key = job['key']; raw = (work / 'vulkan' / (key + '.spv')).read_bytes()
        if raw != (work / 'apple' / (key + '.spv')).read_bytes(): raise ValueError('constant-bank translation differs by target')
        source = subprocess.run([ROOT / 'dm/gfx/tools/spirv_cross_bin/spirv-cross', '--msl', '--msl-version', '20300',
                                 '--msl-argument-buffers', work / 'apple' / (key + '.spv')], check=True, capture_output=True, text=True).stdout
        (work / (key + '.metal')).write_text(source)
        res = json.loads((work / 'apple' / (key + '.res.json')).read_text())
        if res['notes']: raise ValueError(res['notes'])
        reflection = dict(stage='cs', c1=job['c1'], threadgroup_size=[1, 1, 1], bindings=NS['reflect'](key, 'cs', source, res))
        (work / (key + '.reflection.json')).write_text(json.dumps(reflection, indent=2))
        vk.write_stage(key, raw, reflection, out, shutil.which('spirv-val'), shutil.which('spirv-opt'))
    return out


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__); parser.add_argument('directory', type=Path)
    parser.add_argument('--wrong-offset', action='store_true'); args = parser.parse_args()
    print(generate(args.directory.resolve(), args.wrong_offset))
