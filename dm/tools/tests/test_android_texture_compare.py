"""Depth sample/gather exports and the bounded filtered comparison oracle."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
from dm.android.tests.texture_compare_fixture import generate
from dm.android.tools import spv_android as vk


class TextureCompareOracle(unittest.TestCase):
    @unittest.skipUnless(shutil.which('cc'), 'requires a C compiler')
    def test_only_fractional_filtered_scalar_results_allow_a_small_error(self):
        source = r'''
#include "dm/android/tests/texture_compare_cases.h"
int main(void) {
 if(!tc_matches(1,tc_bits(.5f+0x1p-13f),tc_bits(.5f)))return 1;
 if(tc_matches(1,tc_bits(.5f+0x1p-11f),tc_bits(.5f)))return 2;
 if(tc_matches(0,tc_bits(.5f+0x1p-13f),tc_bits(.5f)))return 3;
 if(tc_matches(1,tc_bits(0x1p-13f),tc_bits(0)))return 4;
 if(tc_matches(1,tc_bits(1-0x1p-13f),tc_bits(1)))return 5;
 if(tc_matches(1,tc_bits(NAN),tc_bits(.5f)))return 6;
 if(tc_matches(1,tc_bits(INFINITY),tc_bits(.5f)))return 7;
 if(tc_matches(1,tc_bits(-.1f),tc_bits(.5f)))return 8;
 if(tc_matches(1,tc_bits(1.1f),tc_bits(.5f)))return 9;
 if(!tc_matches(0,0xcccccccc,0xcccccccc))return 10;
 if(tc_matches(0,0xcccccccd,0xcccccccc))return 11;
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'oracle.c';path.write_text(source);binary=Path(tmp)/'oracle'
            subprocess.run(['cc','-std=c11','-Werror=implicit-function-declaration','-I',str(ROOT),str(path),'-lm','-o',str(binary)],check=True,capture_output=True)
            subprocess.run([str(binary)],check=True,capture_output=True)


@unittest.skipUnless(shutil.which('spirv-val') and shutil.which('spirv-opt'), 'requires SPIR-V tools')
class TextureCompareExports(unittest.TestCase):
    def test_actual_guest_depth_sample_and_gather(self):
        with tempfile.TemporaryDirectory() as tmp:
            out=generate(Path(tmp))
            for variant in range(4):
                key=f'{0xfeed9400+variant:016x}'
                code=vk.decode((out/(key+'.spv')).read_bytes())[1]
                opcode=97 if variant%2 else 90
                operations=[i for i in code if vk.opcode(i)==opcode]
                self.assertEqual(len(operations),1 if variant%2 else 256)
                self.assertIn(f'view={5 if variant//2 else 1} num=0',(out/(key+'.vk.txt')).read_text())
                self.assertFalse(any(vk.opcode(i)==96 for i in code))  # no noncompare gather


if __name__ == '__main__': unittest.main()
