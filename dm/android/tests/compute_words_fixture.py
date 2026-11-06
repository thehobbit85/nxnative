"""Execute synthetic guest compute programs that write one reflected output buffer."""
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

GUARD = 0xbaddecaf


def export_cases(work, programs, *, initial_words=None, metal_transform=None):
    work.mkdir(parents=True, exist_ok=True)
    jobs = []
    cases = []
    for key, local, binary, expected, control in programs:
        path = work / (key + '.bin')
        path.write_bytes(binary)
        jobs.append(dict(key=key, stage='cs', code=str(path), local=local, **control))
        cases.append(dict(key=key, local=local, expected=expected, c1=control.get('c1', '')))
    translator = os.environ.get('DMSH_TEST_XLATE', ROOT / 'dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate')
    for target in ('apple', 'vulkan'):
        directory = work / target
        directory.mkdir(exist_ok=True)
        manifest = directory / 'jobs.json'
        manifest.write_text(json.dumps(jobs if target == 'apple' else [dict(j, host_target='vulkan') for j in jobs]))
        subprocess.run([translator, manifest, directory], check=True, capture_output=True)

    out = work / 'export'
    out.mkdir(exist_ok=True)
    cross = ROOT / 'dm/gfx/tools/spirv_cross_bin/spirv-cross'
    for case in cases:
        key = case['key']
        apple = work / 'apple' / (key + '.spv')
        msl = subprocess.run([cross, '--msl', '--msl-version', '20300', '--msl-argument-buffers', apple],
                             check=True, capture_output=True, text=True).stdout
        if metal_transform is not None:
            msl = metal_transform(msl)
        (work / (key + '.metal')).write_text(msl)
        res = json.loads(apple.with_suffix('.res.json').read_text())
        if res['notes']:
            raise ValueError(res['notes'])
        bindings = NS['reflect'](key, 'cs', msl, res)
        constants = bytes.fromhex(case.pop('c1', ''))
        by_kind = {binding['kind']: binding for binding in bindings}
        if len(by_kind) != len(bindings) or set(by_kind) != ({'sbuf_ubo', 'c1'} if constants else {'sbuf_ubo'}):
            raise ValueError(bindings)
        output = by_kind['sbuf_ubo']
        if output['off'] != 56:
            raise ValueError(output)
        if constants:
            if len(constants) % 4 or len(constants) > 65536:
                raise ValueError('invalid compute fixture constant bank')
            case['c1_slot'] = by_kind['c1']['arg_id']
            (work / (key + '.c1')).write_bytes(constants)
        case['metal_slot'] = output['arg_id']
        reflection = dict(stage='cs', threadgroup_size=case['local'], bindings=bindings)
        spv_android.write_stage(key, (work/'vulkan'/apple.name).read_bytes(), reflection, out,
                                shutil.which('spirv-val'), shutil.which('spirv-opt'))
        records = [line.split() for line in (out/(key+'.vk.txt')).read_text().splitlines() if line.startswith('b ')]
        output_records = [r for r in records if r[2:7] == ['ssbo', '1', 'sbuf_ubo', '0', '56']]
        c1_records = [r for r in records if r[2:5] == ['ubo', '1', 'c1']]
        if len(output_records) != 1 or len(c1_records) != bool(constants) or len(records) != len(bindings):
            raise ValueError(records)
        words = case.pop('expected')
        (work/(key+'.expected')).write_bytes(struct.pack(f'<{len(words)}I', *words))
        buffer = f'BUFFER out DATA_TYPE uint32 SIZE {len(words)} FILL {GUARD}'
        if initial_words is not None:
            initial = initial_words[key]
            if len(initial) != len(words):
                raise ValueError('initial and expected buffer lengths differ')
            case['initial'] = key + '.initial'
            (work / case['initial']).write_bytes(struct.pack(f'<{len(initial)}I', *initial))
            buffer = 'BUFFER out DATA_TYPE uint32 DATA ' + ' '.join(map(str, initial)) + ' END'
        script = [f'#!amber\nSHADER compute guest SPIRV-BIN FILE "{key}.spv"',
                  buffer, 'PIPELINE compute check',
                  '  ATTACH guest', f'  BIND BUFFER out AS storage DESCRIPTOR_SET 0 BINDING {output_records[0][1]}',
                  'END', 'RUN check 1 1 1', 'EXPECT out IDX 0 EQ ' + ' '.join(map(str, words))]
        if constants:
            values = struct.unpack(f'<{len(constants) // 4}I', constants)
            script.insert(1, 'BUFFER bank DATA_TYPE uint32 DATA ' + ' '.join(map(str, values)) + ' END')
            script.insert(script.index('END'), f'  BIND BUFFER bank AS uniform DESCRIPTOR_SET 0 BINDING {c1_records[0][1]}')
        (out/(key+'.amber')).write_text('\n'.join(script) + '\n')
    (work/'cases.json').write_text(json.dumps(cases, indent=2) + '\n')
    (out/'cases.list').write_text(''.join(case['key'] + '.amber\n' for case in cases))
    return out
