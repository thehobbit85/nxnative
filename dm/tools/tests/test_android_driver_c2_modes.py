"""Contextual C2 tables and logical slot fallback follow the Apple guest ABI."""
import ctypes
from pathlib import Path
import random
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class ContextualDriverTable(unittest.TestCase):
    def test_modes_and_logical_slots(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / 'modes.c'; library = source.with_suffix('.so')
            source.write_text('#include "dm/gfx/nvn/vulkan/driver_c2_modes.h"\n'
                              'int fill(void*o,size_t n,const uint64_t*t,const uint64_t*i,const uint64_t*s,unsigned mode,unsigned tb,unsigned sb)'
                              '{return dm_vk_c2_fill_mode(o,n,t,i,s,mode,tb,sb);}'
                              'uint64_t texture(const uint64_t*t,unsigned k){return dm_vk_slot_texture(t,k);}'
                              'uint64_t sampler(const uint64_t*t,const uint64_t*s,unsigned k){return dm_vk_slot_sampler(t,s,k);}')
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC', '-I', str(ROOT), source, '-o', library], check=True, capture_output=True)
            lib = ctypes.CDLL(str(library)); words = ctypes.POINTER(ctypes.c_uint64)
            lib.fill.argtypes = [ctypes.c_void_p, ctypes.c_size_t, words, words, words, ctypes.c_uint, ctypes.c_uint, ctypes.c_uint]
            lib.fill.restype = ctypes.c_int
            lib.texture.argtypes = [words, ctypes.c_uint]; lib.texture.restype = ctypes.c_uint64
            lib.sampler.argtypes = [words, words, ctypes.c_uint]; lib.sampler.restype = ctypes.c_uint64
            rng = random.Random(0xc2)
            for iteration in range(16):
                tb, sb = iteration * 101, iteration * 7
                def handle(tb=tb, sb=sb):
                    return (tb + rng.randrange(1, 500) if rng.randrange(3) else 0) | ((sb + rng.randrange(1, 100) if rng.randrange(3) else 0) << 20)
                tx = (ctypes.c_uint64 * 256)(*[handle() if rng.randrange(4) else 0 for _ in range(256)])
                im = (ctypes.c_uint64 * 8)(*[handle() if i % 2 else 0 for i in range(8)])
                sa = (ctypes.c_uint64 * 128)(*[(sb + rng.randrange(1, 100)) << 20 if rng.randrange(3) else 0 for _ in range(128)])
                for slot in range(34):
                    self.assertEqual(lib.texture(tx, slot), (tx[slot] or tx[128 + slot]) if slot < 32 else 0)
                    self.assertEqual(lib.sampler(tx, sa, slot), ((sa[slot] >> 20) or (tx[slot] >> 20)) if slot < 32 else 0)
                for mode in range(3):
                    expected = bytearray(4096)
                    def put(offset, value, expected=expected): struct.pack_into('<I', expected, offset, value & 0xffffffff)
                    def local(value, tb=tb, sb=sb):
                        texture, sampler = value & 0xfffff, (value >> 20) & 0xfff
                        return (texture - tb if texture else 0) | ((sampler - sb if sampler else 0) << 20)
                    for r in range(8): put(0x120 + 8*r, im[r])
                    for r in range(41):
                        if r >= 32 and (not tx[r] or (r < 40 and im[r-32])): continue
                        value = tx[r] if mode == 0 else local(tx[r]) if mode == 2 else (r | (r << 20 if tx[r] >> 20 else 0)) if tx[r] else 0
                        put(0x20 + 8*r, value)
                    for r in range(128):
                        h = tx[128+r]; put(0x168 + 8*r, h if mode == 0 else local(h) if mode == 2 else r if h else 0)
                        h = sa[r]; put(0x568 + 8*r, h if mode == 0 else local(h) if mode == 2 else r << 20 if h else 0)
                    for size in (0, 35, 36, 0x123, 0x124, 0x16b, 0x16c, 0x56b, 0x56c, 4096):
                        out = (ctypes.c_ubyte * (size+8))(*([0xa5] * (size+8)))
                        self.assertEqual(lib.fill(ctypes.byref(out,4),size,tx,im,sa,mode,tb,sb),1)
                        want = bytearray(expected[:size])
                        # Each C2 entry is a 32-bit word in an eight-byte row.
                        for offset in range(0x20, 0x968, 8):
                            if offset < size < offset+4: want[offset:] = b'\0'*(size-offset)
                        self.assertEqual(bytes(out), b'\xa5'*4 + want + b'\xa5'*4, (iteration,mode,size))
                out = (ctypes.c_ubyte * 16)(*([0xa5]*16))
                self.assertEqual(lib.fill(out,16,tx,im,sa,3,tb,sb),0)
                self.assertEqual(bytes(out), b'\xa5'*16)


if __name__ == '__main__': unittest.main()
