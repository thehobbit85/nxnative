"""Export actual Maxwell accesses through a pointer loaded from storage."""
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
from test_txq_type import base, mov, shader
from test_dmsh_pool_arrays import NS

VARIANTS = ([('load', n) for n in range(7)] + [('store', n) for n in (0, 2, 4, 5, 6)] +
            [('atomic', n) for n in (0, 1, 2, 5, 6, 7, 8)] + [('signed', n) for n in (1, 2)])


def generate(work):
    work.mkdir(parents=True, exist_ok=True); export = work / 'export'; export.mkdir(exist_ok=True)
    apple = work / 'apple'; apple.mkdir(exist_ok=True)
    translator = os.environ.get('DMSH_TEST_XLATE', str(ROOT / 'dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate'))
    jobs = []
    def ldc(reg, offset, width=5):
        return base('Ldc') | (width << 48) | (3 << 36) | (offset << 20) | (7 << 16) | (255 << 8) | reg
    def memory(name, reg, address, width):
        return base(name) | (width << 48) | (1 << 45) | (7 << 16) | (address << 8) | reg
    for index, (operation, width) in enumerate(VARIANTS):
        ops = [ldc(4, 0), memory('Ldg', 6, 4, 5), ldc(20, 16), ldc(22, 24),
               *(mov(r, 0xcccccccc) for r in range(8, 12))]
        if operation == 'load': ops += [memory('Ldg', 8, 6, width)]
        elif operation == 'store': ops += [memory('Stg', 20, 6, width)]
        else: ops += [base('Atom') | (width << 52) | ((operation == 'signed') << 49) | (1 << 48) | (20 << 20) | (7 << 16) | (6 << 8) | 8]
        ops += [ldc(12, 56), memory('Stg', 8, 12, 6)]
        key = f'{0xfeed5000+index:016x}'; code = work / (key + '.bin'); code.write_bytes(shader(ops))
        jobs.append(dict(key=key, stage='cs', code=str(code), local=[1, 1, 1]))
    path = work / 'jobs.json'; path.write_text(json.dumps(jobs)); subprocess.run([translator, path, apple], check=True)
    path.write_text(json.dumps([dict(job, host_target='vulkan') for job in jobs])); subprocess.run([translator, path, work], check=True)
    for job in jobs:
        key = job['key']; binary = work / (key + '.spv')
        if binary.read_bytes() != (apple / binary.name).read_bytes():
            raise ValueError('raw-address translation differs between Apple and Vulkan targets')
        source = subprocess.run([ROOT / 'dm/gfx/tools/spirv_cross_bin/spirv-cross', '--msl', '--msl-version', '20300',
                                 '--msl-argument-buffers', apple / binary.name], capture_output=True, text=True, check=True).stdout
        (work / (key + '.metal')).write_text(source)
        resources = json.loads((work / (key + '.res.json')).read_text())
        if any('not implemented' in note or 'Invalid' in note for note in resources['notes']):
            raise ValueError(f'unsupported fixture operation: {resources["notes"]}')
        reflection = dict(stage='cs', bindings=NS['reflect'](key, 'cs', source, resources))
        (work / (key + '.reflection.json')).write_text(json.dumps(reflection, indent=2))
        vk.write_stage(key, binary.read_bytes(), reflection, export, shutil.which('spirv-val'), shutil.which('spirv-opt'))
    return export


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__); parser.add_argument('directory', type=Path)
    print(generate(parser.parse_args().directory.resolve()))
