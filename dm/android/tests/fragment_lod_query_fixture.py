"""Maxwell derivative-based LOD queries with both fixed-point result components."""
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


def program(swapped):
    def load(reg, offset):
        return base('Ldc') | (4 << 48) | (3 << 36) | (offset << 20) | (7 << 16) | (255 << 8) | reg
    def ipa(reg, offset):
        return base('Ipa') | (offset << 28) | (7 << 16) | reg
    def multiply(reg, a, b):
        return base('Fmul') | (b << 20) | (7 << 16) | (a << 8) | reg
    def to_float(reg):
        return base('I2f') | (reg << 20) | (7 << 16) | (1 << 13) | (2 << 10) | (2 << 8) | reg
    header=bytearray(0x50)
    struct.pack_into('<I',header,0,(5<<10)|(3<<5)|1)
    struct.pack_into('<I',header,72,15)
    ops=handle(6,SLOT_128,SAMPLER_1)+[ipa(0,0x74 if swapped else 0x70),ipa(1,0x70 if swapped else 0x74),load(10,0),load(11,4),
        multiply(0,0,10),multiply(1,1,11),mov(2,0x449a4000),mov(3,0x45b17000),
        base('TmmlB')|(3<<31)|(2<<28)|(6<<20)|(7<<16),to_float(0),to_float(1)]
    return bytes(header)+shader(ops)


def generate(work):
    work.mkdir(parents=True, exist_ok=True)
    translator = os.environ.get('DMSH_TEST_XLATE', str(ROOT / 'dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate'))
    jobs = []
    for swapped in (False, True):
        key = f'{0xfeed9700 + swapped:016x}'; binary = work / (key + '.bin')
        binary.write_bytes(program(swapped))
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
