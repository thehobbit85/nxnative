"""Runtime Vulkan descriptor pools with the shared Apple pool wrapper's bounds."""
import struct

from dm.android.tools.spv_capture import Module, instruction
from dm.android.tools.spv_module import opcode, string_operand

COUNTS = {'tex_pool': 1 << 20, 'samp_pool': 1 << 12}


def pointer_operands(x):
    # Operand positions that can carry a whole descriptor-array pointer.
    # Do not scan literals: e.g. a composite index or a line number may happen
    # to equal the variable's ID without referring to that variable at all.
    op = opcode(x)
    if op in (61, 83, 124): return x[3:4]
    if op in (62, 63, 64, 401, 402, 403): return x[1:3] if op < 400 else x[3:5]
    if op in (65, 66, 67, 70): return x[3:4]
    if op == 57: return x[4:]
    if op == 59: return x[4:5]
    if op == 169: return x[4:6]
    if op == 245: return x[3::2]
    if op == 254: return x[1:2]
    if op == 12: return x[5:]
    return ()


def fields(binding):
    kind = binding['kind']
    if kind == 'pool_bounds':
        count = binding.get('reg')
        if binding['type'] != 'ubo' or binding['count'] != 1 or type(count) is not int or not 1 <= count <= 63:
            raise ValueError('invalid descriptor pool bounds block')
    if kind not in COUNTS: return ()
    slot = binding.get('pool_slot')
    if (binding['count'] != COUNTS[kind] or binding.get('runtime') is not True or
            type(slot) is not int or not 0 <= slot < 63 or
            binding['type'] != ('tex' if kind == 'tex_pool' else 'samp')):
        raise ValueError('invalid runtime descriptor pool')
    return ('runtime=1', f'pool_slot={slot}')


def lower(data, reflection):
    resources = {b['msl_name']: b for b in reflection['bindings'] if b['kind'] in COUNTS}
    if not resources: return data, reflection
    m = Module(data)
    names = {x[1]: string_operand(x[2:]) for x in m.code if opcode(x) == 5}
    pools = {}; descriptors = []
    for x in m.code:
        if opcode(x) != 59 or 33 not in m.decorations[x[2]]: continue
        if m.decorations[x[2]].get(34) != (0,): raise ValueError('pool shader requires set-zero descriptors')
        descriptors.append(x[2])
        name = names.get(x[2]); resource = resources.get(name)
        if resource is None: continue
        pointer = m.types[x[1]]; array = m.types[pointer[3]]
        if (x[3] != 0 or opcode(array) != 28 or
                m.constants.get(array[3]) != COUNTS[resource['kind']]):
            raise ValueError('unexpected shared descriptor pool array')
        slot = len(pools); runtime = m.ty(29, array[2]); pointer = m.ty(32, 0, runtime)
        pools[x[2]] = dict(slot=slot, name=name)
        m.declarations.append(instruction(59, pointer, x[2], 0))
    if len(pools) != len(resources) or not pools or len(descriptors) >= 64:
        raise ValueError('incomplete or oversized descriptor pool interface')
    # SPIRV-Cross numbers a binding after a whole million-element array. The
    # sidecar follows these compact bindings; guest sources remain unchanged.
    descriptors.sort(key=lambda var: m.decorations[var][33])
    compact = {var: index for index, var in enumerate(descriptors)}
    array = m.aggregate(28, m.uint, m.const(len(pools))); m.decorate(array, 6, 16)
    block = m.aggregate(30, array); m.decorate(block, 2)
    m.annotations.append(instruction(72, block, 0, 35, 0))
    bounds = m.variable(block, 2, 'dm_pool_bounds'); m.decorate(bounds, 34, 0); m.decorate(bounds, 33, len(descriptors))
    word_pointer = m.ty(32, 2, m.uint); zero = m.const(0)
    rewritten = []; nonuniform = set(); seen = set()
    for x in m.code:
        op = opcode(x)
        if op == 59 and x[2] in pools: continue
        if op == 71 and x[1] in compact and x[2] == 33:
            x = instruction(71, x[1], 33, compact[x[1]])
        if op in (65, 66) and x[3] in pools:
            if len(x) != 5: raise ValueError('nested descriptor pool index')
            seen.add(x[3]); slot = pools[x[3]]['slot']
            index = m.value(rewritten, 124, m.uint, x[4])
            pointer = m.value(rewritten, 65, word_pointer, bounds, zero, m.const(slot))
            limit = m.value(rewritten, 61, m.uint, pointer)
            smaller = m.value(rewritten, 176, m.boolean, index, limit)
            index = m.value(rewritten, 169, m.uint, smaller, index, limit)
            x = instruction(op, x[1], x[2], x[3], index); nonuniform.update((index, x[2]))
        elif any(var in pools for var in pointer_operands(x)):
            # The shared pool_arrays wrapper likewise accepts indexed uses
            # only. Refuse escaped whole-array values, not arbitrary layouts.
            raise ValueError('unsupported whole descriptor pool use')
        if ((op == 61 and x[3] in nonuniform) or
                (op in (83, 100) and x[3] in nonuniform) or
                (op == 86 and any(value in nonuniform for value in x[3:]))):
            nonuniform.add(x[2])
        rewritten.append(x)
    if seen != set(pools): raise ValueError('unused descriptor pool cannot be bounded')
    for value in sorted(nonuniform):
        if 5300 not in m.decorations[value]: m.decorate(value, 5300)
    caps = {x[1] for x in rewritten if opcode(x) == 17}
    extensions = {string_operand(x[1:]) for x in rewritten if opcode(x) == 10}
    prefix = [x for x in rewritten if opcode(x) == 17]
    prefix += [instruction(17, cap) for cap in (5301, 5302, 5307) if cap not in caps]
    prefix += [x for x in rewritten if opcode(x) == 10]
    if 'SPV_EXT_descriptor_indexing' not in extensions:
        name = b'SPV_EXT_descriptor_indexing\0'; name += b'\0' * (-len(name) % 4)
        prefix.append(instruction(10, *struct.unpack(f'<{len(name)//4}I', name)))
    rewritten = prefix + [x for x in rewritten if opcode(x) not in (17, 10)]
    annotation_at = next((j for j, x in enumerate(rewritten) if opcode(x) in (71, 72, 73, 74, 75)),
                         next(j for j, x in enumerate(rewritten) if 19 <= opcode(x) <= 39))
    type_at = next(j for j, x in enumerate(rewritten) if 19 <= opcode(x) <= 39)
    function_at = next(j for j, x in enumerate(rewritten) if opcode(x) == 54)
    words = list(m.header); words[3] = m.next_id
    for j, x in enumerate(rewritten):
        if j == annotation_at:
            for value in m.names: words.extend(value)
        if j == type_at:
            for value in m.annotations: words.extend(value)
        if j == function_at:
            for value in m.declarations: words.extend(value)
        words.extend(x)
    slots = {pool['name']: pool['slot'] for pool in pools.values()}
    bindings = [dict(b, runtime=True, pool_count=COUNTS[b['kind']], pool_slot=slots[b['msl_name']])
                if b['msl_name'] in slots else b for b in reflection['bindings']]
    bindings.append(dict(msl_name='dm_pool_bounds', kind='pool_bounds', reg=len(pools)))
    return struct.pack(f'<{len(words)}I', *words), dict(reflection, bindings=bindings)
