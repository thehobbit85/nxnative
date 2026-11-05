"""Maxwell implicit/bias samples with texel offsets varying within each quad."""
import argparse
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT)); sys.path.insert(0, str(ROOT / 'dm/tools/tests'))
from dm.android.tools import spv_android as vk
from test_txq_type import base, handle, mov, shader, SLOT_128, SAMPLER_1
from test_dmsh_pool_arrays import NS


def program(bias):
    def load(reg, offset):
        return base('Ldc') | (4 << 48) | (3 << 36) | (offset << 20) | (7 << 16) | (255 << 8) | reg
    def ipa(reg, offset):
        return base('Ipa') | (offset << 28) | (7 << 16) | reg
    def f2i(reg, source):
        return base('F2i') | (3 << 39) | (source << 20) | (7 << 16) | (2 << 10) | (2 << 8) | reg
    def multiply(reg, a, b):
        return base('Fmul') | (b << 20) | (7 << 16) | (a << 8) | reg
    header = bytearray(0x50)
    struct.pack_into('<I', header, 0, (5 << 10) | (3 << 5) | 1)
    struct.pack_into('<I', header, 72, 15)  # Fragment RGBA output in R0..R3.
    # FragCoord supplies every signed-four-bit pair over a 16x16 target.
    # Coordinates use the original continuous position, never the offsets.
    ops = handle(6, SLOT_128, SAMPLER_1) + [ipa(0, 0x70), ipa(1, 0x74),
        f2i(12, 0), f2i(13, 1), mov(14, 4),
        base('Shl') | (14 << 20) | (7 << 16) | (13 << 8) | 13,
        base('Lop') | (1 << 41) | (13 << 20) | (7 << 16) | (12 << 8) | (8 if bias else 7)]
    if bias: ops += [load(7, 8)]
    ops += [load(10, 0), load(11, 4), multiply(0, 0, 10), multiply(1, 1, 11),
            base('TexB') | ((2 if bias else 0) << 37) | (1 << 36) | (15 << 31) |
            (2 << 28) | (6 << 20) | (7 << 16)]
    return bytes(header) + shader(ops)


def generate(work):
    work.mkdir(parents=True, exist_ok=True)
    translator = os.environ.get('DMSH_TEST_XLATE', str(ROOT / 'dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate'))
    jobs = []
    for bias in (False, True):
        key = f'{0xfeed8000 + bias:016x}'; binary = work / (key + '.bin')
        binary.write_bytes(program(bias))
        jobs.append(dict(key=key, stage='ps', code=str(binary), tex={'128': '2d'}, fout={'0': 'float'}))
    for target in ('apple', 'vulkan'):
        out = work / target; out.mkdir(exist_ok=True)
        selected = jobs if target == 'apple' else [dict(job, host_target='vulkan') for job in jobs]
        path = out / 'jobs.json'; path.write_text(json.dumps(selected))
        subprocess.run([translator, path, out], check=True)
        export = out / 'export'; export.mkdir(exist_ok=True)
        for job in jobs:
            key = job['key']; binary = out / (key + '.spv')
            source = subprocess.check_output([ROOT / 'dm/gfx/tools/spirv_cross_bin/spirv-cross', '--msl',
                '--msl-version', '20300', '--msl-argument-buffers', binary], text=True)
            (out / (key + '.metal')).write_text(source)
            resources = json.loads((out / (key + '.res.json')).read_text())
            if resources['notes']: raise ValueError(resources['notes'])
            reflection = dict(stage='ps', bindings=NS['reflect'](key, 'ps', source, resources))
            (out / (key + '.reflection.json')).write_text(json.dumps(reflection, indent=2))
            vk.write_stage(key, binary.read_bytes(), reflection, export, shutil.which('spirv-val'), shutil.which('spirv-opt'))
    return work


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__); parser.add_argument('directory', type=Path)
    print(generate(parser.parse_args().directory.resolve()))
