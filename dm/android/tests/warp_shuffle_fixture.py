"""Translate Maxwell SHFL register exchanges and write independent per-lane expectations."""
import argparse
import json
import math
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path[:0] = [str(ROOT), str(ROOT / 'dm/tools/tests')]
from dm.android.tools import spv_android
from test_dmsh_pool_arrays import NS
from test_txq_type import base, mov, shader

# (SHFL mode, immediate source or distance, segment mask and clamp)
SHUFFLES = (
    ((0, 0, 31),), ((0, 31, 31),), ((1, 1, 0),), ((2, 1, 31),),
    ((3, 16, 31),), ((3, 1, 31),), ((0, 2, 0x1c03),),
    ((3, 16, 31), (2, 1, 31), (3, 2, 31)),  # reuses scratch repeatedly
)
GUARD = 0xbaddecaf
SEED = 0x123400


def guest_code(steps, local):
    pred = 7 << 16  # unconditional
    ops = [base('S2r') | (33 << 20) | pred | 14]  # R14 = thread ID x
    for dimension in (1, 2):
        if local[dimension] > 1:
            stride = math.prod(local[:dimension])
            assert stride & (stride - 1) == 0
            ops.extend([base('S2r') | ((33 + dimension) << 20) | pred | 16,
                        mov(10, stride.bit_length() - 1),
                        base('Shl') | (10 << 20) | pred | (16 << 8) | 16,
                        base('Iadd') | (16 << 20) | pred | (14 << 8) | 14])
    ops.extend([mov(10, SEED), base('Iadd') | (10 << 20) | pred | (14 << 8) | 8])
    for mode, selector, clamp in steps:
        ops.append(base('Shfl') | (7 << 48) | (clamp << 34) | (mode << 30) |
                   (3 << 28) | (selector << 20) | pred | (8 << 8) | 8)
    # Store the final R8 in the guest output buffer at thread ID x * 4.
    ops.extend([base('Ldc') | (5 << 48) | (3 << 36) | (56 << 20) | pred | (255 << 8) | 12,
                mov(15, 2), base('Shl') | (15 << 20) | pred | (14 << 8) | 14,
                base('Iadd') | (14 << 20) | pred | (12 << 8) | 12,
                base('Stg') | (4 << 48) | (1 << 45) | pred | (12 << 8) | 8])
    return shader(ops)


def expected(width, steps):
    values = [SEED + tid for tid in range(width)]
    for mode, selector, control in steps:
        result = []
        for tid in range(width):
            lane = tid % 32
            segment = (control >> 8) & 31
            minimum = lane & segment
            maximum = minimum | (control & 31 & ~segment)
            source = (minimum | (selector & ~segment), lane - selector,
                      lane + selector, lane ^ selector)[mode]
            valid = source >= minimum if mode == 1 else source <= maximum
            result.append(values[tid - lane + source] if valid else values[tid])
        values = result
    return values + [GUARD] * 8


def generate(work):
    work.mkdir(parents=True, exist_ok=True)
    jobs = []
    cases = []
    for local in ([32, 1, 1], [64, 1, 1], [128, 1, 1], [8, 4, 2], [36, 1, 1]):
        width = math.prod(local)
        # The last partial warp has four active lanes. Compare only shuffles
        # whose selected source exists; inactive sources have undefined data.
        variants = SHUFFLES if width % 32 == 0 else [SHUFFLES[i] for i in (0, 2, 5, 6)]
        for steps in variants:
            key = f'{0xfeed9d00 + len(jobs):016x}'
            code = work / (key + '.bin')
            code.write_bytes(guest_code(steps, local))
            jobs.append(dict(key=key, stage='cs', code=str(code), local=local))
            cases.append(dict(key=key, local=local, expected=expected(width, steps)))

    translator = os.environ.get('DMSH_TEST_XLATE', ROOT / 'dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate')
    for target in ('apple', 'vulkan'):
        directory = work / target
        directory.mkdir(exist_ok=True)
        manifest = directory / 'jobs.json'
        manifest.write_text(json.dumps(jobs if target == 'apple' else [dict(j, host_target='vulkan') for j in jobs]))
        subprocess.run([translator, manifest, directory], check=True, capture_output=True)

    out = work / 'export'
    out.mkdir(exist_ok=True)
    cross = ROOT / 'dm/gfx/tools/spirv_cross_bin/spirv-cross'
    for case in cases:
        key = case['key']
        apple = work / 'apple' / (key + '.spv')
        msl = subprocess.run([cross, '--msl', '--msl-version', '20300', '--msl-argument-buffers', apple],
                             check=True, capture_output=True, text=True).stdout
        (work / (key + '.metal')).write_text(msl)
        res = json.loads(apple.with_suffix('.res.json').read_text())
        if res['notes']:
            raise ValueError(res['notes'])
        bindings = NS['reflect'](key, 'cs', msl, res)
        if len(bindings) != 1 or bindings[0]['kind'] != 'sbuf_ubo' or bindings[0]['off'] != 56:
            raise ValueError(bindings)
        case['metal_slot'] = bindings[0]['arg_id']
        reflection = dict(stage='cs', threadgroup_size=case['local'], bindings=bindings)
        spv_android.write_stage(key, (work/'vulkan'/apple.name).read_bytes(), reflection, out,
                                shutil.which('spirv-val'), shutil.which('spirv-opt'))
        records = [line.split() for line in (out/(key+'.vk.txt')).read_text().splitlines() if line.startswith('b ')]
        if len(records) != 1 or records[0][2:7] != ['ssbo', '1', 'sbuf_ubo', '0', '56']:
            raise ValueError(records)
        words = case.pop('expected')
        (work/(key+'.expected')).write_bytes(struct.pack(f'<{len(words)}I', *words))
        script = [f'#!amber\nSHADER compute guest SPIRV-BIN FILE "{key}.spv"',
                  f'BUFFER out DATA_TYPE uint32 SIZE {len(words)} FILL {GUARD}', 'PIPELINE compute check',
                  '  ATTACH guest', f'  BIND BUFFER out AS storage DESCRIPTOR_SET 0 BINDING {records[0][1]}',
                  'END', 'RUN check 1 1 1', 'EXPECT out IDX 0 EQ ' + ' '.join(map(str, words))]
        (out/(key+'.amber')).write_text('\n'.join(script) + '\n')
    (work/'cases.json').write_text(json.dumps(cases, indent=2) + '\n')
    return out


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    print(generate(parser.parse_args().directory.resolve()))
