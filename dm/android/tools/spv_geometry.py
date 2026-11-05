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
