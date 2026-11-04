"""Validate the shader module and bindings consumed by the Android Vulkan executor."""
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'android/tools'))
import spv_android as vk


# The same typed image load/write pattern as the two Legends Z-A compute shaders.
COMPUTE = """
OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main"
OpExecutionMode %main LocalSize 1 1 1
OpName %dst "output_image"
OpDecorate %dst DescriptorSet 0
OpDecorate %dst Binding 7
%void = OpTypeVoid
%fn = OpTypeFunction %void
%f = OpTypeFloat 32
%i = OpTypeInt 32 1
%i2 = OpTypeVector %i 2
%f4 = OpTypeVector %f 4
%img = OpTypeImage %f 2D 0 0 0 2 Unknown
%ptr = OpTypePointer UniformConstant %img
%dst = OpVariable %ptr UniformConstant
%coord = OpConstantNull %i2
%color = OpConstantNull %f4
%main = OpFunction %void None %fn
%start = OpLabel
%image = OpLoad %img %dst
OpImageWrite %image %coord %color
OpReturn
OpFunctionEnd
"""


class AndroidShaderTests(unittest.TestCase):
    def test_packaged_geometry_stage_is_preserved_without_claiming_runtime_support(self):
        for stage in ('gs', 'hs', 'ds'):
            self.assertEqual(vk.vk_txt({'stage': stage, 'bindings': []}), f'stage {stage}\n')
        with self.assertRaisesRegex(ValueError, 'stage'):
            vk.vk_txt({'stage': 'invalid', 'bindings': []})

    @unittest.skipUnless(shutil.which('spirv-as') and shutil.which('spirv-val'), 'requires SPIRV-Tools')
    def test_storage_image_contract(self):
        with tempfile.TemporaryDirectory() as tmp:
            spv = Path(tmp) / 'stage.spv'
            for image_format in ('Unknown', 'Rgba32f'):
                subprocess.run(['spirv-as', '--target-env', 'vulkan1.1', '-o', spv, '-'],
                               input=COMPUTE.replace('Unknown', image_format), text=True, check=True)
                raw = spv.read_bytes()
                prepared = vk.prepare_spirv(raw)
                caps = [ins[1] for ins in vk.decode(prepared)[1] if vk.opcode(ins) == 17]
                self.assertEqual(56 in caps, image_format == 'Unknown')
                self.assertEqual(vk.prepare_spirv(prepared), prepared)
                spv.write_bytes(prepared)
                subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', spv], check=True)
                refl = {'bindings': [{'msl_name': 'output_image', 'kind': 'img', 'reg': 3}]}
                binds = vk.vk_reflect(prepared, refl)
                sidecar = vk.vk_txt({'stage': 'cs', 'tg': [1, 1, 1], 'bindings': binds})
                self.assertEqual(sidecar, 'stage cs\ntg 1 1 1\nb 7 simg 1 img 3 -1 - -1\n')
                with self.assertRaisesRegex(ValueError, 'no guest resource'):
                    vk.vk_reflect(prepared, {'bindings': []})

    @unittest.skipUnless(shutil.which('spirv-as') and shutil.which('spirv-val'), 'requires SPIRV-Tools')
    def test_readonly_image_keeps_guest_ownership(self):
        source = COMPUTE.replace('Unknown', 'Rgba32f').replace('OpImageWrite %image %coord %color',
                              '%sample = OpImageRead %f4 %image %coord')
        source = source.replace('OpDecorate %dst Binding 7', 'OpDecorate %dst Binding 7\nOpDecorate %dst NonWritable')
        with tempfile.TemporaryDirectory() as tmp:
            spv = Path(tmp) / 'read.spv'
            subprocess.run(['spirv-as', '--target-env', 'vulkan1.1', '-o', spv, '-'], input=source, text=True, check=True)
            subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', spv], check=True)
            bindings = vk.vk_reflect(spv.read_bytes(), {'bindings': [{'msl_name': 'output_image', 'kind': 'img', 'reg': 3}]})
            self.assertIn('b 7 simg 1 img 3 -1 - -1 ro\n', vk.vk_txt({'stage': 'cs', 'bindings': bindings}))

    def test_driver_constant_binding_uses_guest_bank(self):
        binding = {'binding': 1, 'type': 'ubo', 'count': 1, 'kind': 'drvcb', 'bank': 24, 'off': 0}
        self.assertIn('b 1 ubo 1 drvcb -1 24 - -1', vk.vk_txt({'stage': 'cs', 'bindings': [binding]}))

    def test_keep_required_optional_capabilities(self):
        hdr = [0x07230203, 0x10300, 0, 20, 0]
        caps = [2 << 16 | 17, 10, 2 << 16 | 17, 5378]
        use = [3 << 16 | 22, 1, 64, 1 << 16 | 5364, 1 << 16 | 5365]
        for operations in ([], use):
            words = hdr + caps + operations
            raw = struct.pack(f'<{len(words)}I', *words)
            _, result = vk.decode(vk.prepare_spirv(raw))
            self.assertEqual([x[1] for x in result if vk.opcode(x) == 17], [10, 5378] if operations else [])

    def test_invalid_instruction_word_counts(self):
        hdr = struct.pack('<5I', 0x07230203, 0x10300, 0, 1, 0)
        for word in (0, 3 << 16):
            with self.assertRaisesRegex(ValueError, 'invalid SPIR-V instruction'):
                vk.decode(hdr + struct.pack('<I', word))


if __name__ == '__main__':
    unittest.main()
