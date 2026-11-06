"""Find the native subgroup widths that preserve an exported guest warp."""
import operator
import subprocess

from dm.android.tools import spv_warp_native
from dm.android.tools.spv_module import decode, opcode, pointer_operands, string_operand

SUBGROUP_BUILTINS = {*range(36, 42), *range(4416, 4421)}
U32 = 0xffffffff
OPERATIONS = {128: operator.add, 130: operator.sub, 132: operator.mul,
              164: operator.eq, 165: operator.ne, 166: operator.or_, 167: operator.and_,
              170: operator.eq, 171: operator.ne, 172: operator.gt, 174: operator.ge,
              176: operator.lt, 178: operator.le, 197: operator.or_, 198: operator.xor, 199: operator.and_}


def _native_inputs(code):
    return {i[1]: i[3] for i in code if opcode(i) == 71 and i[2] == 11 and i[3] in SUBGROUP_BUILTINS}


def _scope_operands(inst):
    op = opcode(inst)
    if op == 224:
        return inst[1:3]  # control barrier: execution and memory scope
    if op == 225:
        return inst[1:2]
    if op in (228, 319):  # AtomicStore / AtomicFlagClear have no result
        return inst[2:3]
    if 227 <= op <= 242 or op in (318, 5614, 5615, 6035):
        return inst[4:5]
    return ()


def _subgroup_memory(code):
    scopes = {scope for inst in code for scope in _scope_operands(inst)}
    if not scopes:
        return False
    consts = {i[2]: i[3] for i in code if opcode(i) == 43 and len(i) == 4}
    return any(consts.get(scope, 3) == 3 for scope in scopes)


def _subgroup_extension(code):
    # These operations use subgroup semantics outside the core SPIR-V 1.3
    # opcodes handled by the partitioner. A mixed module must retain width 32.
    names = ('subgroup', 'group_non_uniform', 'group_uniform_arithmetic', 'shader_ballot')
    return any(any(name in string_operand(i[1:]).lower() for name in names) for i in code if opcode(i) == 10)


def _uses_native(code, inputs):
    return (any(333 <= opcode(i) <= 366 or (inputs and any(p in inputs for p in pointer_operands(i))) for i in code)
            or _subgroup_memory(code))


def _minimum_width(code, inputs):
    # An SSA value directly derived from the lane register has a known value
    # for each physical lane 0..31. Wider waves are partitioned by the producer.
    lanes = tuple(range(32))
    scalars = {i[1] for i in code if opcode(i) == 20 or (opcode(i) in (21, 22) and i[2] == 32)}
    values = {i[2]: (i[3],) * 32 for i in code if opcode(i) == 43 and len(i) == 4 and i[1] in scalars}
    varying = set()
    shuffles = []
    body = []
    inside = False
    for inst in code:
        op = opcode(inst)
        if op == 54:
            inside = True
        elif op == 56:
            inside = False
        if not inside:
            continue
        if op == 61 and inputs.get(inst[3]) == 41:
            values[inst[2]] = lanes
            varying.add(inst[2])
            continue
        if any(p in inputs for p in pointer_operands(inst)) or (333 <= op <= 366 and op != 345):
            return 32
        if op == 345:
            shuffles.append(inst)
        args = inst[3:]
        if op in OPERATIONS and inst[1] in scalars and len(args) == 2 and all(v in values for v in args):
            values[inst[2]] = tuple(OPERATIONS[op](a, b) & U32 for a, b in zip(values[args[0]], values[args[1]]))
        elif op in (83, 124) and inst[1] in scalars and inst[3] in values:
            values[inst[2]] = values[inst[3]]
        else:
            body.append(inst)
            continue
        if any(v in varying for v in args):
            varying.add(inst[2])

    # Consume all uses after collecting definitions: a Phi may refer to a
    # value on a loop back edge. Unknown operations must receive a value that
    # repeats within each physical subgroup. This also covers branch conditions,
    # function arguments and values actually stored in buffers or attachments.
    escapes = set()
    for inst in body:
        op = opcode(inst)
        if op == 345:
            operands = inst[4:5]  # the donor index is checked independently below
        else:
            operands = inst[1:]  # an ambiguous literal can only raise the requirement
        escapes.update(v for v in operands if v in varying)
    for width in (4, 8, 16):
        if any(any(value != values[key][lane % width] for lane, value in enumerate(values[key])) for key in escapes):
            continue
        for shuffle in shuffles:
            selector = values.get(shuffle[5])
            if selector is None or any(src // width != lane // width or src % width != selector[lane % width]
                                       for lane, src in enumerate(selector)):
                break
        else:
            return width
    return 32


def requirement(data, optimizer):
    _, code = decode(data)
    if (_subgroup_extension(code) or
            any(opcode(i) == 72 and i[3] == 11 and i[4] in SUBGROUP_BUILTINS for i in code)):
        return (32, 32)
    inputs = _native_inputs(code)
    if inputs and any(opcode(i) == 73 and i[1] in inputs for i in code):
        return (32, 32)
    if not _uses_native(code, inputs):
        return None
    partitioned = any(opcode(i) == 330 and string_operand(i[1:]) == spv_warp_native.MARKER for i in code)
    # Inline the translator's shuffle functions and promote its local registers
    # strictly for analysis. The optimization never changes the shipped program.
    run = subprocess.run([optimizer, '-O', '-', '-o', '-'], input=data, capture_output=True)
    if run.returncode:
        return (32, 128 if partitioned and not _subgroup_memory(code) else 32)
    _, optimized = decode(run.stdout)
    inputs = _native_inputs(optimized)
    if not _uses_native(optimized, inputs):
        return None
    if not partitioned or _subgroup_memory(optimized):
        return (32, 32)
    return (_minimum_width(optimized, inputs), 128)
