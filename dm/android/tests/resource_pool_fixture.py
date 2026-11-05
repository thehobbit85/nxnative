"""Translate an actual Maxwell shader with per-lane handles loaded from storage."""
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


def generate(work, dimension=2):
    work.mkdir(parents=True, exist_ok=True); export = work / 'export'; export.mkdir(exist_ok=True)
    apple = work / 'apple'; apple.mkdir(exist_ok=True)
    def ldc(reg, offset, width=4):
        return base('Ldc') | (width << 48) | (3 << 36) | (offset << 20) | (7 << 16) | (255 << 8) | reg
    def add(reg, other):
        return base('Iadd') | (other << 20) | (7 << 16) | (reg << 8) | reg
    ops = [ldc(4, 0, 5), base('S2r') | (33 << 20) | (7 << 16) | 14, mov(15, 2),
           base('Shl') | (15 << 20) | (7 << 16) | (14 << 8) | 14, add(4, 14),
           base('Ldg') | (4 << 48) | (1 << 45) | (7 << 16) | (4 << 8) | 6,
           ldc(0, 16), mov(1, 0x3f000000), mov(3, 0), base('TexB') | (1 << 37) | (1 << 31) | (dimension << 28) | (6 << 20) | (7 << 16) | 8,
           ldc(12, 56, 5), add(12, 14), base('Stg') | (4 << 48) | (1 << 45) | (7 << 16) | (12 << 8) | 8]
    key = '00000000feed6000'; path = work / (key + '.bin'); path.write_bytes(shader(ops))
    job = dict(key=key, stage='cs', code=str(path), local=[8, 1, 1]); jobs = work / 'jobs.json'
    translator = os.environ.get('DMSH_TEST_XLATE', str(ROOT / 'dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate'))
    jobs.write_text(json.dumps([job])); subprocess.run([translator, jobs, apple], check=True)
    jobs.write_text(json.dumps([dict(job, host_target='vulkan')])); subprocess.run([translator, jobs, work], check=True)
    binary = work / (key + '.spv')
    if binary.read_bytes() != (apple / binary.name).read_bytes(): raise ValueError('pool raw translation differs by target')
    source = subprocess.run([ROOT / 'dm/gfx/tools/spirv_cross_bin/spirv-cross', '--msl', '--msl-version', '20300',
                             '--msl-argument-buffers', apple / binary.name], capture_output=True, text=True, check=True).stdout
    source = NS['pool_arrays'](source); (work / (key + '.metal')).write_text(source)
    resources = json.loads((work / (key + '.res.json')).read_text())
    if any('not implemented' in note or 'Invalid' in note for note in resources['notes']): raise ValueError(resources['notes'])
    reflection = dict(stage='cs', threadgroup_size=[8, 1, 1], bindings=NS['reflect'](key, 'cs', source, resources))
    (work / (key + '.reflection.json')).write_text(json.dumps(reflection, indent=2))
    vk.write_stage(key, binary.read_bytes(), reflection, export, shutil.which('spirv-val'), shutil.which('spirv-opt'))
    return export


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__); parser.add_argument('directory', type=Path)
    print(generate(parser.parse_args().directory.resolve()))
