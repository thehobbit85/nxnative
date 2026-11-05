"""Build the shared PrimitiveID geometry corpus for Vulkan and optional Metal."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path[:0] = [str(ROOT), str(ROOT / 'dm/tools/tests'), str(ROOT / 'dm/gfx/tests')]
from test_txq_type import TABLE
from test_dmsh_reflect import REFLECT
from dm.android.tools import spv_android as vk
from dm.gfx.tools import dmsh_geometry


def primitive_id_program(invocations):
    # Same Maxwell operations as ComputeSplitPrimitiveId's Apple regression:
    # viewport mask = PrimitiveID + 1; all captured vertex attributes pass through.
    def instruction(name, emitter=''):
        pattern = r'Add\("([01x]{64})",\s*InstName\.' + name + r'\s*,'
        if emitter: pattern += r'\s*InstEmit\.' + emitter + r'\s*,'
        match = re.search(pattern, TABLE)
        if not match: raise ValueError('missing Maxwell instruction ' + name)
        return int(match[1].replace('x', '0'), 2) | (7 << 16)
    ops = [instruction('Ald') | (255 << 8) | (255 << 39) | (0x60 << 20),
           instruction('Iadd', 'IaddI') | (1 << 20),
           instruction('Ast') | (255 << 8) | (0x3a0 << 20), instruction('Exit') | 15]
    header = bytearray(0x50)
    struct.pack_into('<5I', header, 0, (1 << 24) | (4 << 10) | (3 << 5) | 1, 0, invocations << 24, 7 << 24, 1)
    return bytes(header) + struct.pack('<8Q', 0, *ops[:3], 0, ops[3], 0, 0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--metal', action='store_true')
    args = parser.parse_args(); work = args.directory.resolve(); work.mkdir(parents=True, exist_ok=True)
    raw = work / 'raw'; raw.mkdir(exist_ok=True); out = work / 'export'; out.mkdir(exist_ok=True)
    jobs = []
    for inv in (1, 3):
        key = f'{0xfeed0000 + inv:016x}'; code = work / (key + '.bin'); code.write_bytes(primitive_id_program(inv))
        jobs.append(dict(key=key, stage='gs', code=str(code), tex={}, img={}, attr={}, fout={}, debug=True, reduced=True,
                         tf=[dict(stride=16, outputs=[[0, 4]]), [], [], []]))
    (work / 'jobs.json').write_text(json.dumps(jobs)); os.environ['DMSH_SPV_CAPTURE'] = str(raw)
    vk.capture([str(work / 'jobs.json'), str(work)])
    maps = {}
    for job in jobs:
        for name in vk.spv_geometry.product_family(job['key']):
            control = json.loads((raw / (name + '.control.json')).read_text())
            reflection = vk.spv_geometry.helper_reflection(dict(stage='gs', bindings=[]), control)
            vk.write_stage(name, (raw / (name + '.spv')).read_bytes(), reflection, out,
                           shutil.which('spirv-val'), shutil.which('spirv-opt'))
            if '.gc' not in name: continue
            resources = vk.vk_reflect((out / (name + '.spv')).read_bytes(), reflection)
            bindings = {b['reg']: b['binding'] for b in resources}
            if set(bindings) != set(range(5)): raise ValueError('unexpected Vulkan geometry fixture bindings')
            if 'vk' in maps and maps['vk'] != bindings: raise ValueError('inconsistent Vulkan fixture bindings')
            maps['vk'] = bindings
            if not args.metal: continue
            cross = ROOT / 'dm/gfx/tools/spirv_cross_bin/spirv-cross'
            result = subprocess.run([cross, '--msl', '--msl-version', '20300', '--msl-argument-buffers',
                                     '--msl-texture-buffer-native', raw / (name + '.spv')], check=True, capture_output=True, text=True)
            source = dmsh_geometry.capture_input(dmsh_geometry.topology_pointer(result.stdout))
            resources = REFLECT(name, 'gs', source, json.loads((work / (name + '.res.json')).read_text()))
            bindings = {b['reg']: b['arg_id'] for b in resources}
            if set(bindings) != set(range(5)): raise ValueError('unexpected Metal geometry fixture bindings')
            if 'metal' in maps and maps['metal'] != bindings: raise ValueError('inconsistent Metal fixture bindings')
            maps['metal'] = bindings
            path = work / (name + '.metal'); path.write_text(source)
            subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-c', '-w', '-fmodules-cache-path=' + str(work / 'modules'),
                            path, '-o', path.with_suffix('.air')], check=True)
            subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', path.with_suffix('.air'), '-o', path.with_suffix('.metallib')], check=True)
    (work / 'geometry_bindings.h').write_text(''.join(
        'static const unsigned gc_' + api + '_bindings[] = {' + ','.join(str(bindings[i]) for i in range(5)) + '};\n'
        for api, bindings in maps.items()))
    print('Validated 32 geometry modules; emitted compute bindings for ' + ', '.join(maps))


if __name__ == '__main__': main()
