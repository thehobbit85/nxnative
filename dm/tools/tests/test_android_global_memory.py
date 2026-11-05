"""Raw guest addresses retain the shared pool map and write-log resources."""
import unittest
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from dm.android.tools import spv_android as vk
from dm.android.tools.spv_module import decode, opcode
from dm.android.tests.global_memory_fixture import generate, VARIANTS


class GlobalMemoryExport(unittest.TestCase):
    def test_pool_map_and_write_log_are_distinct_storage_resources(self):
        bindings = [dict(binding=4, type='ssbo', count=1, kind='gmem_map', read_only=True, rw=True),
                    dict(binding=5, type='ssbo', count=1, kind='gmem_log', rw=True)]
        text = vk.vk_txt(dict(stage='cs', bindings=bindings, capabilities=[5347]))
        self.assertIn('cap 5347\n', text)
        self.assertIn('b 4 ssbo 1 gmem_map -1 -1 - -1 ro gmem_write=1\n', text)
        self.assertIn('b 5 ssbo 1 gmem_log -1 -1 - -1\n', text)

    def test_read_only_raw_access_and_invalid_resources(self):
        binding = dict(binding=0, type='ssbo', count=1, kind='gmem_map', rw=False)
        self.assertIn('gmem_write=0\n', vk.vk_txt(dict(stage='cs', bindings=[binding])))
        for changes in ({'type': 'ubo'}, {'count': 2}, {'rw': 1}, {'kind': 'gmem_log'},
                        {'kind': 'gmem_log', 'rw': True, 'read_only': True}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                vk.vk_txt(dict(stage='cs', bindings=[dict(binding, **changes)]))


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE') and all(shutil.which(t) for t in ('spirv-opt', 'spirv-val')),
                     'requires the locked translator and SPIRV-Tools')
class TranslatedGlobalMemory(unittest.TestCase):
    def test_all_supported_raw_address_families_preserve_shared_resources(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp); export = generate(work)
            for index, (operation, _) in enumerate(VARIANTS):
                key = f'{0xfeed5000+index:016x}'
                reflection = json.loads((work / (key + '.reflection.json')).read_text())
                maps = [b for b in reflection['bindings'] if b['kind'] == 'gmem_map']
                logs = [b for b in reflection['bindings'] if b['kind'] == 'gmem_log']
                with self.subTest(operation=VARIANTS[index]):
                    self.assertEqual(len(maps), 1)
                    self.assertEqual(maps[0]['rw'], operation != 'load')
                    self.assertEqual(len(logs), int(operation != 'load'))
                    text = (export / (key + '.vk.txt')).read_text()
                    self.assertIn(f'gmem_write={int(operation != "load")}\n', text)
                    self.assertIn('cap 5347\n', text)
                    self.assertNotIn('cap 11\n', text)  # pointer words need no integer64 arithmetic
                    self.assertIn('ext SPV_KHR_physical_storage_buffer\n', text)


@unittest.skipUnless(all(shutil.which(t) for t in ('spirv-as', 'spirv-opt', 'spirv-val')), 'requires SPIRV-Tools')
class AddressWordLowering(unittest.TestCase):
    def test_real_integer64_operations_keep_their_feature_requirement(self):
        assembly = '''OpCapability Shader
OpCapability Int64
OpCapability PhysicalStorageBufferAddresses
OpExtension "SPV_KHR_physical_storage_buffer"
OpMemoryModel PhysicalStorageBuffer64 GLSL450
OpEntryPoint GLCompute %main "main"
OpExecutionMode %main LocalSize 1 1 1
OpDecorate %block Block
OpMemberDecorate %block 0 Offset 0
OpMemberDecorate %block 1 Offset 4
OpMemberDecorate %block 2 Offset 8
OpDecorate %buffer DescriptorSet 0
OpDecorate %buffer Binding 0
%void = OpTypeVoid
%function = OpTypeFunction %void
%uint = OpTypeInt 32 0
%wide = OpTypeInt 64 0
%pair = OpTypeVector %uint 2
%block = OpTypeStruct %uint %uint %uint
%ssbo = OpTypePointer StorageBuffer %block
%word = OpTypePointer StorageBuffer %uint
%physical = OpTypePointer PhysicalStorageBuffer %uint
%zero = OpConstant %uint 0
%one = OpConstant %uint 1
%two = OpConstant %uint 2
%wide_one = OpConstant %wide 1
%buffer = OpVariable %ssbo StorageBuffer
%main = OpFunction %void None %function
%entry = OpLabel
%low_ptr = OpAccessChain %word %buffer %zero
%high_ptr = OpAccessChain %word %buffer %one
%out_ptr = OpAccessChain %word %buffer %two
%low = OpLoad %uint %low_ptr
%high = OpLoad %uint %high_ptr
%words = OpCompositeConstruct %pair %low %high
%address = OpBitcast %wide %words
ADDRESS
%pointer = OpConvertUToPtr %physical SOURCE
%loaded = OpLoad %uint %pointer Aligned 4
RESULT
OpStore %out_ptr %result
OpReturn
OpFunctionEnd
'''
        with tempfile.TemporaryDirectory() as tmp:
            for mode in ('pointer_only', 'integer_arithmetic', 'noncanonical_address'):
                address = '%offset = OpIAdd %wide %address %wide_one' if mode == 'noncanonical_address' else ''
                result = ('%sum = OpIAdd %wide %address %wide_one\n%narrow = OpUConvert %uint %sum\n%result = OpIAdd %uint %loaded %narrow'
                          if mode == 'integer_arithmetic' else '%result = OpCopyObject %uint %loaded')
                source = assembly.replace('ADDRESS', address).replace('SOURCE', '%offset' if address else '%address').replace('RESULT', result)
                binary = Path(tmp) / (mode + '.spv')
                subprocess.run(['spirv-as', '--target-env', 'vulkan1.1', '-o', binary, '-'], input=source, text=True, check=True)
                data = vk.export_spirv(binary.read_bytes(), shutil.which('spirv-opt'))
                subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', '-'], input=data, check=True)
                with self.subTest(mode=mode):
                    self.assertEqual(11 in vk.requirements(data)['capabilities'], mode != 'pointer_only')
                    self.assertEqual(any(opcode(x) == 120 for x in decode(data)[1]), mode == 'noncanonical_address')


if __name__ == '__main__': unittest.main()
