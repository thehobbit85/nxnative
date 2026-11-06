"""Use logical Maxwell warps for converged Vulkan compute collectives."""
import math
import struct

from dm.android.tools.spv_capture import Module, instruction
from dm.android.tools.spv_module import decode, opcode, pointer_operands

VOTE_ALL, VOTE_ANY, VOTE_EQUAL = 334, 335, 336
BALLOT, SHUFFLE = 339, 345
COLLECTIVES = {VOTE_ALL, VOTE_ANY, VOTE_EQUAL, BALLOT, SHUFFLE}


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
    # A branch or early return that excludes even one warp would make a
    # workgroup barrier invalid. Accept single-block functions and callees.
    seen_label = False
    bools = set()
    in_function = False
    for inst in module.code:
        op = opcode(inst)
        if op == 54:
            seen_label = False
            in_function = True
        elif op == 56:
            in_function = False
        # Module annotations and debug info can have a literal in place of
        # the result ID. Only typed constants, undef and function values count.
        if (in_function or op in (1, 41, 42, 46, 48, 49, 52)) and len(inst) > 2 and inst[1] == module.boolean:
            bools.add(inst[2])
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
            if op not in COLLECTIVES or module.constants.get(inst[3]) != 3:
                return False
            if op == SHUFFLE:
                result = module.types[inst[1]]
                if opcode(result) not in (21, 22) or result[2] != 32:
                    return False
            if op == VOTE_EQUAL and inst[4] not in bools:
                return False
    return True


class _Exchange:
    def __init__(self, module, size, index):
        self.module = module
        self.size = size
        self.index = index
        self.pointer = module.ty(32, 4, module.uint)
        self.scratch = module.variable(module.ty(28, module.uint, module.const(size)), 4)
        self.scope = module.const(2)
        self.barrier = instruction(224, self.scope, self.scope, module.const(0x108))  # AcquireRelease | WorkgroupMemory

    def emit(self, code, inst):
        m = self.module
        u32 = m.uint
        linear = m.value(code, 61, u32, self.index)
        warp = m.value(code, 199, u32, linear, m.const(0xffffffe0))
        own = m.value(code, 65, self.pointer, self.scratch, linear)
        if opcode(inst) == SHUFFLE:
            ty, result, _, value, source = inst[1:]
            bits = value if ty == u32 else m.value(code, 124, u32, value)
            code.extend((instruction(62, own, bits), self.barrier))
            source_lane = m.value(code, 199, u32, source, m.const(31))
            src = m.value(code, 197, u32, warp, source_lane)
            # Only active source lanes have defined values. The Maxwell helper
            # handles its own clamp, and scratch indexing is always in bounds.
            active = m.value(code, 176, m.boolean, src, m.const(self.size))
            bounded = m.value(code, 169, u32, active, src, linear)
            address = m.value(code, 65, self.pointer, self.scratch, bounded)
            bits = m.value(code, 61, u32, address)
            code.append(instruction(83 if ty == u32 else 124, ty, result, bits))
        else:
            lane = m.value(code, 199, u32, linear, m.const(31))
            self._vote(code, inst, own, lane, warp)
        # Another exchange can only reuse the array after every read completes.
        code.append(self.barrier)

    def _vote(self, code, inst, own, lane, warp):
        m = self.module
        zero = m.const(0)
        # One writer clears each scratch word. All lanes in a logical warp
        # then accumulate their predicate bits into that warp's first word.
        code.extend((instruction(62, own, zero), self.barrier))
        first = m.value(code, 65, self.pointer, self.scratch, warp)
        bit = m.value(code, 196, m.uint, m.const(1), lane)
        contribution = m.value(code, 169, m.uint, inst[4], bit, zero)
        m.value(code, 241, m.uint, first, self.scope, zero, contribution)  # OpAtomicOr
        code.append(self.barrier)
        mask = m.value(code, 61, m.uint, first)
        op = opcode(inst)
        if op == BALLOT:
            code.append(instruction(80, inst[1], inst[2], mask, zero, zero, zero))
        elif op == VOTE_ANY:
            code.append(instruction(171, m.boolean, inst[2], mask, zero))
        else:
            active = m.const(0xffffffff)
            tail = self.size % 32
            if tail:
                last = m.value(code, 170, m.boolean, warp, m.const(self.size - tail))
                active = m.value(code, 169, m.uint, last, m.const((1 << tail) - 1), active)
            if op == VOTE_ALL:
                code.append(instruction(170, m.boolean, inst[2], mask, active))
            else:
                all_true = m.value(code, 170, m.boolean, mask, active)
                all_false = m.value(code, 170, m.boolean, mask, zero)
                code.append(instruction(166, m.boolean, inst[2], all_true, all_false))


def lower(data):
    _, code = decode(data)
    if not any(opcode(inst) in COLLECTIVES for inst in code):
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
    if 25 in builtins or other_subgroup_inputs.intersection(builtins) or not _qualifies(module, lane):
        return data

    index = builtins.get(29, lane)
    input_types = {inst[2]: module.types[inst[1]][3] for inst in code if opcode(inst) == 59 and inst[3] == 1}
    if any(input_types.get(var) != module.uint for var in (lane, index) if var is not None):
        return data
    if index is None:
        index = module.variable(module.uint, 1)
        module.decorate(index, 11, 29)
    exchange = _Exchange(module, size, index)
    output = []
    for inst in code:
        op = opcode(inst)
        if op == 17 and inst[1] in (62, 64, 65):  # replaced Vote, Ballot and Shuffle
            continue
        if op == 71 and inst[1] == lane and inst[2] == 11:
            if index == lane:
                output.append(instruction(71, lane, 11, 29))
            continue
        if index != lane and ((op in (5, 71) and inst[1] == lane) or (op == 59 and inst[2] == lane)):
            continue
        if op == 15:
            # A declared but previously unused index might be absent.
            end = next(i + 1 for i in range(3, len(inst)) if inst[i] < 0x01000000)
            interfaces = [key for key in inst[end:] if key not in (lane, index)]
            output.append(instruction(15, *inst[1:end], *interfaces, index))
        elif op == 61 and inst[3] == lane:
            linear = module.value(output, 61, module.uint, index)
            output.append(instruction(199, module.uint, inst[2], linear, module.const(31)))
        elif op in COLLECTIVES:
            exchange.emit(output, inst)
        else:
            output.append(inst)
    first_type = next(i for i, inst in enumerate(output) if 19 <= opcode(inst) <= 39)
    output[first_type:first_type] = module.annotations
    first_function = next(i for i, inst in enumerate(output) if opcode(inst) == 54)
    output[first_function:first_function] = module.declarations
    words = [*module.header[:3], module.next_id, module.header[4]]
    for inst in output:
        words.extend(inst)
    return struct.pack(f'<{len(words)}I', *words)
