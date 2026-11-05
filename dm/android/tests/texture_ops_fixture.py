"""Generate actual Maxwell 2D sample/fetch/gradient/gather/query operations."""
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
from test_txq_type import base, handle, mov, shader, SLOT_128, SAMPLER_1
from test_dmsh_pool_arrays import NS

VARIANTS = ([dict(op=op, lod=lod, offset=offset) for op in ('sample', 'fetch')
             for lod in (0, 1) for offset in (0, 1)] +
            [dict(op='gradient', offset=offset) for offset in (0, 1)] +
            [dict(op='gather', component=component, offset=offset)
             for component in range(4) for offset in range(3)] + [dict(op='query'), dict(op='query', full=True)])


def program(case):
    def load(reg, offset, width=4):
        return base('Ldc') | (width << 48) | (3 << 36) | (offset << 20) | (7 << 16) | (255 << 8) | reg
    op = case['op']
    if op == 'gradient':
        ops = handle(0, SLOT_128, SAMPLER_1) + [load(1, 0), load(2, 4), load(3, 8)]
        ops += [load(4 + i, 16 + 4 * i) for i in range(4)]
        instruction = base('TxdB') | (case['offset'] << 35) | (4 << 20)
    elif op == 'query':
        ops = handle(6, SLOT_128, SAMPLER_1) + [load(7, 8), mov(19, 0x76543210)]
        instruction = base('TxqB') | (1 << 22) | (6 << 8)
    else:
        ops = handle(6, SLOT_128, SAMPLER_1) + [load(0, 0), load(1, 4), load(7, 8), load(8, 12)]
        if op == 'sample': instruction = base('TexB') | ((3 if case['lod'] else 1) << 37) | (case['offset'] << 36)
        elif op == 'fetch': instruction = base('TldB') | (case['lod'] << 55) | (case['offset'] << 35)
        else: instruction = base('Tld4B') | (case['component'] << 38) | (case['offset'] << 36)
        instruction |= 6 << 20
    # Keep both packed width/height/mip outputs with a guarded register and the
    # full query, whose absent 2D depth must be zero after type resolution.
    mask = 11 if op == 'query' and not case.get('full') else 15
    instruction |= (mask << 31) | (7 << 16) | 16
    if op != 'query': instruction |= 2 << 28
    ops += [instruction, load(4, 56, width=5), base('Stg') | (6 << 48) | (1 << 45) | (7 << 16) | (4 << 8) | 16]
    return shader(ops)


def generate(work):
    work.mkdir(parents=True, exist_ok=True); export = work / 'export'; export.mkdir(exist_ok=True)
    apple = work / 'apple'; apple.mkdir(exist_ok=True)
    translator = os.environ.get('DMSH_TEST_XLATE', str(ROOT / 'dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate'))
    jobs = []
    for index, case in enumerate(VARIANTS):
        key = f'{0xfeed7000 + index:016x}'; code = work / (key + '.bin'); code.write_bytes(program(case))
        jobs.append(dict(key=key, stage='cs', code=str(code), tex={'128': '2d'}, local=[1, 1, 1]))
    path = work / 'jobs.json'; path.write_text(json.dumps(jobs)); subprocess.run([translator, path, apple], check=True)
    path.write_text(json.dumps([dict(job, host_target='vulkan') for job in jobs])); subprocess.run([translator, path, work], check=True)
    for job, case in zip(jobs, VARIANTS):
        key = job['key']; binary = work / (key + '.spv')
        source = subprocess.run([ROOT / 'dm/gfx/tools/spirv_cross_bin/spirv-cross', '--msl', '--msl-version', '20300',
                                 '--msl-argument-buffers', apple / binary.name], capture_output=True, text=True, check=True).stdout
        (work / (key + '.metal')).write_text(source)
        resources = json.loads((work / (key + '.res.json')).read_text())
        if resources['notes']: raise ValueError(f'unexpected translator notes for {case}: {resources["notes"]}')
        reflection = dict(stage='cs', bindings=NS['reflect'](key, 'cs', source, resources))
        (work / (key + '.reflection.json')).write_text(json.dumps(reflection, indent=2))
        vk.write_stage(key, binary.read_bytes(), reflection, export, shutil.which('spirv-val'), shutil.which('spirv-opt'))
    (work / 'cases.json').write_text(json.dumps(VARIANTS, indent=2))
    return export


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__); parser.add_argument('directory', type=Path)
    args = parser.parse_args(); print(generate(args.directory.resolve()))
