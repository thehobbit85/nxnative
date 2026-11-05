"""Vulkan lowering for the shared geometry-as-compute helpers."""
import struct

from dm.android.tools.spv_capture import Module, instruction
from dm.android.tools.spv_module import decode, opcode, string_operand


def topology_buffer(data):
    """Read the topology remap through an unsigned texel view, preserving bits.

    The shared translator represents this internal uint table as a float buffer.
    A floating-point image fetch may flush small index bit patterns to zero.
    Retype only this helper's image/load/fetch chain, then bitcast the result
    back into the translator's existing bit-preserving loads and stores. Guest
    textures may share its original type and must keep their own numeric view.
    """
    _, code = decode(data)
    named = [i[1] for i in code if opcode(i) == 5 and string_operand(i[2:]) == 'trb_data']
    if not named: return data
    if len(named) != 1: raise ValueError('ambiguous geometry topology buffer')
    variable, = named
    m = Module(data)
    declarations = [i for i in code if opcode(i) == 59 and i[2] == variable]
    if len(declarations) != 1 or declarations[0][3] != 0:
        raise ValueError('geometry topology needs a sampled buffer')
    declaration, = declarations
    pointer = m.types[declaration[1]]
    sampled = m.types[pointer[3]]
    if opcode(sampled) != 27: raise ValueError('geometry topology needs a sampled buffer')
    image = m.types[sampled[2]]
    if opcode(image) != 25 or image[3:] != (5, 0, 0, 0, 1, 0):
        raise ValueError('geometry topology needs a non-array sampled buffer')
    scalar = m.types[image[2]]
    if opcode(scalar) == 21 and scalar[2:] == (32, 0): return data
    if opcode(scalar) != 22 or scalar[2] != 32:
        raise ValueError('geometry topology needs a 32-bit buffer')
    unsigned_image = m.ty(25, m.uint, *image[3:])
    unsigned_sampled = m.ty(27, unsigned_image)
    unsigned_pointer = m.ty(32, 0, unsigned_sampled)
    # Move the declaration after its new types. Retain its ID, name, binding
    # and set decorations; shared image types themselves remain untouched.
    m.declarations.append(instruction(59, unsigned_pointer, variable, *declaration[3:]))
    loaded = set(); images = set(); rewritten = []
    for i in code:
        op = opcode(i)
        if op == 59 and i[2] == variable: continue
        if op == 61 and i[3] == variable:
            loaded.add(i[2]); i = instruction(op, unsigned_sampled, *i[2:])
        elif op == 100 and i[3] in loaded:  # OpImage
            images.add(i[2]); i = instruction(op, unsigned_image, *i[2:])
        elif op == 95 and i[3] in images:  # OpImageFetch
            vector = m.types[i[1]]
            if opcode(vector) != 23 or vector[2] != image[2] or vector[3] != 4:
                raise ValueError('unexpected geometry topology buffer fetch type')
            value = m.ident()
            rewritten.append(instruction(op, m.ty(23, m.uint, 4), value, *i[3:]))
            i = instruction(124, i[1], i[2], value)  # OpBitcast, never a numeric conversion
        rewritten.append(i)
    words = list(m.header); words[3] = m.next_id
    placed = False
    for i in rewritten:
        if opcode(i) == 54 and not placed:
            for declaration in m.declarations: words.extend(declaration)
            placed = True
        words.extend(i)
    if not placed: raise ValueError('geometry topology shader has no function')
    return struct.pack(f'<{len(words)}I', *words)


def viewport_mask_reader(data):
    """Match Metal's viewport-zero reader using X clipping, including depth clamp.

    The shared generator can store a scalar into the mask's declared int[1].
    Keep the mask privately as a scalar, then select an outside-X position when
    bit zero is clear. Other viewport bits do not broadcast on the Apple route.
    """
    _, code = decode(data)
    masks = [i[1] for i in code if opcode(i) == 71 and i[2:] == (11, 5253)]
    if not masks: return data
    entries = [i for i in code if opcode(i) == 15]
    if len(masks) != 1 or len(entries) != 1 or entries[0][1] != 0:
        raise ValueError('geometry mask reader needs one vertex entry and mask')
    mask, = masks; entry = entries[0][2]; m = Module(data)
    variables = {i[2]: i for i in code if opcode(i) == 59}
    variable = variables[mask]; array = m.types[m.types[variable[1]][3]]
    if len(variable) != 4 or variable[3] != 3 or opcode(array) != 28 or m.constants.get(array[3]) != 1:
        raise ValueError('geometry mask reader needs one output mask word')
    scalar = array[2]
    if opcode(m.types[scalar]) != 21 or m.types[scalar][2] != 32:
        raise ValueError('geometry mask reader needs a 32-bit mask')
    positions = []
    def position(variable, ty, path, decorations):
        if decorations.get(11) == (0,): positions.append((variable, ty, path))
        elif opcode(m.types[ty]) == 30:
            for index, child in enumerate(m.types[ty][2:]):
                position(variable, child, path + [index], m.members[ty, index])
    for ident, decl in variables.items():
        if decl[3] == 3 and ident != mask:
            position(ident, m.types[decl[1]][3], [], m.decorations[ident])
    if len(positions) != 1: raise ValueError('geometry mask reader needs one position output')
    pvar, ptype, path = positions[0]; vector = m.types[ptype]
    if opcode(vector) != 23 or vector[3] != 4 or m.types[vector[2]] != instruction(22, vector[2], 32):
        raise ValueError('geometry mask reader needs float4 position')
    private_pointer = m.ty(32, 6, scalar); one = m.const(1, scalar)
    m.declarations.append(instruction(59, private_pointer, mask, 6, one))
    hidden = m.ident()
    m.declarations.append(instruction(44, ptype, hidden,
                                     *(m.const(v, vector[2]) for v in (0x40000000, 0, 0, 0x3f800000))))
    bool4 = m.ty(23, m.boolean, 4)
    value_types = {i[2]: i[1] for i in code if len(i) > 2 and (opcode(i) == 12 or opcode(i) >= 41)
                   and opcode(i) not in (71, 72, 73, 74, 75) and i[1] in m.types}
    keep_extension = any(opcode(i) == 17 and i[1] in (5254, 5259, 5260) for i in code)
    rewritten = []; in_entry = False
    for i in code:
        op = opcode(i)
        if op == 59 and i[2] == mask: continue
        if op == 71 and i[1] == mask: continue
        if op == 17 and i[1] == 5255: continue  # ShaderViewportMaskNV
        if op == 10 and string_operand(i[1:]) == 'SPV_NV_viewport_array2' and not keep_extension: continue
        if op == 15:
            start = 3
            while not any((i[start] >> (8 * byte)) & 255 == 0 for byte in range(4)): start += 1
            start += 1
            i = instruction(op, *i[1:start], *(ident for ident in i[start:] if ident != mask))
        if op == 54: in_entry = i[2] == entry
        if op in (65, 66) and i[3] == mask:
            if len(i) != 5 or m.constants.get(i[4]) != 0:
                raise ValueError('unsupported geometry mask array access')
            i = instruction(83, private_pointer, i[2], mask)  # scalar pointer replaces mask[0]
        elif op == 62 and i[1] == mask:
            value = i[2]
            if value_types.get(value) == array[1]:
                value = m.value(rewritten, 81, scalar, value, 0)
            elif value_types.get(value) != scalar:
                raise ValueError('unsupported geometry mask store')
            i = instruction(62, mask, value, *i[3:])
        elif op == 61 and i[3] == mask:
            raise ValueError('unsupported whole-array geometry mask load')
        if op == 253 and in_entry:
            pointer = pvar if not path else m.value(rewritten, 65, m.ty(32, 3, ptype), pvar, *(m.const(k) for k in path))
            bits = m.value(rewritten, 61, scalar, mask)
            bit = m.value(rewritten, 199, scalar, bits, one)
            visible = m.value(rewritten, 170, m.boolean, bit, one)
            condition = m.value(rewritten, 80, bool4, *([visible] * 4))
            original = m.value(rewritten, 61, ptype, pointer)
            selected = m.value(rewritten, 169, ptype, condition, original, hidden)
            rewritten.append(instruction(62, pointer, selected))
        if op == 56: in_entry = False
        rewritten.append(i)
    words = list(m.header); words[3] = m.next_id; placed = False
    for i in rewritten:
        if opcode(i) == 54 and not placed:
            for declaration in m.declarations: words.extend(declaration)
            placed = True
        words.extend(i)
    if not placed: raise ValueError('geometry mask reader has no function')
    return struct.pack(f'<{len(words)}I', *words)
