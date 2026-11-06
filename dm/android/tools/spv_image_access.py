"""Vulkan capabilities required by actual formatless image reads and writes."""
from dm.android.tools.spv_module import opcode


def required(instructions):
    # Image type operands: Sampled=2 (storage), Format=0 (Unknown). A typed
    # storage image or an unused declaration requires neither capability.
    types = [x for x in instructions if opcode(x) == 25]
    # SPIR-V ImageFormat enumerants that require StorageImageExtendedFormats,
    # even if an access to the corresponding image is optimized away later.
    result = {49} if any(6 <= x[8] <= 20 or 25 <= x[8] <= 29 or 34 <= x[8] <= 39 for x in types) else set()
    # A SubpassData input attachment also has Sampled=2 and Format=Unknown,
    # but OpImageRead on it needs InputAttachment, not a storage-image feature.
    unformatted = {x[1] for x in types if x[3] != 6 and x[7] == 2 and x[8] == 0}
    if not unformatted:
        return result
    # In function code an image result starts with its type and ID, whether
    # loaded, extracted from a descriptor array, or passed through a helper.
    # Keep module-level annotations out: a decoration on an image type has
    # a similar operand layout, but its next word is a literal, not a result ID.
    values = set(); in_function = False
    for x in instructions:
        op = opcode(x)
        if op == 54: in_function = True
        if (in_function or op == 1) and len(x) > 2 and x[1] in unformatted: values.add(x[2])
        if op == 56: in_function = False
    for x in instructions:
        op = opcode(x)
        if op in (98, 320) and x[3] in values: result.add(55)  # OpImageRead / OpImageSparseRead
        if op == 99 and x[1] in values: result.add(56)  # ImageWrite
    return result
