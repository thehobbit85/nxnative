"""Bounded SPIR-V decoding shared by Android export and capture lowering."""
import struct


def decode(data):
    if len(data) < 20 or len(data) % 4:
        raise ValueError('truncated SPIR-V module')
    words = struct.unpack(f'<{len(data) // 4}I', data)
    if words[0] != 0x07230203:
        raise ValueError('invalid SPIR-V magic')
    if words[1] not in (0x10000, 0x10100, 0x10200, 0x10300):
        raise ValueError('SPIR-V version is incompatible with Vulkan 1.1')
    at = 5
    instructions = []
    while at < len(words):
        count = words[at] >> 16
        if not count or at + count > len(words):
            raise ValueError(f'invalid SPIR-V instruction at word {at}')
        instructions.append(words[at:at + count])
        at += count
    return words[:5], instructions


def opcode(instruction):
    return instruction[0] & 0xffff


def string_operand(words):
    return struct.pack(f'<{len(words)}I', *words).split(b'\0', 1)[0].decode()
