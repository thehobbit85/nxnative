"""C0 stage tables, draw values and bounded sample metadata use the guest ABI."""
import ctypes
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class Binding(ctypes.Structure):
    _fields_ = [('stage', ctypes.c_int32), ('index', ctypes.c_int32),
                ('addr', ctypes.c_uint64), ('size', ctypes.c_uint64)]


@unittest.skipUnless(shutil.which('cc'), 'requires C compiler')
class DriverConstants(unittest.TestCase):
    def test_stage_tables_draw_values_sample_rows_and_guards(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp)/'c0.c'; library = source.with_suffix('.so')
            source.write_text('#include "dm/gfx/nvn/vulkan/driver_c0.h"\n'
                              'int fill(void*d,size_t n,int stage,const p_bind_buf*s,const p_bind_buf*u,'
                              'const uint32_t*g,uint32_t b,const uint8_t*p,unsigned k,int v)'
                              '{return dm_vk_c0_fill(d,n,stage,s,u,g,b,p,k,v);}')
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-shared','-fPIC',
                            '-I',str(ROOT),'-I',str(ROOT/'dm/gfx/nvn/include'),source,'-o',library],
                           capture_output=True,check=True)
            module = ctypes.CDLL(str(library))
            module.fill.argtypes = [ctypes.c_void_p,ctypes.c_size_t,ctypes.c_int,
                                    ctypes.POINTER(Binding),ctypes.POINTER(Binding),
                                    ctypes.POINTER(ctypes.c_uint32),ctypes.c_uint32,
                                    ctypes.POINTER(ctypes.c_uint8),ctypes.c_uint,ctypes.c_int]
            module.fill.restype = ctypes.c_int
            storage = (Binding*16)(*[Binding(0,i,0x1234567800000000+0x100*i,0x100000020+i) for i in range(16)])
            uniform = (Binding*16)(*[Binding(5,i,0xabcd000000000000+0x1000*i,0x200000040+i) for i in range(16)])
            dispatch = (ctypes.c_uint32*3)(3,7,11)
            positions = (ctypes.c_uint8*16)(*[0x80+i for i in range(16)])
            # Literal offsets are independent of the production header's map.
            for stage,base in enumerate((0x110,0x510,0x410,0x670,0x770,0x310)):
                for nvn2 in (0,1):
                    for samples in (1,2,4,8,16):
                        size = 4096; output = (ctypes.c_uint8*(size+8))(*([0xa5]*(size+8)))
                        expected = bytearray(size)
                        for r in range(16):
                            struct.pack_into('<QI',expected,base+16*r,storage[r].addr,storage[r].size&0xffffffff)
                        if stage == 5:
                            struct.pack_into('<3I',expected,0x200,3,7,11)
                            if nvn2: struct.pack_into('<3I',expected,4,3,7,11)
                            for r in range(16):
                                struct.pack_into('<QI',expected,0x210+16*r,uniform[r].addr,uniform[r].size&0xffffffff)
                        else:
                            struct.pack_into('<f',expected,0x610,2047)
                            struct.pack_into('<I',expected,0x644,0x40110010)
                            if stage == 1:
                                expected[0x620:0x620+samples] = bytes(positions)[:samples]
                                struct.pack_into('<4I',expected,0x630,samples-1,samples.bit_length()-1,0,0)
                        self.assertEqual(module.fill(ctypes.byref(output,4),size,stage,storage,uniform,
                                                     dispatch,0x40110010,positions,samples,nvn2),1)
                        self.assertEqual(bytes(output),b'\xa5'*4+expected+b'\xa5'*4,(stage,nvn2,samples))
            for stage,size,samples in ((-1,4096,1),(6,4096,1),(0,0,1),(5,0x9ff,1),
                                       (1,4096,0),(1,4096,3),(1,4096,32)):
                output = (ctypes.c_uint8*4104)(*([0xa5]*4104))
                self.assertEqual(module.fill(ctypes.byref(output,4),size,stage,storage,uniform,
                                             dispatch,0,positions,samples,0),0)
                self.assertEqual(bytes(output),b'\xa5'*4104)


if __name__ == '__main__': unittest.main()
