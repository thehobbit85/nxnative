"""Expose the shared Apple stage-memory prefix for differential GPU tests.

The full object/mesh function is compiled separately, unchanged. This adapter
replaces only its host inputs and final Metal output handoff with a raw dump;
initialization, payload filling, the translated body and barriers stay intact.
It does not exercise native mesh rasterization or Android renderer dispatch.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'dm/gfx/tools'))
import dmsh_meshstage as shared


def sources(msl, info):
    info = dict(info)
    for field in ('vertex', 'primitive'):
        if field in info: info[field] = [tuple(value) for value in info[field]]
    native = shared.to_stage(msl, info)
    entry = re.search(r'\[\[(?:object|mesh)\]\] void main0\(((?:[^()]|\([^()]*\))*)\)\n\{\n', native)
    if not entry: raise ValueError('shared Apple stage entry changed')
    body_end = re.search(r'    dm_stage_body\([^\n]*\);\n    threadgroup_barrier\(mem_flags::mem_threadgroup\);\n', native[entry.end():])
    if not body_end: raise ValueError('shared Apple stage body/barrier changed')
    prefix = native[entry.end():entry.end() + body_end.end()]
    memory = re.search(r'threadgroup spvUnsafeArray<uint, (\d+)> (\w+);', prefix)
    if not memory: raise ValueError('shared Apple stage memory declaration changed')
    words, name = int(memory[1]), memory[2]
    parameters, group, local, local_type = [], None, None, None
    for param in shared._params(entry[1]):
        thread = shared._THREAD.match(param)
        if thread:
            if thread[3] == 'threadgroup_position_in_grid': group = thread
            else:
                local_type = thread[1]
                local = thread[2] + ('.x' if thread[1] == 'uint3' else '')
                parameters.append(param)
        elif ('[[payload]]' in param or param in ('mesh_grid_properties dm_grid', 'dm_mesh_t dm_out')):
            continue
        elif param.startswith('constant uint* dm_first '): parameters.append(param)
        else: raise ValueError(f'unexpected fixture parameter: {param}')
    if group is None or local is None: raise ValueError('shared Apple stage has no thread/group inputs')
    # Compute builtins must all use scalars or matching-width vectors; the
    # shared wrapper supplies a scalar local ID when the raw body needs none.
    physical = 'dm_physical_group' + ('.x' if local_type == 'uint3' else '')
    parameters += [f'{local_type} dm_physical_group [[threadgroup_position_in_grid]]',
                   'device const uint* dm_input [[buffer(1)]]', 'device uint* dm_result [[buffer(2)]]']
    task = info['kind'] == 'ms' and info['task']
    number = f'dm_input[5u + {physical} * 11u]' if task else physical
    group_value = f'uint3({number}, 0u, 0u)' if group[1] == 'uint3' else number
    setup = f'    {group[1]} {group[2]} = {group_value};\n'
    if task:
        setup += ('    dm_payload_t dm_payload;\n'
                  f'    for (uint i = 0; i < {(info["payload"] + 3) // 4}u; i++) '
                  f'dm_payload.w[i] = dm_input[5u + {physical} * 11u + 1u + i];\n')
    dump = (f'    for (uint i = {local}; i < {words}u; i += {info["threads"]}u)\n'
            f'        dm_result[13u + {physical} * {words}u + i] = {name}[i];\n')
    adapter = native[:entry.start()] + 'kernel void main0(' + ', '.join(parameters) + ')\n{\n' + setup + prefix + dump + '}\n'
    return native, adapter
