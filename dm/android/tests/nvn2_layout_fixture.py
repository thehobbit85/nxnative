"""Actual Ampere loads from every region of a nonzero-push NVN2 C0 layout."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path[:0] = [str(ROOT), str(ROOT / 'dm/tools/tests'), str(ROOT / 'dm/gfx/tests')]
from test_dmsh_control_flow import bits, assemble, EXIT
from test_dmsh_pool_arrays import NS
from dm.android.tools import spv_android as vk

KEY = '00000000feed9100'
OFFSETS = (*range(0, 0xdc, 4), 0x200)


def load(dst, bank, offset, width=4):
    # Write-barrier seven means no deferred load, matching these serial stores.
    return bits((0, 12, 0xb82), (12, 3, 7), (16, 8, dst), (24, 8, 255),
                (38, 16, offset), (54, 5, bank), (73, 3, width), (110, 3, 7))


def store(src, offset):
    return bits((0, 12, 0x986), (12, 3, 7), (24, 8, 4), (32, 8, src),
                (40, 24, offset), (72, 1, 1), (73, 3, 4), (110, 3, 7))


def generate(work, wrong_push=False):
    work.mkdir(parents=True, exist_ok=True)
    operations = [load(4, 3, 56, 5)]
    for index, offset in enumerate(OFFSETS):
        operations += [load(16, 0, offset), store(16, 4 * index)]
    binary = work / 'layout.bin'; binary.write_bytes(assemble(operations + [EXIT]))
    # push=19 rounds to32. The negative changes it to16, shifting every region.
    job = dict(key=KEY, stage='cs', arch='sm86', code=str(binary), local=[1, 1, 1],
               nvn2_cs=[2, 3, 2, 0xd8, 16 if wrong_push else 19, 3, 4])
    translator = os.environ.get('DMSH_TEST_XLATE', str(ROOT / 'dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate'))
    for target in ('apple', 'vulkan'):
        out = work / target; out.mkdir(exist_ok=True); path = out / 'jobs.json'
        path.write_text(json.dumps([job if target == 'apple' else dict(job, host_target='vulkan')]))
        subprocess.run([translator, path, out], check=True, capture_output=True)
    raw = (work / 'apple' / (KEY + '.spv')).read_bytes()
    if raw != (work / 'vulkan' / (KEY + '.spv')).read_bytes():
        raise ValueError('NVN2 layout translation differs by target')
    source = subprocess.run([ROOT / 'dm/gfx/tools/spirv_cross_bin/spirv-cross', '--msl', '--msl-version',
                             '20300', '--msl-argument-buffers', work / 'apple' / (KEY + '.spv')],
                            check=True, capture_output=True, text=True).stdout
    (work / (KEY + '.metal')).write_text(source)
    res = json.loads((work / 'apple' / (KEY + '.res.json')).read_text())
    if res['notes']: raise ValueError(res['notes'])
    reflection = dict(stage='cs', threadgroup_size=[1, 1, 1], bindings=NS['reflect'](KEY, 'cs', source, res))
    (work / (KEY + '.reflection.json')).write_text(json.dumps(reflection, indent=2))
    out = work / 'export'; out.mkdir(exist_ok=True)
    vk.write_stage(KEY, raw, reflection, out, shutil.which('spirv-val'), shutil.which('spirv-opt'))
    return out


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__); parser.add_argument('directory', type=Path)
    parser.add_argument('--wrong-push', action='store_true'); args = parser.parse_args()
    print(generate(args.directory.resolve(), args.wrong_push))
