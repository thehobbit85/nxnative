"""Actual Maxwell probes for wrapped fetches, logical slot arrays and large interfaces."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT)); sys.path.insert(0, str(ROOT / 'dm/tools/tests'))
from dm.android.tools import spv_android as vk
from test_txq_type import base, mov, shader, handle, SAMPLER_1
from test_dmsh_pool_arrays import NS


def ldc(reg, bank, offset, width=4):
    return base('Ldc') | (width << 48) | (bank << 36) | (offset << 20) | (7 << 16) | (255 << 8) | reg


def code(variant):
    if variant in ('many', 'many_pool'):
        ops = [mov(20, 0), mov(0, 0), mov(1, 0)]
        add = base('Iadd') | (8 << 20) | (7 << 16) | (20 << 8) | 20
        for slot in range(40):
            ops += handle(6, 0x20 + 8 * slot, SAMPLER_1)
            ops += [base('TexB') | (1 << 37) | (1 << 31) | (2 << 28) | (6 << 20) | (7 << 16) | 8, add]
        for bank in range(3, 13): ops += [ldc(8, bank, 0), add]
        for slot in range(16):
            ops += [ldc(4, 0, 0x310 + slot * 16, 5),
                    base('Ldg') | (4 << 48) | (1 << 45) | (7 << 16) | (4 << 8) | 8, add]
        if variant == 'many_pool':
            ops += [base('Ldg') | (4 << 48) | (1 << 45) | (7 << 16) | (4 << 8) | 6,
                    base('TexB') | (1 << 37) | (1 << 31) | (2 << 28) | (6 << 20) | (7 << 16) | 8, add]
        return shader(ops, base('Stg') | (4 << 48) | (1 << 45) | (7 << 16) | (4 << 8) | 20)
    # Per-lane handles from guest storage prevent constant-folding to one slot.
    ops = [ldc(4, 3, 0, 5), base('S2r') | (33 << 20) | (7 << 16) | 14, mov(15, 2),
           base('Shl') | (15 << 20) | (7 << 16) | (14 << 8) | 14,
           base('Iadd') | (14 << 20) | (7 << 16) | (4 << 8) | 4,
           base('Ldg') | (4 << 48) | (1 << 45) | (7 << 16) | (4 << 8) | 6,
           ldc(0, 3, 16), mov(1, 0)]
    if variant == 'slots':
        ops += [base('TldB') | (1 << 31) | (6 << 20) | (7 << 16) | 8]
    elif variant in ('wrapped', 'size'):
        ops += [base('Tld') | (1 << 31) | (0x5a << 36) | (7 << 16) | 8]
        if variant == 'size':
            ops += [mov(4, 0), base('Txq') | (0x5a << 36) | (1 << 31) | (1 << 22) | (7 << 16) | (4 << 8) | 9,
                    base('Iadd') | (9 << 20) | (7 << 16) | (8 << 8) | 8]
    else: raise ValueError('unknown resource fixture')
    ops += [ldc(12, 3, 56, 5), base('Iadd') | (14 << 20) | (7 << 16) | (12 << 8) | 12,
            base('Stg') | (4 << 48) | (1 << 45) | (7 << 16) | (12 << 8) | 8]
    return shader(ops)


def generate(work, variant):
    work.mkdir(parents=True, exist_ok=True); apple = work / 'apple'; apple.mkdir(exist_ok=True)
    export = work / 'export'; export.mkdir(exist_ok=True)
    key = '00000000feed6100'; path = work / (key + '.bin'); path.write_bytes(code(variant))
    job = dict(key=key, stage='cs', code=str(path), local=[1 if variant.startswith('many') else 8, 1, 1])
    translator = os.environ.get('DMSH_TEST_XLATE', str(ROOT / 'dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate'))
    for output, extra in ((apple, {}), (work, dict(host_target='vulkan'))):
        jobs = output / 'jobs.json'; jobs.write_text(json.dumps([dict(job, **extra)]))
        subprocess.run([translator, jobs, output], check=True, capture_output=True)
    raw = (work / (key + '.spv')).read_bytes()
    if raw != (apple / (key + '.spv')).read_bytes(): raise ValueError('resource translation differs by target')
    source = subprocess.run([ROOT / 'dm/gfx/tools/spirv_cross_bin/spirv-cross', '--msl', '--msl-version', '20300',
                             '--msl-argument-buffers', '--msl-texture-buffer-native', apple / (key + '.spv')],
                            capture_output=True, text=True, check=True).stdout
    # Keep the shared small slot arrays. The normal pool wrapper leaves buffer arrays alone.
    source = NS['pool_arrays'](source); (work / (key + '.metal')).write_text(source)
    resources = json.loads((work / (key + '.res.json')).read_text())
    if any('not implemented' in n or 'Invalid' in n for n in resources['notes']): raise ValueError(resources['notes'])
    reflection = dict(stage='cs', threadgroup_size=job['local'], bindings=NS['reflect'](key, 'cs', source, resources))
    (work / (key + '.reflection.json')).write_text(json.dumps(reflection, indent=2))
    vk.write_stage(key, raw, reflection, export, shutil.which('spirv-val'), shutil.which('spirv-opt'))
    return export


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path); parser.add_argument('variant', choices=('wrapped', 'size', 'slots', 'many', 'many_pool'))
    args = parser.parse_args(); print(generate(args.directory.resolve(), args.variant))
