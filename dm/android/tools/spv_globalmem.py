"""Describe the shared raw-address shader ABI without treating its map as guest data."""
import struct

from dm.android.tools.spv_module import decode, opcode


def lower_physical_addresses(data):
    """Bitcast the shared translator's two address words directly to the pointer.

    PhysicalStorageBuffer64 permits a two-component 32-bit integer vector as
    the OpBitcast operand. Its intermediate uint64 is unnecessary; retaining
    it would exclude devices with bufferDeviceAddress but without shaderInt64.
    Leave actual 64-bit guest arithmetic and every other pointer form intact.
    """
    header, instructions = decode(data)
    types = {x[1]: x for x in instructions if opcode(x) in (21, 23, 32)}
    values = {x[2]: x for x in instructions if opcode(x) in (80, 124)}
    result = []; candidates = set()
    for instruction in instructions:
        x = instruction
        if opcode(x) == 120:  # OpConvertUToPtr
            pointer = types.get(x[1], ())
            cast = values.get(x[3], ())
            integer = types.get(cast[1], ()) if cast else ()
            pair = values.get(cast[3], ()) if len(cast) == 4 and opcode(cast) == 124 else ()
            vector = types.get(pair[1], ()) if pair else ()
            component = types.get(vector[2], ()) if len(vector) == 4 and opcode(vector) == 23 else ()
            if (len(pointer) == 4 and pointer[2] == 5349 and
                    len(integer) == 4 and opcode(integer) == 21 and integer[2:] == (64, 0) and
                    len(pair) == 5 and opcode(pair) == 80 and len(vector) == 4 and opcode(vector) == 23 and vector[3] == 2 and
                    len(component) == 4 and opcode(component) == 21 and component[2:] == (32, 0)):
                candidates.add(cast[2])
                x = (4 << 16 | 124, x[1], x[2], pair[2])
        result.append(x)
    changed = bool(candidates)
    if changed:
        # Older NDK optimizers deliberately skip DCE for physical addressing.
        # Remove only the now-unused address casts and their scalar type/constant
        # declarations. Counting every operand word is conservative: a literal
        # equal to an ID may retain it, but an actual use can never be missed.
        wide = {x[1] for x in result if opcode(x) == 21 and x[2] == 64}
        candidates |= wide | {x[2] for x in result if opcode(x) in (43, 46) and x[1] in wide}
        while candidates:
            definitions = {x[1] if opcode(x) == 21 else x[2]: x for x in result
                           if opcode(x) in (21, 43, 46, 124)}
            dead = {value for value in candidates if value in definitions and not any(
                value in x[1:] for x in result if x is not definitions[value] and
                not (opcode(x) in (5, 71) and x[1] == value))}
            if not dead: break
            result = [x for x in result if not any(x is definitions[value] for value in dead) and
                      not (opcode(x) in (5, 71) and x[1] in dead)]
            candidates -= dead
    words = list(header)
    for x in result: words.extend(x)
    return struct.pack(f'<{len(words)}I', *words), changed


def drop_unused_int64(data):
    """Retain Int64 exactly when a 64-bit integer type remains after lowering."""
    header, instructions = decode(data)
    if any(opcode(x) == 21 and x[2] == 64 for x in instructions): return data
    words = list(header)
    for x in instructions:
        if opcode(x) != 17 or x[1] != 11: words.extend(x)
    return struct.pack(f'<{len(words)}I', *words)


def global_memory_fields(binding):
    kind = binding['kind']
    if kind not in ('gmem_map', 'gmem_log'): return ()
    if binding['type'] != 'ssbo' or binding['count'] != 1 or type(binding.get('rw')) is not bool:
        raise ValueError('invalid global-memory storage resource')
    if kind == 'gmem_log':
        if not binding['rw'] or binding.get('read_only'):
            raise ValueError('global-memory write log must be writable')
        return ()
    # The map itself is read-only. Its shared reflection's rw flag describes
    # accesses through the physical pointers, which ordinary SSBO flags miss.
    return (f"gmem_write={int(binding['rw'])}",)
