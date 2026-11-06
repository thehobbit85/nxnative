"""Vulkan 1.1 requires separate capabilities for unformatted image reads/writes."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
from dm.android.tools import spv_android as vk


def assembly(fmt, read, write, indirect=False, *, array=False):
    operations = '%v = OpImageRead %four %im %coord\n' if read else ''
    operations += ('OpImageWrite %im %coord ' + ('%v' if read else '%zero4') + '\n') if write else ''
    source = '''OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main"
OpExecutionMode %main LocalSize 1 1 1
OpDecorate %bound DescriptorSet 0
OpDecorate %bound Binding 0
%void = OpTypeVoid
%u = OpTypeInt 32 0
%f = OpTypeFloat 32
%four = OpTypeVector %f 4
%two = OpTypeVector %u 2
%u0 = OpConstant %u 0
%f0 = OpConstant %f 0
%zero4 = OpConstantComposite %four %f0 %f0 %f0 %f0
%coord = OpConstantComposite %two %u0 %u0
%img = OpTypeImage %f 2D 0 0 0 2 ''' + fmt + '''
%ptr = OpTypePointer UniformConstant %img
%bound = OpVariable %ptr UniformConstant
%ft = OpTypeFunction %void
%ht = OpTypeFunction %void %img
%main = OpFunction %void None %ft
%entry = OpLabel
''' + ('''%loaded = OpLoad %img %bound
%call = OpFunctionCall %void %helper %loaded
OpReturn
OpFunctionEnd
%helper = OpFunction %void None %ht
%im = OpFunctionParameter %img
%hentry = OpLabel
''' if indirect else '%im = OpLoad %img %bound\n') + operations + 'OpReturn\nOpFunctionEnd\n'
    if array:
        source = source.replace('%ptr = OpTypePointer UniformConstant %img', '%n = OpConstant %u 2\n%array = OpTypeArray %img %n\n%ptr = OpTypePointer UniformConstant %array')
        source = source.replace('%im = OpLoad %img %bound', '%all = OpLoad %array %bound\n%im = OpCompositeExtract %img %all 0')
    return source


@unittest.skipUnless(shutil.which('spirv-as') and shutil.which('spirv-val'), 'requires SPIR-V tools')
class ImageAccess(unittest.TestCase):
    def prepared(self, source):
        raw = subprocess.run(['spirv-as','--target-env','vulkan1.1','-','-o','-'],input=source.encode(),stdout=subprocess.PIPE,check=True).stdout
        fixed = vk.prepare_spirv(raw)
        subprocess.run(['spirv-val','--target-env','vulkan1.1','-'],input=fixed,check=True)
        return vk.requirements(fixed)['capabilities']

    def test_array_extracted_image_access(self):
        for read, write in ((True, False), (False, True), (True, True)):
            caps = self.prepared(assembly('Unknown',read,write,array=True))
            self.assertEqual((55 in caps, 56 in caps),(read,write))

    def test_subpass_input_does_not_require_storage_read(self):
        source = assembly('Unknown',True,False).replace('OpCapability Shader', 'OpCapability Shader\nOpCapability InputAttachment')
        source = source.replace('GLCompute','Fragment').replace('LocalSize 1 1 1','OriginUpperLeft').replace('OpTypeImage %f 2D','OpTypeImage %f SubpassData')
        source = source.replace('OpDecorate %bound Binding 0', 'OpDecorate %bound Binding 0\nOpDecorate %bound InputAttachmentIndex 0')
        self.assertEqual(self.prepared(source), [1,40])

    def test_capability_follows_the_actual_operation_and_image_type(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp)
            for fmt in ('Unknown', 'R32f', 'Rg32f'):
                for read, write in ((False,False), (True,False), (False,True), (True,True)):
                    for indirect in (False,True):
                        with self.subTest(fmt=fmt, read=read, write=write, indirect=indirect):
                            raw = subprocess.run(['spirv-as','--target-env','vulkan1.1','-','-o','-'],input=assembly(fmt,read,write,indirect).encode(),stdout=subprocess.PIPE,check=True).stdout
                            fixed = vk.prepare_spirv(raw); caps = vk.requirements(fixed)['capabilities']
                            self.assertEqual(55 in caps, fmt=='Unknown' and read)
                            self.assertEqual(56 in caps, fmt=='Unknown' and write)
                            self.assertEqual(49 in caps, fmt=='Rg32f')
                            self.assertEqual(vk.prepare_spirv(fixed), fixed)
                            file = p/'image.spv'; file.write_bytes(fixed)
                            subprocess.run(['spirv-val','--target-env','vulkan1.1',file],check=True)

    @unittest.skipUnless(shutil.which('spirv-opt'), 'requires SPIR-V optimizer')
    def test_maxwell_surface_exports(self):
        from dm.android.tests.storage_surface_fixture import generate
        with tempfile.TemporaryDirectory() as tmp:
            out = generate(Path(tmp))
            stages = sorted(out.glob('*.spv'))
            self.assertEqual(len(stages), 8)
            for i, path in enumerate(stages):
                with self.subTest(key=path.stem):
                    code = path.read_bytes()
                    ops = {vk.opcode(x) for x in vk.decode(code)[1]}
                    self.assertTrue({98, 99} <= ops)  # OpImageRead and OpImageWrite
                    expected = {55, 56} if i == 0 else {49} if i in (2, 4, 5, 6, 7) else set()
                    caps = set(vk.requirements(code)['capabilities'])
                    self.assertEqual(caps & {49, 55, 56}, expected)
                    sidecar = path.with_suffix('.vk.txt').read_text().splitlines()
                    self.assertEqual({int(s[4:]) for s in sidecar if s.startswith('cap ')}, caps)


if __name__ == '__main__': unittest.main()
