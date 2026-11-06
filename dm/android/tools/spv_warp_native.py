"""Partition native subgroup exchanges into 32-lane NVIDIA warps.

The shared Ryujinx shuffle/vote helpers select a warp base and ballot word when
the subgroup width is known at translation. Bundled Vulkan shaders can run on
different drivers. Apply the same selection to the remaining native operations
after converged compute shaders have taken the workgroup-memory path.
"""
import struct

from dm.android.tools import spv_warp, spv_warp_masks
from dm.android.tools.spv_capture import Module, instruction
from dm.android.tools.spv_module import decode, opcode, string_operand

MARKER = 'nxnative.native-warp32.v1'


def _bool_true(module):
    for inst in module.code + module.declarations:
        if opcode(inst) == 41:
            return inst[2]
    result = module.ident()
    module.declarations.append(instruction(41, module.boolean, result))
    return result


def _vote(module, code, inst, physical):
    m = module
    word = m.value(code, 194, m.uint, physical, m.const(5))
    vec = m.ty(23, m.uint, 4)
    def ballot(predicate):
        bits = m.value(code, spv_warp.BALLOT, vec, inst[3], predicate)
        return m.value(code, 77, m.uint, bits, word)
    mask = ballot(inst[4])
    zero = m.const(0)
    op = opcode(inst)
    if op == spv_warp.BALLOT:
        code.append(instruction(80, vec, inst[2], mask, zero, zero, zero))
    elif op == spv_warp.VOTE_ANY:
        code.append(instruction(171, m.boolean, inst[2], mask, zero))
    else:
        active = ballot(_bool_true(m))
        if op == spv_warp.VOTE_ALL:
            code.append(instruction(170, m.boolean, inst[2], mask, active))
        else:
            all_true = m.value(code, 170, m.boolean, mask, active)
            all_false = m.value(code, 170, m.boolean, mask, zero)
            code.append(instruction(166, m.boolean, inst[2], all_true, all_false))


def lower(data):
    header, code = decode(data)
    if header[1] < 0x10300:
        return data
    inputs = [(i[3], i[1]) for i in code if opcode(i) == 71 and i[2] == 11]
    builtins = dict(inputs)
    lane = builtins.get(41)
    ops = {opcode(i) for i in code if 333 <= opcode(i) <= 366}
    if not ops and lane is None and not spv_warp_masks.MASKS.intersection(builtins):
        return data
    if any(opcode(i) == 330 and string_operand(i[1:]) == MARKER for i in code):
        return data
    if len(inputs) != len(builtins) or ops - spv_warp.COLLECTIVES or set(range(36, 41)).intersection(builtins):
        return data
    if sum(opcode(i) == 15 for i in code) != 1:
        return data
    m = Module(data)
    if not spv_warp.supported_subgroup_ops(m, lane):
        return data
    masks = spv_warp_masks.inputs(m, builtins)
    if masks is None:
        return data
    if not ops and not any(opcode(i) == 61 and (i[3] == lane or i[3] in masks) for i in code):
        return data
    if ops - {spv_warp.SHUFFLE} and not any(opcode(i) == 17 and i[1] == 64 for i in code):
        # Keep support for modules/devices that require votes but lack ballot.
        return data
    if lane is None:
        lane = m.variable(m.uint, 1)
        m.decorate(lane, 11, 41)
    elif not any(opcode(i) == 59 and i[2] == lane and i[3] == 1 and m.types[i[1]][3] == m.uint for i in code):
        return data
    result = []
    for inst in code:
        op = opcode(inst)
        if op == 15:
            end = next(i + 1 for i in range(3, len(inst)) if inst[i] < 0x01000000)
            interface = [v for v in inst[end:] if v != lane and v not in masks]
            result.append(instruction(15, *inst[1:end], *interface, lane))
        elif masks and ((op in (5, 71) and inst[1] in masks) or (op in (59, 65, 66) and inst[2] in masks)):
            continue
        elif op == 61 and inst[3] in masks:
            spv_warp_masks.emit(m, result, inst, lane, *masks[inst[3]])
        elif op == 61 and inst[3] == lane:
            physical = m.value(result, 61, m.uint, lane)
            result.append(instruction(199, m.uint, inst[2], physical, m.const(31)))
        elif op == spv_warp.SHUFFLE:
            physical = m.value(result, 61, m.uint, lane)
            base = m.value(result, 199, m.uint, physical, m.const(0xffffffe0))
            source = m.value(result, 199, m.uint, inst[5], m.const(31))
            source = m.value(result, 197, m.uint, base, source)
            result.append(instruction(op, *inst[1:5], source))
        elif op in spv_warp.COLLECTIVES:
            _vote(m, result, inst, m.value(result, 61, m.uint, lane))
        else:
            result.append(inst)

    # SPIR-V 1.3 group operations allow a debug-section processing record.
    # Prepared exports can cross this pass more than once.
    tag = MARKER.encode() + b'\0'
    tag += b'\0' * (-len(tag) % 4)
    start = next(i for i, inst in enumerate(result) if opcode(inst) in (71, 72) or 19 <= opcode(inst) <= 39)
    result.insert(start, instruction(330, *struct.unpack(f'<{len(tag)//4}I', tag)))
    first_type = next(i for i, inst in enumerate(result) if 19 <= opcode(inst) <= 39)
    result[first_type:first_type] = m.annotations
    first_function = next(i for i, inst in enumerate(result) if opcode(inst) == 54)
    result[first_function:first_function] = m.declarations
    words = [*m.header[:3], m.next_id, m.header[4]]
    for inst in result:
        words.extend(inst)
    return struct.pack(f'<{len(words)}I', *words)
