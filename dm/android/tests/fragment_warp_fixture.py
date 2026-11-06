"""Draw guest quad-local SHFL on full and partially covered fragment quads."""
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
from test_txq_type import base, shader

SIDE = 8
ISOLATED = {(0, 0), (3, 0), (0, 3), (3, 3), (6, 4), (5, 7)}


def program(mode, selector):
    header = bytearray(80)
    struct.pack_into('<I', header, 0, (5 << 10) | (3 << 5) | 1)
    struct.pack_into('<I', header, 72, 15)
    def coord(reg, offset):
        return base('Ipa') | (offset << 28) | (7 << 16) | reg
    ops = [coord(4, 0x70), coord(5, 0x74)]
    for dst, src in ((0, 4), (1, 5)):
        ops.append(base('Shfl') | (7 << 48) | (0x1c03 << 34) | (mode << 30)
                   | (3 << 28) | (selector << 20) | (7 << 16) | (src << 8) | dst)
    return bytes(header) + shader(ops + [coord(2, 0x70), coord(3, 0x74)])


def draw_data(mode, selector, sparse):
    vertices = [-1, -1, 3, -1, -1, 3]
    if sparse:
        vertices = []
        for x, y in sorted(ISOLATED):
            for u, v in ((x, y), (x + 1.875, y), (x, y + 1.875)):
                vertices.extend([2 * u / SIDE - 1, 2 * v / SIDE - 1])
    values = []
    for y in range(SIDE):
        for x in range(SIDE):
            lane = (x & 1) | ((y & 1) << 1)
            source = selector if mode == 0 else lane ^ selector
            pixel = [(x & ~1) + (source & 1) + 0.5, (y & ~1) + (source >> 1) + 0.5, x + 0.5, y + 0.5]
            values.extend(pixel if not sparse or (x, y) in ISOLATED else [0.0] * 4)
    return vertices, values


def generate(work):
    out = work / 'export'
    out.mkdir(parents=True, exist_ok=True)
    vs = '#version 450\nlayout(location=0) in vec2 p; void main(){gl_Position=vec4(p,0,1);}'
    subprocess.run([os.environ.get('GLSLC', 'glslc'), '-fshader-stage=vert', '--target-env=vulkan1.1',
                    '-', '-o', out / 'screen.spv'], input=vs.encode(), check=True)
    modes = [(0, n) for n in range(4)] + [(3, n) for n in (1, 2, 3)]
    jobs = []
    for i, (mode, selector) in enumerate(modes):
        key = f'{0xfeeda100 + i:016x}'
        binary = work / (key + '.bin')
        binary.write_bytes(program(mode, selector))
        jobs.append(dict(key=key, stage='ps', code=str(binary), fout={'0': 'float'}))
    translator = os.environ.get('DMSH_TEST_XLATE', ROOT / 'dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate')
    for target in ('apple', 'vulkan'):
        directory = work / target
        directory.mkdir(exist_ok=True)
        manifest = directory / 'jobs.json'
        manifest.write_text(json.dumps(jobs if target == 'apple' else [dict(j, host_target='vulkan') for j in jobs]))
        subprocess.run([translator, manifest, directory], check=True, capture_output=True)

    cases = []
    for job, (mode, selector) in zip(jobs, modes):
        key = job['key']
        apple = work / 'apple' / (key + '.spv')
        msl = subprocess.check_output([ROOT / 'dm/gfx/tools/spirv_cross_bin/spirv-cross', '--msl', '--msl-version',
                                       '20300', '--msl-argument-buffers', apple], text=True)
        (work / (key + '.metal')).write_text(msl)
        resources = json.loads(apple.with_suffix('.res.json').read_text())
        if resources['notes']:
            raise ValueError(resources['notes'])
        bindings = NS['reflect'](key, 'ps', msl, resources)
        if len(bindings) != 1 or bindings[0]['kind'] != 'support':
            raise ValueError(bindings)
        spv_android.write_stage(key, (work / 'vulkan' / apple.name).read_bytes(), dict(stage='ps', bindings=bindings),
                                out, shutil.which('spirv-val'), shutil.which('spirv-opt'))
        records = [line.split() for line in (out / (key + '.vk.txt')).read_text().splitlines() if line.startswith('b ')]
        if len(records) != 1 or records[0][2:5] != ['ubo', '1', 'support']:
            raise ValueError(records)
        for sparse in (False, True):
            vertices, values = draw_data(mode, selector, sparse)
            name = key + ('-edges' if sparse else '-full')
            for suffix, data in (('.vertices', vertices), ('.expected', values)):
                (work / (name + suffix)).write_bytes(struct.pack(f'<{len(data)}f', *data))
            cases.append(dict(key=key, name=name, metal_slot=bindings[0]['arg_id'], side=SIDE))
            script = [f'#!amber\nSHADER vertex screen SPIRV-BIN FILE "screen.spv"\nSHADER fragment guest SPIRV-BIN FILE "{key}.spv"',
                      'BUFFER vertices DATA_TYPE vec2<float> DATA ' + ' '.join(map(str, vertices)) + ' END',
                      'BUFFER support DATA_TYPE float SIZE 1024 FILL 1.0', 'BUFFER frame FORMAT R32G32B32A32_SFLOAT',
                      'PIPELINE graphics check', '  ATTACH screen', '  ATTACH guest', '  VERTEX_DATA vertices LOCATION 0',
                      f'  FRAMEBUFFER_SIZE {SIDE} {SIDE}', f'  BIND BUFFER support AS uniform DESCRIPTOR_SET 0 BINDING {records[0][1]}',
                      '  BIND BUFFER frame AS color LOCATION 0', 'END', 'CLEAR check',
                      f'RUN check DRAW_ARRAY AS TRIANGLE_LIST START_IDX 0 COUNT {len(vertices) // 2}',
                      'EXPECT frame IDX 0 EQ ' + ' '.join(map(str, values))]
            (out / (name + '.amber')).write_text('\n'.join(script) + '\n')
    (out / 'cases.list').write_text(''.join(c['name'] + '.amber\n' for c in cases))
    (work / 'metal.list').write_text(''.join(j['key'] + '\n' for j in jobs))
    (work / 'cases.json').write_text(json.dumps(cases, indent=2) + '\n')


if __name__ == '__main__':
    generate(Path(sys.argv[1]).resolve())
