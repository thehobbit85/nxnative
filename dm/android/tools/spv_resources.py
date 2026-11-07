"""Resource contracts whose guest source is not described by a scalar slot."""
import re
import struct
import subprocess

from dm.android.tools.spv_capture import Module, instruction
from dm.android.tools.spv_module import opcode, string_operand, pointer_operands

MAX_BINDINGS = 128


def specialize_image_helpers(data, optimizer):
    """Expose each descriptor argument before runtime image-type selection.

    A shared helper can be called with both an ordinary image and an NVN buffer
    presented through a 2D interface. The optimizer's structured inliner gives
    each call its own resource operations while retaining descriptor numbers.
    """
    m = Module(data)
    def descriptor(ty):
        while ty in m.types:
            instruction = m.types[ty]
            op = opcode(instruction)
            if op in (25, 26, 27): return True
            if op not in (28, 29, 32): return False
            ty = instruction[3 if op == 32 else 2]
        return False
    if not any(opcode(item) in (54, 55) and descriptor(item[1]) for item in m.code):
        return data
    return subprocess.run([optimizer, '--merge-return', '--inline-entry-points-exhaustive',
                           '--eliminate-dead-functions', '-', '-o', '-'],
                          input=data, stdout=subprocess.PIPE, check=True).stdout


def nonuniform_slots(data, reflection):
    """Shared Metal slot arrays permit a different resource in every lane."""
    resources = {b['msl_name']: b for b in reflection['bindings']
                 if b.get('msl_name', '').endswith('_cb0_40000000') and b.get('kind') not in ('tex_pool', 'samp_pool')}
    if not resources: return data
    m = Module(data)
    names = {x[1]: string_operand(x[2:]) for x in m.code if opcode(x) == 5}
    arrays = {}; caps = {5301}
    for x in m.code:
        if opcode(x) != 59 or names.get(x[2]) not in resources: continue
        array = m.types[m.types[x[1]][3]]
        if x[3] != 0 or opcode(array) != 28 or not 1 <= m.constants.get(array[3], 0) <= 32:
            raise ValueError('invalid logical descriptor array')
        ty = m.types[array[2]]
        if opcode(ty) == 27: ty = m.types[ty[2]]
        cap = 5311 if opcode(ty) == 25 and ty[3] == 5 else 5307
        arrays[x[2]] = cap
    if len(arrays) != len(resources): raise ValueError('incomplete logical descriptor arrays')
    nonuniform = set()
    for x in m.code:
        if opcode(x) in (65, 66) and x[3] in arrays:
            if len(x) != 5: raise ValueError('nested logical descriptor index')
            nonuniform.update((x[2], x[4])); caps.add(arrays[x[3]])
        elif any(var in arrays for var in pointer_operands(x)):
            raise ValueError('unsupported whole logical descriptor array use')
    # Follow selected descriptors through loads and sampled-image construction.
    # A fixed point also covers backedge Phi values without relying on block order.
    while True:
        before = len(nonuniform)
        for x in m.code:
            op = opcode(x)
            operands = x[3:4] if op in (61, 83, 100) else x[3:] if op == 86 else x[4:6] if op == 169 else x[3::2] if op == 245 else ()
            if any(value in nonuniform for value in operands): nonuniform.add(x[2])
        if len(nonuniform) == before: break
    annotations = [instruction(71, value, 5300) for value in sorted(nonuniform) if 5300 not in m.decorations[value]]
    existing = {x[1] for x in m.code if opcode(x) == 17}
    extensions = {string_operand(x[1:]) for x in m.code if opcode(x) == 10}
    prefix = [x for x in m.code if opcode(x) == 17] + [instruction(17, cap) for cap in sorted(caps - existing)]
    prefix += [x for x in m.code if opcode(x) == 10]
    if 'SPV_EXT_descriptor_indexing' not in extensions:
        name = b'SPV_EXT_descriptor_indexing\0'; name += b'\0' * (-len(name) % 4)
        prefix.append(instruction(10, *struct.unpack(f'<{len(name)//4}I', name)))
    words = list(m.header); inserted = False
    for x in prefix + [x for x in m.code if opcode(x) not in (17, 10)]:
        if not inserted and 19 <= opcode(x) <= 39:
            for annotation in annotations: words.extend(annotation)
            inserted = True
        words.extend(x)
    return struct.pack(f'<{len(words)}I', *words)


def slot_array(resource, descriptor, count):
    """Mirror the shared Apple TEXD/SAMPD recognition, without changing its reflection."""
    if resource.get('kind') in ('tex_pool', 'samp_pool') or not resource.get('msl_name', '').endswith('_cb0_40000000'):
        return resource
    match = re.fullmatch(r'array<(.+),\s*(\d+)>', resource.get('msl_type', ''))
    if not match or not 1 <= count <= 32 or int(match[2]) != count:
        raise ValueError('invalid logical slot array')
    element = match[1]
    if element.startswith('sampler') and descriptor == 'samp': kind = 'samp_slots'
    elif element.startswith(('texture', 'depth')) and descriptor in ('tex', 'cis', 'tbuf', 'utb'): kind = 'tex_slots'
    else: raise ValueError('incompatible logical slot array descriptor')
    return dict(resource, kind=kind, reg=None)


def fields(binding):
    kind = binding['kind']
    if kind in ('tex_slots', 'samp_slots'):
        types = ('samp',) if kind == 'samp_slots' else ('tex', 'cis', 'tbuf', 'utb')
        if (binding['type'] not in types or type(binding['count']) is not int or
                not 1 <= binding['count'] <= 32 or binding.get('reg') is not None):
            raise ValueError('invalid logical slot array binding')
    result = []
    for name in ('fetch1d', 'fetch1d_size'):
        if name not in binding: continue
        if type(binding[name]) is not bool: raise ValueError('invalid wrapped fetch flag')
        if binding[name]: result.append(name + '=1')
    sampled = ('tex', 'tex_ubo', 'tex_slots', 'tex_c2a', 'tex_htab', 'tex_pool')
    if result and (kind not in sampled or binding['type'] not in ('tex', 'cis') or
                   type(binding['count']) is not int or binding['count'] < 1 or
                   binding.get('view') != 1 or not binding.get('fetch1d')):
        raise ValueError('invalid wrapped fetch binding')
    return tuple(result)
