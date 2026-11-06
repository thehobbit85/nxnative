"""Reproduce Maxwell's 32-lane compute shuffles with one word per invocation."""
import math
import struct

from dm.android.tools.spv_capture import Module, instruction
from dm.android.tools.spv_module import decode, opcode, pointer_operands


def _workgroup_size(code):
    entries = [inst for inst in code if opcode(inst) == 15]
    if len(entries) != 1 or entries[0][1] != 5:  # GLCompute
        return 0
    sizes = [inst[3:] for inst in code if opcode(inst) == 16 and inst[1:3] == (entries[0][2], 17)]
    if len(sizes) != 1 or len(sizes[0]) != 3 or min(sizes[0]) < 1:
        return 0
    size = math.prod(sizes[0])
    return size if size <= 1024 else 0


def _qualifies(module, lane):
    # Each shuffle becomes two workgroup barriers. A branch or early return
    # that excludes even one warp would make that transformation invalid.
    # Only accept functions with a single block, including all their callees.
    seen_label = False
    for inst in module.code:
        op = opcode(inst)
        if op == 54:
            seen_label = False
        if op == 248:
            if seen_label:
                return False
            seen_label = True
        if lane in pointer_operands(inst) and (op != 61 or inst[3] != lane or inst[1] != module.uint):
            return False
        if op == 59 and inst[3] == 4:
            # Preserve the guest's shared-memory budget. With no other
            # Workgroup allocation, our scratch occupies at most 4 KiB.
            return False
        if 333 <= op <= 366:
            if op != 345 or module.constants.get(inst[3]) != 3:
                return False
            result = module.types[inst[1]]
            if opcode(result) not in (21, 22) or result[2] != 32:
                return False
    return True


def lower(data):
    _, code = decode(data)
    if not any(opcode(inst) == 345 for inst in code):  # OpGroupNonUniformShuffle
        return data
    size = _workgroup_size(code)
    if not size:
        return data
    module = Module(data)
    builtins = {inst[3]: inst[1] for inst in code if opcode(inst) == 71 and inst[2] == 11}
    lane = builtins.get(41)  # SubgroupLocalInvocationId
    # A WorkgroupSize built-in takes precedence over LocalSize. Its value
    # may be specialized at pipeline creation, after we allocate the array.
    other_subgroup_inputs = set(range(36, 41)) | set(range(4416, 4421))
    if 25 in builtins or lane is None or other_subgroup_inputs.intersection(builtins):
        return data
    if not _qualifies(module, lane):
        return data

    # Retarget the input variable to LocalInvocationIndex, which flattens
    # x, y and z in guest order. Reuse an existing index input when present.
    index = builtins.get(29, lane)
    input_types = {inst[2]: module.types[inst[1]][3] for inst in code if opcode(inst) == 59 and inst[3] == 1}
    if any(input_types.get(var) != module.uint for var in (lane, index)):
        return data
    ptr = module.ty(32, 4, module.uint)
    count = module.const(size)
    scratch = module.variable(module.ty(28, module.uint, count), 4)
    mask = module.const(31)
    warp_mask = module.const(0xffffffe0)
    group_scope = module.const(2)
    acquire_release_workgroup = module.const(0x108)
    barrier = instruction(224, group_scope, group_scope, acquire_release_workgroup)
    output = []
    declared = False
    for inst in code:
        op = opcode(inst)
        if op == 54 and not declared:
            output.extend(module.declarations)
            declared = True
        if op == 17 and inst[1] in (62, 64, 65):  # unused Vote, Ballot and Shuffle
            continue
        if op == 71 and inst[1] == lane and inst[2] == 11:
            if index == lane:
                output.append(instruction(71, lane, 11, 29))
            continue
        if index != lane:
            if (op in (5, 71) and inst[1] == lane) or (op == 59 and inst[2] == lane):
                continue
        if op == 15:
            # A declared but previously unused index might not be in the
            # entry point interface. Add it even in that case.
            end = next(i + 1 for i in range(3, len(inst)) if inst[i] < 0x01000000)
            interfaces = [key for key in inst[end:] if key not in (lane, index)]
            output.append(instruction(15, *inst[1:end], *interfaces, index))
            continue
        if op == 61 and inst[3] == lane:
            linear = module.value(output, 61, module.uint, index)
            output.append(instruction(199, module.uint, inst[2], linear, mask))
        elif op == 345:
            ty, result, _, value, source = inst[1:]
            linear = module.value(output, 61, module.uint, index)
            own = module.value(output, 65, ptr, scratch, linear)
            bits = value if ty == module.uint else module.value(output, 124, module.uint, value)
            output.extend((instruction(62, own, bits), barrier))
            # Ignore high bits of indices outside the guest warp; the original
            # Maxwell helper selects its own value for an invalid source. In a
            # partial final warp, never address an unallocated scratch word.
            warp = module.value(output, 199, module.uint, linear, warp_mask)
            source_lane = module.value(output, 199, module.uint, source, mask)
            src = module.value(output, 197, module.uint, warp, source_lane)
            active = module.value(output, 176, module.boolean, src, count)
            bounded = module.value(output, 169, module.uint, active, src, linear)
            address = module.value(output, 65, ptr, scratch, bounded)
            bits = module.value(output, 61, module.uint, address)
            output.append(instruction(83 if ty == module.uint else 124, ty, result, bits))
            # All reads must finish before a later shuffle reuses this array.
            output.append(barrier)
        else:
            output.append(inst)

    words = [*module.header[:3], module.next_id, module.header[4]]
    for inst in output:
        words.extend(inst)
    return struct.pack(f'<{len(words)}I', *words)
