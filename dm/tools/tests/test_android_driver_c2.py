"""C2 byte boundaries and image/combined overlap follow the shared guest ABI."""
import ctypes
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


@unittest.skipUnless(shutil.which('cc'), 'requires C compiler')
class DriverTable(unittest.TestCase):
    def test_boundaries_padding_and_image_priority(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp)/'c2.c'; library = source.with_suffix('.so')
            source.write_text('#include "dm/gfx/nvn/vulkan/driver_c2.h"\n'
                              'void fill(void *out,size_t n,const uint64_t*t,const uint64_t*i,const uint64_t*s){dm_vk_c2_fill(out,n,t,i,s);}'
                              'uint64_t read_word(const uint64_t*t,const uint64_t*i,const uint64_t*s,uint64_t off){return dm_vk_c2_handle(t,i,s,off);}')
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-shared','-fPIC','-I',str(ROOT),source,'-o',library],
                           capture_output=True,check=True)
            module = ctypes.CDLL(str(library)); word_ptr = ctypes.POINTER(ctypes.c_uint64)
            module.fill.argtypes = [ctypes.c_void_p,ctypes.c_size_t,word_ptr,word_ptr,word_ptr]
            module.fill.restype = None
            module.read_word.argtypes = [word_ptr,word_ptr,word_ptr,ctypes.c_uint64]; module.read_word.restype = ctypes.c_uint64
            textures = (ctypes.c_uint64*256)(*[1000+i for i in range(256)])
            images = (ctypes.c_uint64*32)(*[2000+i if i%2 else 0 for i in range(32)])
            samplers = (ctypes.c_uint64*128)(*[(300+i)<<20 for i in range(128)])
            expected = {}
            for slot in range(41): expected[0x20+8*slot] = 2000+slot-32 if 32 <= slot < 40 and slot%2 else 1000+slot
            for slot in range(128):
                expected[0x168+8*slot] = 1128+slot
                expected[0x568+8*slot] = (300+slot)<<20
            for offset in range(0xa08):
                self.assertEqual(module.read_word(textures,images,samplers,offset),expected.get(offset,0),hex(offset))
            self.assertEqual(module.read_word(textures,images,samplers,2**64-1),0)
            for size in (0,1,31,32,33,35,36,0x120,0x123,0x124,0x160,0x164,0x168,0x56c,0x964,0xa00,4096):
                output = (ctypes.c_uint8*(size+8))(*([0xa5]*(size+8))); want = bytearray(size)
                for offset,value in expected.items():
                    if offset+4 <= size: struct.pack_into('<I',want,offset,value)
                module.fill(ctypes.byref(output,4),size,textures,images,samplers)
                self.assertEqual(bytes(output),b'\xa5'*4+want+b'\xa5'*4, f'size {size}')


if __name__ == '__main__': unittest.main()
