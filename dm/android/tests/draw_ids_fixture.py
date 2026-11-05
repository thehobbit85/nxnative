"""Maxwell vertex/instance attributes and C0 draw values, through shared translation."""
import argparse
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path[:0] = [str(ROOT), str(ROOT / 'dm/tools/tests')]
from test_txq_type import base, shader, mov
from test_dmsh_pool_arrays import NS
from dm.android.tools import spv_android as vk

KEY = '00000000feed9200'


def program(wrong_vertex=False):
    def load(dst, bank, offset, width=4):
        return base('Ldc') | (width << 48) | (bank << 36) | (offset << 20) | (7 << 16) | (255 << 8) | dst
    def attribute(dst, offset):
        return base('Ald') | (7 << 16) | (255 << 8) | (255 << 39) | (offset << 20) | dst
    def output(src, offset):
        return base('Ast') | (7 << 16) | (255 << 8) | (offset << 20) | src
    header = bytearray(0x50); struct.pack_into('<I', header, 0, (1 << 10) | (3 << 5) | 1)
    operations = [attribute(16, 0x2f8 if wrong_vertex else 0x2fc), attribute(17, 0x2f8), load(18, 0, 0x644), load(19, 0, 0x610),
        load(4, 3, 56, 5), mov(9, 4), base('Shl') | (9 << 20) | (7 << 16) | (17 << 8) | 8,
        base('Iadd') | (8 << 20) | (7 << 16) | (4 << 8) | 4,
        base('Stg') | (6 << 48) | (1 << 45) | (7 << 16) | (4 << 8) | 16,
        mov(20, 0), mov(21, 0x3f800000)]
    operations += [output(20, off) for off in (0x70, 0x74, 0x78)] + [output(21, 0x7c)]
    return bytes(header) + shader(operations)


def generate(work, wrong_vertex=False):
    work.mkdir(parents=True, exist_ok=True); binary = work / 'vertex.bin'; binary.write_bytes(program(wrong_vertex))
    job = dict(key=KEY, stage='vs', code=str(binary))
    translator = os.environ.get('DMSH_TEST_XLATE', str(ROOT / 'dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate'))
    for target in ('apple', 'vulkan'):
        out = work / target; out.mkdir(exist_ok=True); path = out / 'jobs.json'
        path.write_text(json.dumps([job if target == 'apple' else dict(job, host_target='vulkan')]))
        subprocess.run([translator, path, out], check=True, capture_output=True)
    raw = (work / 'apple' / (KEY + '.spv')).read_bytes()
    if raw != (work / 'vulkan' / (KEY + '.spv')).read_bytes(): raise ValueError('draw-ID translation differs by target')
    source = subprocess.run([ROOT / 'dm/gfx/tools/spirv_cross_bin/spirv-cross', '--msl', '--msl-version',
                             '20300', '--msl-argument-buffers', work / 'apple' / (KEY + '.spv')],
                            check=True, capture_output=True, text=True).stdout
    (work / (KEY + '.metal')).write_text(source)
    res = json.loads((work / 'apple' / (KEY + '.res.json')).read_text())
    if res['notes']: raise ValueError(res['notes'])
    reflection = dict(stage='vs', bindings=NS['reflect'](KEY, 'vs', source, res))
    (work / (KEY + '.reflection.json')).write_text(json.dumps(reflection, indent=2))
    out = work / 'export'; out.mkdir(exist_ok=True)
    vk.write_stage(KEY, raw, reflection, out, shutil.which('spirv-val'), shutil.which('spirv-opt'))
    return out


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__); parser.add_argument('directory', type=Path)
    parser.add_argument('--wrong-vertex', action='store_true'); args = parser.parse_args()
    print(generate(args.directory.resolve(), args.wrong_vertex))
