"""NVN2 task/mesh stage-memory export through ordinary Vulkan compute.

Reuse the shared Ampere translator and stage-memory layout. Each output record
is the complete stage memory, in uint words. The renderer consumes the retained
vertex/primitive maps and counts; a task/mesh module is not ordinary guest CS.

Control words: first group, group count, output word offset, output capacity,
input word offset, input capacity, input stride, reserved (zero). A task-fed
mesh input record is its child-group number followed by the task payload.
"""
from pathlib import Path
import struct
import sys

from dm.android.tools.spv_capture import Module, instruction
from dm.android.tools.spv_module import opcode

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'gfx/tools'))
import dmsh_meshstage as shared


def layout(info, offsets=None):
    """Validate the shared decoder's metadata before using it as an allocation."""
    if not isinstance(info, dict) or info.get('kind') not in ('ts', 'ms') or 'refused' in info:
        raise ValueError('invalid or refused mesh stage')
    if (type(info.get('threads')) is not int or not 1 <= info['threads'] <= 1024 or
            type(info.get('payload')) is not int or not 0 <= info['payload'] <= 0xffff):
        raise ValueError('invalid mesh threads or payload')
    if info['kind'] == 'ts':
        if info['payload'] < 8: raise ValueError('task payload has no launch count')
        if any(k in info for k in ('task', 'max_v', 'max_p', 'vertex', 'primitive')):
            raise ValueError('task stage has mesh output fields')
    else:
        if (type(info.get('task')) is not bool or
                type(info.get('max_v')) is not int or not 1 <= info['max_v'] <= shared.METAL_MAX_VERTICES or
                type(info.get('max_p')) is not int or not 1 <= info['max_p'] <= shared.METAL_MAX_PRIMITIVES):
            raise ValueError('invalid mesh output limits')
        for key in ('vertex', 'primitive'):
            values = info.get(key)
            if not isinstance(values, list) or len(values) > 132: raise ValueError('invalid mesh output map')
            order = []
            for pair in values:
                if not isinstance(pair, (list, tuple)) or len(pair) != 2: raise ValueError('invalid mesh output map')
                loc, comp = pair
                if ((loc is None and key != 'vertex') or
                        (loc is not None and (type(loc) is not int or not 0 <= loc < 32)) or
                        type(comp) is not int or not 0 <= comp < 4):
                    raise ValueError('invalid mesh output component')
                order.append((0 if loc is None else loc + 1) * 4 + comp)
            if order != sorted(set(order)): raise ValueError('invalid mesh output order')
        if not any(pair[0] is None for pair in info['vertex']): raise ValueError('mesh has no position output')
        if info['task'] and info['payload'] < 4: raise ValueError('mesh payload has no group number')
    lay, size = shared.memory(info)
    if size > shared.METAL_MAX_SHARED: raise ValueError('mesh stage memory exceeds reference limit')
    if offsets is not None and (not isinstance(offsets, list) or any(type(v) is not int for v in offsets) or offsets != lay):
        raise ValueError('mesh stage-memory offsets differ from decoded layout')
    return lay, size


def validate_module(data, info, offsets):
    if offsets is None: raise ValueError('missing mesh stage-memory offsets')
    lay, size = layout(info, offsets)
    m = Module(data)
    entries = [i for i in m.code if opcode(i) == 15]
    modes = [i for i in m.code if opcode(i) == 16]
    if (len(entries) != 1 or entries[0][1] != 5 or
            [i[3:] for i in modes if i[2] == 17] != [(info['threads'], 1, 1)] or
            any(i[1] != entries[0][2] or i[2] != 17 for i in modes)):
        raise ValueError('mesh needs one compute entry with its decoded group size')
    memory = [i for i in m.code if opcode(i) == 59 and i[3] == 4]
    if len(memory) != 1: raise ValueError('mesh needs exactly one stage-memory array')
    ty = m.types[m.types[memory[0][1]][3]]
    if opcode(ty) != 28 or ty[2] != m.uint or m.constants.get(ty[3]) != size // 4:
        raise ValueError('mesh stage-memory array differs from decoded size')
    return m, entries[0], memory[0][2], lay, size


def lines(info, control, stage):
    lay, size = layout(info)
    if stage != info['kind']: raise ValueError('mesh logical stage differs from metadata')
    if not isinstance(control, dict): raise ValueError('invalid mesh helper bindings')
    bindings = [control.get(k) for k in ('input_binding', 'output_binding', 'control_binding')]
    if any(type(v) is not int or not 0 <= v <= 0x7fffffff for v in bindings) or len(set(bindings)) != 3:
        raise ValueError('invalid mesh helper bindings')
    fields = [info['threads'], info['payload'], int(info.get('task', False)), info.get('max_v', 0),
              info.get('max_p', 0), size // 4, lay[2] // 4, lay[3] // 4, *bindings]
    result = ['mesh ' + ' '.join(map(str, fields))]
    for name, tag in (('vertex', 'meshv'), ('primitive', 'meshp')):
        if info.get(name):
            result.append(tag + ' ' + ' '.join(str(-1 if loc is None else loc) + ' ' + str(c) for loc, c in info[name]))
    return result


def body_workgroup(m, variable):
    """A flattened task-fed dispatch still exposes the child's local group ID.

    Keep the physical input for the wrapper's indexing; redirect the original
    functions' loads/access chains to a per-invocation logical group vector.
    """
    logical = m.variable(m.ty(23, m.uint, 3), 6, 'dm_mesh_group')
    pointers = {variable: logical}; code = []; inside = False
    for i in m.code:
        op = opcode(i)
        if op == 54: inside = True
        if inside:
            if op == 61 and i[3] in pointers:
                i = instruction(op, *i[1:3], pointers[i[3]], *i[4:])
            elif op in (65, 66, 67, 70, 83) and i[3] in pointers:
                pointer_type = m.ty(32, 6, m.types[i[1]][3])
                i = instruction(op, pointer_type, i[2], pointers[i[3]], *i[4:])
                pointers[i[2]] = i[2]
            elif ((op == 57 and any(v in pointers for v in i[4:])) or
                  (op in (169, 245) and any(v in pointers for v in i[3:]))):
                raise ValueError('unsupported escaping mesh workgroup pointer')
        code.append(i)
        if op == 56: inside = False
    m.code = code
    return logical


def lower(data, info, offsets):
    m, original, memory, _, size = validate_module(data, info, offsets)
    old_entry = original[2]; entry = m.ident()
    binding = max((i[3] for i in m.code if opcode(i) == 71 and i[2] == 33), default=-1) + 1
    if binding > 0x7ffffffd: raise ValueError('no mesh helper binding space')
    array = m.aggregate(29, m.uint); m.decorate(array, 6, 4)
    storage = m.aggregate(30, array); m.decorate(storage, 3)
    m.annotations.append(instruction(72, storage, 0, 35, 0))
    source = m.variable(storage, 2, 'dm_mesh_input')
    destination = m.variable(storage, 2, 'dm_mesh_output')
    m.decorate(source, 24)  # NonWritable
    ctltype = m.aggregate(30, *([m.uint] * 8)); m.decorate(ctltype, 2)
    for k in range(8): m.annotations.append(instruction(72, ctltype, k, 35, k * 4))
    control = m.variable(ctltype, 2, 'dm_mesh_control')
    for k, variable in enumerate((source, destination, control)):
        m.decorate(variable, 34, 0); m.decorate(variable, 33, binding + k)
    def builtin(number, ty):
        for i in m.code:
            if opcode(i) == 59 and i[3] == 1 and m.decorations[i[2]].get(11) == (number,):
                if m.types[i[1]][3] != ty: raise ValueError('invalid mesh input builtin type')
                return i[2]
        variable = m.variable(ty, 1); m.decorate(variable, 11, number); m.interfaces.append(variable)
        return variable
    group_var = builtin(26, m.ty(23, m.uint, 3)); lid_var = builtin(29, m.uint)
    up = m.ty(32, 2, m.uint); wp = m.ty(32, 4, m.uint)
    zero = m.const(0); one = m.const(1); words = m.const(size // 4)
    void = m.ty(19); function_type = m.ty(33, void)
    code = [instruction(54, void, entry, 0, function_type), instruction(248, m.ident())]
    def value(op, *args, ty=None): return m.value(code, op, m.uint if ty is None else ty, *args)
    def get(variable, index, pointer=up): return value(61, value(65, variable, index, ty=pointer))
    def get_buffer(variable, index): return value(61, value(65, variable, zero, index, ty=up))
    def put(variable, index, val, pointer=wp): code.append(instruction(62, value(65, variable, index, ty=pointer), val))
    params = [get(control, m.const(k)) for k in range(8)]
    group = value(61, group_var, ty=m.ty(23, m.uint, 3)); gx = value(81, group, 0)
    lid = value(61, lid_var)
    length = value(68, destination, 0)
    checks = [value(176, gx, params[1], ty=m.boolean), value(176, gx, params[3], ty=m.boolean),
              value(170, value(81, group, 1), zero, ty=m.boolean), value(170, value(81, group, 2), zero, ty=m.boolean),
              value(170, params[7], zero, ty=m.boolean), value(178, params[2], length, ty=m.boolean)]
    available = value(130, length, params[2])
    checks.append(value(176, gx, value(134, available, words), ty=m.boolean))
    task = info['kind'] == 'ms' and info['task']
    pay = (info['payload'] + 3) // 4
    srcbase = zero
    if task:
        slen = value(68, source, 0); need = m.const(pay + 1)
        # Test a complete record before any payload read. Division and subtraction
        # use a nonzero stride and are harmless even on a rejected range.
        stride = value(169, value(174, params[6], one, ty=m.boolean), params[6], one)
        checks += [value(176, gx, params[5], ty=m.boolean), value(174, params[6], need, ty=m.boolean),
                   value(178, params[4], slen, ty=m.boolean), value(174, slen, need, ty=m.boolean),
                   value(178, params[4], value(130, slen, need), ty=m.boolean)]
        checks.append(value(178, gx, value(134, value(130, value(130, slen, need), params[4]), stride), ty=m.boolean))
        srcbase = value(128, params[4], value(132, gx, stride))
    valid = checks[0]
    for check in checks[1:]: valid = value(167, valid, check, ty=m.boolean)
    body, end = m.ident(), m.ident()
    code += [instruction(247, end, 0), instruction(250, valid, body, end), instruction(248, body)]
    current = body
    def loop(count, emit):
        nonlocal current
        header, body_label, step, done, index, next_index = (m.ident() for _ in range(6))
        code.extend([instruction(249, header), instruction(248, header),
                     instruction(245, m.uint, index, lid, current, next_index, step)])
        condition = value(176, index, m.const(count), ty=m.boolean)
        code.extend([instruction(246, done, step, 0), instruction(250, condition, body_label, done), instruction(248, body_label)])
        emit(index)
        code.extend([instruction(249, step), instruction(248, step),
                     instruction(128, m.uint, next_index, index, m.const(info['threads'])),
                     instruction(249, header), instruction(248, done)])
        current = done
    def barrier(): code.append(instruction(224, m.const(2), m.const(2), m.const(264)))
    loop(size // 4, lambda index: put(memory, index, zero))
    barrier()
    if task:
        loop(pay, lambda index: put(memory, value(128, m.const(4), index),
                                   get_buffer(source, value(128, value(128, srcbase, one), index))))
        barrier()
    first = get_buffer(source, srcbase) if task else value(128, params[0], gx)
    set_group, group_done = m.ident(), m.ident()
    is_first = value(170, lid, zero, ty=m.boolean)
    code += [instruction(247, group_done, 0), instruction(250, is_first, set_group, group_done), instruction(248, set_group)]
    put(memory, zero, first)
    if task: put(memory, m.const(4), first)
    code += [instruction(249, group_done), instruction(248, group_done)]
    current = group_done
    barrier()
    if task:
        logical_group = body_workgroup(m, group_var)
        code.append(instruction(62, logical_group, value(80, first, zero, zero, ty=m.ty(23, m.uint, 3))))
    value(57, old_entry, ty=void)  # Every early return rejoins the wrapper.
    barrier()
    base = value(128, params[2], value(132, gx, words))
    def store(index):
        ptr = value(65, destination, zero, value(128, base, index), ty=up)
        code.append(instruction(62, ptr, get(memory, index, wp)))
    loop(size // 4, store)
    code += [instruction(249, end), instruction(248, end), instruction(253), instruction(56)]
    rewritten = []
    for i in m.code:
        if opcode(i) == 15: i = instruction(15, i[1], entry, *i[3:], *m.interfaces)
        if opcode(i) == 16: i = instruction(16, entry, *i[2:])
        rewritten.append(i)
    annotation_at = next((j for j, i in enumerate(rewritten) if opcode(i) in (71, 72, 73, 74, 75)),
                         next(j for j, i in enumerate(rewritten) if 19 <= opcode(i) <= 39))
    type_at = next(j for j, i in enumerate(rewritten) if 19 <= opcode(i) <= 39)
    function_at = next(j for j, i in enumerate(rewritten) if opcode(i) == 54)
    packed = list(m.header); packed[3] = m.next_id
    for j, i in enumerate(rewritten):
        if j == annotation_at:
            for item in m.names: packed.extend(item)
        if j == type_at:
            for item in m.annotations: packed.extend(item)
        if j == function_at:
            for item in m.declarations: packed.extend(item)
        packed.extend(i)
    for item in code: packed.extend(item)
    return struct.pack(f'<{len(packed)}I', *packed), dict(input_binding=binding, output_binding=binding + 1, control_binding=binding + 2)
