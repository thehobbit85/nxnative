"""Vertex-output capture in the shared Metal geometry ABI, using Vulkan buffers.

The six control words are first vertex, vertex count, first instance, instance
count, destination word offset and record capacity. Capture preserves raw bits
in dmsh_geometry.WORDS order. The generated entry is used with raster discard.
"""
import collections
import struct

from dm.android.tools.spv_module import decode, opcode
from dm.gfx.tools.dmsh_geometry import WORDS


def instruction(op, *args):
    return ((len(args) + 1) << 16 | op, *args)


class Module:
    def __init__(self, data):
        self.header, self.code = decode(data)
        self.next_id = self.header[3]
        self.declarations = []
        self.annotations = []
        self.names = []
        self.interfaces = []
        self.types = {i[1]: i for i in self.code if 19 <= opcode(i) <= 39}
        self.constants = {i[2]: i[3] for i in self.code if opcode(i) == 43 and len(i) == 4}
        self.decorations = collections.defaultdict(dict)
        self.members = collections.defaultdict(dict)
        for i in self.code:
            if opcode(i) == 71: self.decorations[i[1]][i[2]] = i[3:]
            if opcode(i) == 72: self.members[i[1], i[2]][i[3]] = i[4:]
        self.uint = self.ty(21, 32, 0)
        self.boolean = self.ty(20)

    def ident(self):
        result = self.next_id; self.next_id += 1
        return result

    def ty(self, op, *args):
        for key, value in self.types.items():
            if opcode(value) == op and value[2:] == args: return key
        key = self.ident(); value = instruction(op, key, *args)
        self.types[key] = value; self.declarations.append(value)
        return key

    def aggregate(self, op, *args):
        # Aggregates own their layout decorations; do not reuse an existing
        # array or block that may carry a different stride or member layout.
        key = self.ident(); value = instruction(op, key, *args)
        self.types[key] = value; self.declarations.append(value)
        return key

    def const(self, value, ty=None):
        ty = self.uint if ty is None else ty
        for i in self.code + self.declarations:
            if opcode(i) == 43 and i[1] == ty and i[3:] == (value,): return i[2]
        key = self.ident(); self.declarations.append(instruction(43, ty, key, value))
        self.constants[key] = value
        return key

    def variable(self, ty, storage, name=None):
        key = self.ident(); self.declarations.append(instruction(59, self.ty(32, storage, ty), key, storage))
        if name:
            data = name.encode() + b'\0'; data += b'\0' * (-len(data) % 4)
            self.names.append(instruction(5, key, *struct.unpack(f'<{len(data) // 4}I', data)))
        return key

    def decorate(self, key, decoration, *args):
        self.annotations.append(instruction(71, key, decoration, *args))

    def value(self, code, op, ty, *args):
        key = self.ident(); code.append(instruction(op, ty, key, *args)); return key

    def builtin(self, builtin):
        for i in self.code:
            if opcode(i) == 59 and i[3] == 1 and self.decorations[i[2]].get(11) == (builtin,):
                return i[2], self.types[i[1]][3]
        ty = self.ty(21, 32, 1); key = self.variable(ty, 1)
        self.decorate(key, 11, builtin); self.interfaces.append(key)
        return key, ty

    def output_words(self):
        words = {}
        def builtin_used(variable, path):
            # The shared translator declares all gl_PerVertex members, including
            # unused CullDistance. SPIRV-Cross emits only active builtins. Prove
            # absence from the original functions before omitting a member;
            # whole-block loads/stores, dynamic indices and pointer escapes keep
            # it active, so unsupported live outputs still fail explicitly.
            in_function = False
            for i in self.code:
                op = opcode(i)
                if op == 54: in_function = True
                if op == 59 and i[2] == variable and len(i) > 4: return True
                # OpExtInst's instruction number is a literal, not an ID:
                # GLSL Fma (50) can coincide with the output block's ID 50.
                # Its actual arguments still count, including pointer escapes.
                operands = i[1:4] + i[5:] if op == 12 else i[1:]
                if in_function and variable in operands:
                    if op in (65, 66) and i[3] == variable:
                        access = [self.constants.get(index) for index in i[4:]]
                        if any(a is not None and a != b for a, b in zip(access, path)): continue
                    return True
                if op == 56: in_function = False
            return False
        def scalar_words(ty, path, located):
            value = self.types[ty]; op = opcode(value)
            if op in (21, 22) and value[2] == 32: return [(0, ty, path)], 1
            if op == 23:
                return [(c, value[2], path + [c]) for c in range(value[3])], value[3]
            if op in (24, 28):
                count = value[3] if op == 24 else self.constants[value[3]]
                if not 1 <= count <= 32: raise ValueError('geometry capture array exceeds word map')
                values = []; span = 0
                for c in range(count):
                    part, width = scalar_words(value[2], path + [c], located)
                    stride = (width + 3) // 4 * 4 if located else width
                    values += [(span + offset, scalar, indices) for offset, scalar, indices in part]
                    span += stride
                    if span > WORDS: raise ValueError('geometry capture array exceeds word map')
                return values, span
            raise ValueError('unsupported geometry capture output type')
        def add(variable, ty, path, dec):
            if opcode(self.types[ty]) == 30:
                location = dec.get(30, (None,))[0]
                span = 0
                for member, subtype in enumerate(self.types[ty][2:]):
                    member_dec = dict(self.members[ty, member])
                    if location is not None and 30 not in member_dec and 11 not in member_dec:
                        member_dec[30] = (location + span // 4,)
                    width = add(variable, subtype, path + [member], member_dec)
                    span += (width + 3) // 4 * 4
                return span
            if 11 in dec and not builtin_used(variable, path): return 0
            values, width = scalar_words(ty, path, 30 in dec)
            if 30 in dec:
                location = dec[30][0]; component = dec.get(31, (0,))[0]
                if location >= 32 or component >= 4 or location * 4 + component + width > 128:
                    raise ValueError('geometry capture output exceeds attribute map')
                start = 16 + location * 4 + component
            else:
                builtin = dec.get(11, (None,))[0]
                layouts = {0: (0, 4), 1: (4, 1), 3: (5, 8), 9: (13, 1), 10: (14, 1)}
                if builtin not in layouts: raise ValueError('unsupported geometry capture builtin')
                start, count = layouts[builtin]
                if width > count: raise ValueError('geometry capture builtin exceeds word map')
            for offset, scalar, indices in values:
                if opcode(self.types[scalar]) not in (21, 22) or self.types[scalar][2] != 32:
                    raise ValueError('geometry capture needs 32-bit output components')
                if start + offset in words: raise ValueError('overlapping geometry capture outputs')
                words[start + offset] = (variable, scalar, indices)
            return width
        for i in self.code:
            if opcode(i) == 59 and i[3] == 3:
                add(i[2], self.types[i[1]][3], [], self.decorations[i[2]])
        return words


def geometry_capture(data):
    m = Module(data)
    entries = [i for i in m.code if opcode(i) == 15]
    if len(entries) != 1 or entries[0][1] != 0:
        raise ValueError('geometry capture needs one vertex entry point')
    entry = entries[0][2]; outputs = m.output_words()
    bindings = [i[3] for i in m.code if opcode(i) == 71 and i[2] == 33]
    binding = max(bindings, default=-1) + 1
    if binding >= 0xffffffff: raise ValueError('no descriptor bindings available for geometry capture')
    array = m.aggregate(29, m.uint); m.decorate(array, 6, 4)  # ArrayStride
    storage = m.aggregate(30, array); m.decorate(storage, 3)  # BufferBlock
    m.annotations.append(instruction(72, storage, 0, 35, 0))
    destination = m.variable(storage, 2, 'dm_geometry_output')
    control_type = m.aggregate(30, *([m.uint] * 6)); m.decorate(control_type, 2)
    for k in range(6): m.annotations.append(instruction(72, control_type, k, 35, k * 4))
    control = m.variable(control_type, 2, 'dm_geometry_control')
    for variable, b in ((destination, binding), (control, binding + 1)):
        m.decorate(variable, 34, 0); m.decorate(variable, 33, b)
    vid, vty = m.builtin(42); iid, ity = m.builtin(43)
    uint_ptr = m.ty(32, 2, m.uint)
    pair = m.aggregate(30, m.uint, m.uint)
    zero = m.const(0); word_count = m.const(WORDS)
    prologue = []
    def calc(op, *args, ty=None): return m.value(prologue, op, m.uint if ty is None else ty, *args)
    def load_input(variable, ty):
        loaded = calc(61, variable, ty=ty)
        return loaded if ty == m.uint else calc(124, loaded)
    params = [calc(61, calc(65, control, m.const(k), ty=uint_ptr)) for k in range(6)]
    vertex = calc(130, load_input(vid, vty), params[0])
    instance = calc(130, load_input(iid, ity), params[2])
    product = calc(151, instance, params[1], ty=pair)  # UMulExtended
    linear = calc(149, calc(81, product, 0), vertex, ty=pair)  # IAddCarry
    index = calc(81, linear, 0)
    span = calc(151, index, word_count, ty=pair)
    address = calc(149, calc(81, span, 0), params[4], ty=pair)
    base = calc(81, address, 0)
    length = calc(68, destination, 0)  # ArrayLength of the bound output range
    checks = [calc(176, vertex, params[1], ty=m.boolean), calc(176, instance, params[3], ty=m.boolean),
              calc(176, index, params[5], ty=m.boolean), calc(174, length, word_count, ty=m.boolean),
              calc(178, base, calc(130, length, word_count), ty=m.boolean)]
    checks += [calc(170, calc(81, value, 1), zero, ty=m.boolean) for value in (product, linear, span, address)]
    valid = checks[0]
    for check in checks[1:]: valid = calc(167, valid, check, ty=m.boolean)
    body, end = m.ident(), m.ident()
    prologue += [instruction(247, end, 0), instruction(250, valid, body, end), instruction(248, body)]
    # Entirely absent fields use dmsh_geometry.capture's record defaults.
    # Declared fields follow the shared Metal builder's out={} and vs_init:
    # only float4 generic outputs initialize W to 1. Integer and builtin
    # fields begin at zero, then the original shader overwrites them.
    defaults = {k: 0x3f800000 if k in (3, 4) or k >= 19 and k % 4 == 3 else 0 for k in range(WORDS)}
    variables = {i[2]: i for i in m.code if opcode(i) == 59}
    for word, (variable, ty, indices) in outputs.items():
        pointer = variable if not indices else m.value(prologue, 65, m.ty(32, 3, ty), variable, *(m.const(i) for i in indices))
        parent = None; subtype = m.types[variables[variable][1]][3]
        for index in indices:
            parent = m.types[subtype]
            subtype = parent[index + 2] if opcode(parent) == 30 else parent[2]
        value = 0x3f800000 if (word >= 16 and parent is not None and opcode(parent) == 23 and parent[3] == 4 and
                               indices[-1] == 3 and opcode(m.types[ty]) == 22) else 0
        prologue.append(instruction(62, pointer, m.const(value, ty)))

    def stores():
        code = []
        for word in range(WORDS):
            value = m.const(defaults[word])
            if word in outputs:
                variable, ty, indices = outputs[word]
                pointer = variable if not indices else m.value(code, 65, m.ty(32, 3, ty), variable, *(m.const(i) for i in indices))
                value = m.value(code, 61, ty, pointer)
                if ty != m.uint: value = m.value(code, 124, m.uint, value)
            index = m.value(code, 128, m.uint, base, m.const(word))
            pointer = m.value(code, 65, uint_ptr, destination, zero, index)
            code.append(instruction(62, pointer, value))
        return code

    rewritten = []; in_entry = False; insert = False; started = False
    for i in m.code:
        op = opcode(i)
        if op == 54: in_entry = i[2] == entry
        if op == 248 and in_entry and not started:
            rewritten.append(i); insert = started = True; continue
        if insert and op not in (59, 8, 317):
            rewritten += prologue; insert = False
        if op == 253 and in_entry: rewritten += stores()
        if op == 56 and in_entry:
            rewritten += [instruction(248, end), instruction(253)]
            in_entry = False
        if op == 15: i = instruction(15, *i[1:], *m.interfaces)
        rewritten.append(i)
    if not started: raise ValueError('geometry capture entry has no block')
    annotation_at = next((j for j, i in enumerate(rewritten) if opcode(i) in (71, 72, 73, 74, 75)),
                         next(j for j, i in enumerate(rewritten) if 19 <= opcode(i) <= 39))
    type_at = next(j for j, i in enumerate(rewritten) if 19 <= opcode(i) <= 39)
    function_at = next(j for j, i in enumerate(rewritten) if opcode(i) == 54)
    words = list(m.header); words[3] = m.next_id
    for j, i in enumerate(rewritten):
        if j == annotation_at:
            for x in m.names: words.extend(x)
        if j == type_at:
            for x in m.annotations: words.extend(x)
        if j == function_at:
            for x in m.declarations: words.extend(x)
        words.extend(i)
    return struct.pack(f'<{len(words)}I', *words), dict(words=WORDS, control_words=6,
                                                     output_binding=binding, control_binding=binding + 1)
