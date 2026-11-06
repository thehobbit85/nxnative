"""Export actual returning Maxwell image atomics for Apple and Android execution.

Reuse the shared native Metal atomic fixture. Amber's SPIRV-BIN input executes
exactly the exported Vulkan product, using its serialized binding number.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path[:0] = [str(ROOT), str(ROOT/'dm/tools/tests')]
from test_image_atomic_bounds import synthetic_shader, cross
from test_dmsh_pool_arrays import NS
from test_txq_type import mov
from dm.android.tools import spv_android as vk


def generate(work):
    work.mkdir(parents=True, exist_ok=True); jobs = []; rows = []
    for dim, shape, image, n in ((1, 'buffer', 'buf', 8), (3, '2d', '2d', 32), (5, '3d', '3d', 96)):
        for x in (-1, 0, 8):
            for compare in (None, 2, 3):
                key = f'{0xfeed9b00 + len(jobs):016x}'
                code = work/(key+'.bin')
                seed = synthetic_shader(x, 0, dim=dim, discard=False, consume_result=True,
                                        cas=None if compare is None else (compare, 7))
                if compare is None:
                    # The shared UMax seed uses 1 against a texel containing 3.
                    # Use 7 to distinguish a store of 7 + old 3 from either a
                    # plain read (3+3) or incorrectly returning the new 7 (7+7).
                    words = list(struct.unpack(f'<{len(seed)//8}Q', seed))
                    if words.count(mov(3, 1)) != 1: raise ValueError('unexpected UMax seed operand')
                    words[words.index(mov(3, 1))] = mov(3, 7)
                    seed = struct.pack(f'<{len(words)}Q', *words)
                code.write_bytes(seed)
                jobs.append(dict(key=key, stage='cs', code=str(code), img={'0':dict(type=image, fmt='R32Uint')}, local=[1,1,1], debug=True))
                expected = 3 if x != 0 else 6 if compare == 2 else 10
                rows.append(dict(key=key, shape=shape, count=n, x=x, compare=compare, expected=expected))
    translator = os.environ.get('DMSH_TEST_XLATE', str(ROOT/'dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate'))
    for target in ('apple', 'vulkan'):
        out = work/target; out.mkdir(exist_ok=True); path = out/'jobs.json'
        path.write_text(json.dumps(jobs if target == 'apple' else [dict(job, host_target='vulkan') for job in jobs]))
        subprocess.run([translator, path, out], check=True, capture_output=True)
    out = work/'export'; out.mkdir(exist_ok=True)
    for row in rows:
        key = row['key']; source = cross(work/'apple'/(key+'.spv'), work/(key+'.metal'))
        res = json.loads((work/'apple'/(key+'.res.json')).read_text())
        if res['notes']: raise ValueError(res['notes'])
        refl = dict(stage='cs', threadgroup_size=[1,1,1], bindings=NS['reflect'](key, 'cs', source, res))
        raw = (work/'vulkan'/(key+'.spv')).read_bytes(); vk.write_stage(key, raw, refl, out, shutil.which('spirv-val'), shutil.which('spirv-opt'))
        resources = [line.split() for line in (out/(key+'.vk.txt')).read_text().splitlines() if line.startswith('b ')]
        buffer = row['shape'] == 'buffer'; kind = 'stb' if buffer else 'simg'
        if len(resources) != 1 or resources[0][2:6] != [kind,'1','img','0']:
            raise ValueError(f'unexpected image atomic binding: {resources}')
        binding = int(resources[0][1]); n = row['count']
        decl = ('BUFFER im DATA_TYPE R32_UINT SIZE 8' if buffer else
                'IMAGE im DATA_TYPE uint32 DIM_2D WIDTH 8 HEIGHT 4' if row['shape'] == '2d' else
                'IMAGE im DATA_TYPE uint32 DIM_3D WIDTH 8 HEIGHT 4 DEPTH 3')
        descriptor = 'storage_texel_buffer' if buffer else 'storage_image'
        script = f'''#!amber
SHADER compute guest SPIRV-BIN FILE "{key}.spv"
{decl} FILL 3
PIPELINE compute test
  ATTACH guest
  BIND BUFFER im AS {descriptor} DESCRIPTOR_SET 0 BINDING {binding}
END
RUN test 1 1 1
EXPECT im IDX 0 EQ {row['expected']} {' '.join(['3'] * (n-1))}
'''
        (out/(key+'.amber')).write_text(script)
    (work/'cases.json').write_text(json.dumps(rows, indent=2)+'\n')
    return out


def metal_run(work):
    for row in json.loads((work/'cases.json').read_text()):
        key = row['key']; air = work/(key+'.air'); lib = work/(key+'.metallib')
        subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-c', '-fpreserve-invariance',
                        '-fmodules-cache-path='+str(work/'modules'), work/(key+'.metal'), '-o', air], check=True)
        subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', air, '-o', lib], check=True)
        subprocess.run([work/'atomic_metal', lib, str(row['expected']), row['shape']], check=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__); parser.add_argument('directory', type=Path)
    parser.add_argument('--metal-run', action='store_true'); args = parser.parse_args(); work = args.directory.resolve()
    if args.metal_run: metal_run(work)
    else: print(generate(work))
