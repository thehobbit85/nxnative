"""Execute emitted lane arithmetic against the Vulkan native-subgroup primitive contract.

The physical model supplies the local index as the hardware lane, one distinct
value per lane for Shuffle, and the four known words of a physical ballot.
This exercises the exporter's actual index selection and predicate operations
for 32, 64 and 128 lanes on an emulator whose hardware subgroup has four.
"""
from itertools import product
import os
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
from dm.android.tools import spv_warp_native
from dm.android.tools.spv_capture import Module, instruction
from dm.android.tools.spv_module import opcode


def predicate(i):
    return i % 2 == 0 if i // 32 == 2 else i // 32 % 2 != 0


def physical_inputs(raw, active_lanes):
    module = Module(raw)
    builtins = {i[3]: i[1] for i in module.code if opcode(i) == 71 and i[2] == 11}
    true_ids = {i[2] for i in module.code if opcode(i) == 41}
    active = [sum(1 << (i % 32) for i in active_lanes if i // 32 == warp) for warp in range(4)]
    votes = [sum(1 << (i % 32) for i in active_lanes if i // 32 == warp and predicate(i)) for warp in range(4)]
    # The guest path reads component zero of Eq/Le; the physical register
    # spans the entire wave. Model the lower word even for lanes above 31.
    masks = {i[2]: kind for i in module.code if opcode(i) == 65 and len(i) == 5
             for kind in (4416, 4419) if i[3] == builtins.get(kind) and module.constants.get(i[4]) == 0}
    out = []
    for i in module.code:
        op = opcode(i)
        if op == 61 and i[3] == builtins[41]:
            out.append(instruction(61, i[1], i[2], builtins[29]))
        elif op == 61 and i[3] in masks:
            lane = module.value(out, 61, module.uint, builtins[29])
            within = module.value(out, 176, module.boolean, lane, module.const(32))
            bitpos = module.value(out, 199, module.uint, lane, module.const(31))
            eq = module.value(out, 196, module.uint, module.const(1), bitpos)
            low, upper = eq, module.const(0)
            if masks[i[3]] == 4419:
                low = module.value(out, 197, module.uint, eq, module.value(out, 130, module.uint, eq, module.const(1)))
                upper = module.const(0xffffffff)
            out.append(instruction(169, module.uint, i[2], within, low, upper))
        elif op == 345:
            # The probed shader shuffles gl_LocalInvocationIndex. The register
            # at a selected source therefore equals that source's physical ID.
            out.append(instruction(83, i[1], i[2], i[5]))
        elif op == 339:
            words = active if i[4] in true_ids else votes
            out.append(instruction(80, i[1], i[2], *(module.const(w) for w in words)))
        elif op in (334, 335, 336):
            # This branch runs only in the unmodified negative control.
            global_vote = {334: votes == active, 335: any(votes), 336: votes == active or not any(votes)}[op]
            out.append(instruction(170, i[1], i[2], module.const(0), module.const(0 if global_vote else 1)))
        else:
            out.append(i)
    at = next(n for n, i in enumerate(out) if opcode(i) == 54)
    out[at:at] = module.declarations
    words = [*module.header[:3], module.next_id, module.header[4]]
    for i in out:
        words.extend(i)
    return struct.pack(f'<{len(words)}I', *words)


def generate(directory, negative=False):
    out = directory / 'export'
    out.mkdir(parents=True, exist_ok=True)
    shader = Path(__file__).with_name('native_warp.comp')
    cases = []
    for count, sparse in product((32, 64, 116, 128), (False, True)):
        raw = subprocess.check_output([os.environ.get('GLSLC', 'glslc'), '-fshader-stage=comp', '--target-env=vulkan1.1',
                                       f'-DLANES={count}', f'-DSPARSE={int(sparse)}', shader, '-o', '-'])
        raw = raw if negative else spv_warp_native.lower(raw)
        active_lanes = {i for i in range(count) if not sparse or i % 32 < 4 or (i % 32 // 4 + i // 32) % 3 != 1}
        data = physical_inputs(raw, active_lanes)
        subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', '-'], input=data, check=True)
        key = f'native-{count}' + ('-sparse' if sparse else '')
        (out / (key + '.spv')).write_bytes(data)
        expected = []
        for tid in range(count):
            if tid not in active_lanes:
                expected.extend([3135106223] * 10)
                continue
            lane = tid % 32
            first = tid - lane
            peers = [i for i in range(first, first + 32) if i in active_lanes]
            bits = [predicate(i) for i in peers]
            mask = sum(1 << (i % 32) for i in peers if predicate(i))
            expected.extend((lane, first, first + (lane ^ 3), mask, 0, int(all(bits)), int(any(bits)),
                             int(len(set(bits)) == 1), 1 << lane, (2 << lane) - 1))
        script = [f'#!amber\nSHADER compute guest SPIRV-BIN FILE "{key}.spv"',
                  f'BUFFER out DATA_TYPE uint32 SIZE {len(expected)} FILL 3135106223',
                  'PIPELINE compute check', '  ATTACH guest', '  BIND BUFFER out AS storage DESCRIPTOR_SET 0 BINDING 0',
                  'END', 'RUN check 1 1 1', 'EXPECT out IDX 0 EQ ' + ' '.join(map(str, expected))]
        (out / (key + '.amber')).write_text('\n'.join(script) + '\n')
        cases.append(key + '.amber')
    (out / 'cases.list').write_text('\n'.join(cases) + '\n')


if __name__ == '__main__':
    generate(Path(sys.argv[1]).resolve(), '--negative' in sys.argv[2:])
