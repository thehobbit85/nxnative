"""Test returning Maxwell image atomics on separate slices of a storage array."""
import json
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
from test_image_atomic_bounds import synthetic_shader, cross
from test_txq_type import mov


def generate(work):
    out = work / 'export'
    out.mkdir(parents=True, exist_ok=True)
    jobs, cases = [], []
    # Different initial words and sentinels on every layer expose swapped
    # coordinates, a view of only layer zero, or a clamped out-of-range write.
    initial = [13 + i for i in range(8 * 4 * 3)]
    for x, y, z in ((0, 0, 0), (3, 2, 1), (7, 3, 2), (-1, 0, 1), (8, 2, 1), (1, 4, 2), (0, 0, -1), (0, 0, 3)):
        pos = x + 8 * (y + 4 * z) if 0 <= x < 8 and 0 <= y < 4 and 0 <= z < 3 else None
        old = initial[pos] if pos is not None else 0
        for compare in (None, old, old + 1):
            key = f'{0xfeeda400 + len(cases):016x}'
            new = 200
            binary = synthetic_shader(x, y, z, dim=4, discard=False, consume_result=True,
                                      cas=None if compare is None else (compare, new))
            if compare is None:
                words = list(struct.unpack(f'<{len(binary)//8}Q', binary))
                if words.count(mov(3, 1)) != 1:
                    raise ValueError('unexpected UMax operand')
                words[words.index(mov(3, 1))] = mov(3, new)
                binary = struct.pack(f'<{len(words)}Q', *words)
            path = work / (key + '.bin')
            path.write_bytes(binary)
            jobs.append(dict(key=key, stage='cs', code=str(path), img={'0': dict(type='2darray', fmt='R32Uint')},
                             local=[1, 1, 1], debug=True))
            expected = initial.copy()
            if pos is not None and (compare is None or old == compare):
                expected[pos] = new
            expected[0] += old
            for suffix, values in (('.initial', initial), ('.expected', expected)):
                (out / (key + suffix)).write_bytes(struct.pack('<96I', *values))
            cases.append(dict(key=key, vk_format='R32_UINT', layers=3, atomic=True))

    translator = os.environ.get('DMSH_TEST_XLATE', ROOT / 'dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate')
    for target in ('apple', 'vulkan'):
        d = work / target
        d.mkdir(exist_ok=True)
        path = d / 'jobs.json'
        path.write_text(json.dumps(jobs if target == 'apple' else [dict(job, host_target='vulkan') for job in jobs]))
        subprocess.run([translator, path, d], check=True, capture_output=True)
    manifest = []
    for row in cases:
        key = row['key']
        apple = work / 'apple' / (key + '.spv')
        source = cross(apple, work / (key + '.metal'))
        if 'texture2d_array<uint' not in source:
            raise ValueError('expected an array storage texture')
        res = json.loads(apple.with_suffix('.res.json').read_text())
        if res['notes']:
            raise ValueError(res['notes'])
        bindings = NS['reflect'](key, 'cs', source, res)
        if len(bindings) != 1 or bindings[0]['kind'] != 'img':
            raise ValueError(bindings)
        row['metal'] = {'img': bindings[0]['arg_id']}
        row['image_path'] = 'export/' + key
        spv_android.write_stage(key, (work / 'vulkan' / apple.name).read_bytes(),
                                dict(stage='cs', threadgroup_size=[1,1,1], bindings=bindings), out,
                                shutil.which('spirv-val'), shutil.which('spirv-opt'))
        records = [s.split() for s in (out / (key + '.vk.txt')).read_text().splitlines() if s.startswith('b ')]
        if len(records) != 1 or records[0][2:6] != ['simg', '1', 'img', '0']:
            raise ValueError(records)
        manifest.append(f'{key} {records[0][1]}')
    (work / 'cases.json').write_text(json.dumps(cases, indent=2) + '\n')
    (out / 'cases.list').write_text('\n'.join(manifest) + '\n')


if __name__ == '__main__':
    generate(Path(sys.argv[1]).resolve())
