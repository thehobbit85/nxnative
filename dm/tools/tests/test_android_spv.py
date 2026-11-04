"""Validate the shader module and bindings consumed by the Android Vulkan executor."""
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

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
    @unittest.skipUnless(shutil.which('glslc') and shutil.which('spirv-val'), 'requires glslc and SPIRV-Tools')
    def test_typed_buffer_numeric_classes_and_readonly_storage(self):
        source = Path(__file__).resolve().parents[2] / 'gfx/nvn/vulkan/tests/texel.comp'
        resources = [('floats', 'tbuf', 0), ('integers', 'tbuf', 2), ('unsignedValues', 'tex', 3),
                     ('destination', 'img', 0), ('previous', 'img', 1), ('alias', 'sbuf', 0),
                     ('result', 'sbuf', 1), ('constants', 'drvcb', -1)]
        reflection = {'bindings': [dict(msl_name=name, kind=kind, reg=reg) for name, kind, reg in resources]}
        with tempfile.TemporaryDirectory() as tmp:
            spv = Path(tmp) / 'texel.spv'
            subprocess.run(['glslc', '--target-env=vulkan1.1', source, '-o', spv], check=True)
            subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', spv], check=True)
            binds = vk.vk_reflect(spv.read_bytes(), reflection)
        self.assertEqual([b['type'] for b in binds[:5]], ['tbuf', 'tbuf', 'utb', 'stb', 'stb'])
        self.assertEqual([b['num'] for b in binds[:5]], [0, 1, 2, 2, 2])
        self.assertEqual(binds[0]['count'], 2)
        self.assertNotIn('read_only', binds[3])
        self.assertTrue(binds[4]['read_only'])

    @unittest.skipUnless(shutil.which('glslc') and shutil.which('spirv-val'), 'requires glslc and SPIRV-Tools')
    def test_separate_texture_sampler_arrays_export_guest_sources(self):
        source = Path(__file__).resolve().parents[2] / 'gfx/nvn/vulkan/tests/separate.frag'
        reflection = {'bindings': [dict(msl_name='images', kind='tex_ubo', reg=7, off=16),
                                   dict(msl_name='samplers', kind='samp_ubo', reg=8, off=24),
                                   dict(msl_name='p', kind='cbuf', reg=9)]}
        with tempfile.TemporaryDirectory() as tmp:
            spv = Path(tmp) / 'separate.spv'
            for numeric, prefix in enumerate(('', 'i', 'u')):
                text = source.read_text().replace('texture3D images', prefix + 'texture3D images')
                text = text.replace('sampler3D(', prefix + 'sampler3D(').replace('vec4 color', prefix + 'vec4 color')
                subprocess.run(['glslc', '-fshader-stage=fragment', '--target-env=vulkan1.1', '-', '-o', spv], input=text, text=True, check=True)
                subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', spv], check=True)
                binds = vk.vk_reflect(spv.read_bytes(), reflection)
                self.assertEqual((binds[0]['type'], binds[0]['count'], binds[0]['view'], binds[0]['num']), ('tex', 2, 2, numeric))
                self.assertEqual((binds[1]['type'], binds[1]['kind'], binds[1]['count']), ('samp', 'samp_ubo', 2))
                self.assertIn('b 1 samp 2 samp_ubo 8 24 - -1', vk.vk_txt(dict(stage='ps', bindings=binds)))

    @unittest.skipUnless(shutil.which('glslc') and shutil.which('spirv-val'), 'requires glslc and SPIRV-Tools')
    def test_storage_readonly_requires_all_block_members(self):
        source = '''#version 450
layout(local_size_x=1) in;
layout(set=0,binding=0,std430) readonly buffer Input { uint count; uint words[]; } src;
layout(set=0,binding=1,std430) buffer Output { uint words[]; } dst;
void main() { dst.words[0]=src.words[src.count]; }
'''
        reflection = {'bindings': [dict(msl_name='src', kind='sbuf', reg=0), dict(msl_name='dst', kind='sbuf', reg=1)]}
        with tempfile.TemporaryDirectory() as tmp:
            spv = Path(tmp) / 'storage.spv'
            subprocess.run(['glslc', '-fshader-stage=compute', '--target-env=vulkan1.1', '-', '-o', spv], input=source, text=True, check=True)
            raw = spv.read_bytes()
            header, instructions = vk.decode(raw)
            for partial in (False, True):
                words = list(header)
                for instruction in instructions:
                    if partial and vk.opcode(instruction) == 72 and instruction[2:4] == (1, 24):
                        continue  # only one member remains NonWritable
                    words.extend(instruction)
                data = struct.pack(f'<{len(words)}I', *words)
                spv.write_bytes(data)
                subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', spv], check=True)
                binds = vk.vk_reflect(data, reflection)
                self.assertEqual(binds[0].get('read_only', False), not partial)
                self.assertFalse(binds[1].get('read_only', False))

    @unittest.skipUnless(shutil.which('spirv-as') and shutil.which('spirv-val'), 'requires SPIRV-Tools')
    def test_sampled_array_reflection_retains_shape_and_numeric_class(self):
        source = '''
OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint Fragment %main "main" %out
OpExecutionMode %main OriginUpperLeft
OpName %images "images"
OpDecorate %images DescriptorSet 0
OpDecorate %images Binding 3
OpDecorate %out Location 0
%void = OpTypeVoid
%fn = OpTypeFunction %void
%float = OpTypeFloat 32
%uint = OpTypeInt 32 0
%component = COMPONENT
%value4 = OpTypeVector %component 4
%float3 = OpTypeVector %float 3
%zero = OpConstant %uint 0
%length = OpConstant %uint 32
%half = OpConstant %float 0.5
%coord = OpConstantComposite %float3 %half %half %half
%image = OpTypeImage %component 3D 0 0 0 1 Unknown
%sampled = OpTypeSampledImage %image
%array = OpTypeArray %sampled %length
%arrayptr = OpTypePointer UniformConstant %array
%elementptr = OpTypePointer UniformConstant %sampled
%images = OpVariable %arrayptr UniformConstant
%outptr = OpTypePointer Output %value4
%out = OpVariable %outptr Output
%main = OpFunction %void None %fn
%label = OpLabel
%element = OpAccessChain %elementptr %images %zero
%loaded = OpLoad %sampled %element
%color = OpImageSampleImplicitLod %value4 %loaded %coord
OpStore %out %color
OpReturn
OpFunctionEnd
'''
        reflection = {'bindings': [dict(msl_name='images', kind='tex_ubo', reg=7, off=352),
                                   dict(msl_name='imagesSmplr', kind='samp_ubo', reg=8, off=128)]}
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'array.spv'
            for numeric, component in enumerate(('OpTypeFloat 32', 'OpTypeInt 32 1', 'OpTypeInt 32 0')):
                # SPIR-V scalar types must be unique; reuse the coordinate/count
                # type when it is also the sampled component type.
                text = source.replace('COMPONENT', component)
                if numeric in (0, 2):
                    text = text.replace('%component = ' + component + '\n', '').replace('%component', '%float' if numeric == 0 else '%uint')
                subprocess.run(['spirv-as', '--target-env', 'vulkan1.1', '-o', path, '-'], input=text, text=True, check=True)
                subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', path], check=True)
                binding, = vk.vk_reflect(path.read_bytes(), reflection)
                self.assertEqual((binding['type'], binding['count'], binding['view'], binding['num']), ('cis', 32, 2, numeric))
                self.assertEqual(binding['samp']['off'], 128)

    def test_uniform_texture_array_preserves_independent_sampler_source(self):
        binding = dict(binding=3, type='cis', count=32, kind='tex_ubo', reg=7, off=352,
                       view=2, num=0, samp=dict(kind='samp_ubo', reg=8, off=128))
        text = vk.vk_txt(dict(stage='ps', bindings=[binding]))
        self.assertIn('b 3 cis 32 tex_ubo 7 352 samp_ubo 8', text)
        self.assertIn('soff=128', text)
        self.assertIn('view=2', text)
        self.assertIn('num=0', text)

    def test_retry_reuses_content_addressed_compiles_but_recaptures_spirv(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            seen = []
            def stop_before_translation(command, **kwargs):
                env = kwargs['env']
                seen.append(dict(env, metal_cache=str((Path(env['DMSH_CAS']) / 'metallib').resolve())))
                raise RuntimeError('translation boundary')
            for name in ('first', 'retry'):
                with mock.patch.object(vk.subprocess, 'run', side_effect=stop_before_translation), \
                     mock.patch.object(vk, 'host_contract', return_value=b'fixture producer'), \
                     mock.patch.object(vk.shutil, 'which', side_effect=lambda name: '/tools/' + name):
                    with self.assertRaisesRegex(RuntimeError, 'translation boundary'):
                        vk.export(root / 'pkg', ['0' * 16], root / name)
            self.assertNotEqual(seen[0]['DMSH_CAS'], seen[1]['DMSH_CAS'])
            self.assertEqual(seen[0]['metal_cache'], seen[1]['metal_cache'])
            self.assertEqual(Path(seen[0]['metal_cache']), (root / 'vk-metallib').resolve())
            self.assertEqual(Path(seen[0]['DMSH_PREFILL']), root / 'cache')
            self.assertNotEqual(seen[0]['DMSH_SPV_CAPTURE'], seen[1]['DMSH_SPV_CAPTURE'])

    @unittest.skipUnless(all(shutil.which(t) for t in ('spirv-as', 'spirv-val', 'spirv-opt')), 'requires SPIRV-Tools')
    def test_unused_descriptor_without_metal_metadata_is_removed(self):
        # The raw translator keeps unreferenced globals; SPIRV-Cross omits them
        # from Metal's argument buffer. Live bindings must keep their numbers.
        source = COMPUTE.replace('OpDecorate %dst Binding 7',
                                 'OpDecorate %dst Binding 7\nOpDecorate %unused DescriptorSet 0\n'
                                 'OpDecorate %unused Binding 1')
        source = source.replace('%dst = OpVariable %ptr UniformConstant',
                                '%dst = OpVariable %ptr UniformConstant\n%unused = OpVariable %ptr UniformConstant')
        source = source.replace('OpName %dst "output_image"',
                                'OpName %dst "output_image"\nOpName %unused "unused_image"')
        reflection = {'bindings': [{'msl_name': 'output_image', 'kind': 'img', 'reg': 3}]}
        with tempfile.TemporaryDirectory() as tmp:
            spv = Path(tmp) / 'unused.spv'
            subprocess.run(['spirv-as', '--target-env', 'vulkan1.1', '-o', spv, '-'],
                           input=source.replace('Unknown', 'Rgba32f'), text=True, check=True)
            raw = spv.read_bytes()
            with self.assertRaisesRegex(ValueError, 'unused_image'):
                vk.vk_reflect(raw, reflection)
            prepared = vk.export_spirv(raw, shutil.which('spirv-opt'))
            spv.write_bytes(prepared)
            subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', spv], check=True)
            bindings = vk.vk_reflect(prepared, reflection)
            self.assertEqual([(b['binding'], b['kind'], b['reg']) for b in bindings], [(7, 'img', 3)])
            self.assertTrue(any(vk.opcode(i) == 99 for i in vk.decode(prepared)[1]))
            with self.assertRaisesRegex(ValueError, 'output_image'):
                vk.vk_reflect(prepared, {'bindings': []})

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
