"""A workgroup barrier is only valid when all invocations reach the shuffle."""
from pathlib import Path
import shutil
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
from dm.android.tools import spv_warp
from dm.android.tools.spv_module import decode, opcode


def assembly(*, branch=False, existing_index=False, declared_index_only=False, specialize_size=False, shared=False, mask=None):
    extra = ' %index' if existing_index and not declared_index_only else ''
    annotations = 'OpDecorate %index BuiltIn LocalInvocationIndex\n' if existing_index else ''
    variables = '%index = OpVariable %input Input\n' if existing_index else ''
    capabilities = ''
    if mask:
        capabilities = 'OpCapability GroupNonUniformBallot\n'
        extra += ' %mask'
        annotations += 'OpDecorate %mask BuiltIn ' + mask + '\n'
        variables += '%vec = OpTypeVector %uint 4\n%mptr = OpTypePointer Input %vec\n%mask = OpVariable %mptr Input\n'
    if specialize_size:
        annotations += 'OpDecorate %size BuiltIn WorkgroupSize\nOpDecorate %count SpecId 0\n'
        variables += '%uvec3 = OpTypeVector %uint 3\n%count = OpSpecConstant %uint 64\n%size = OpSpecConstantComposite %uvec3 %count %one %one\n'
    if shared:
        variables += '%wg = OpTypePointer Workgroup %uint\n%scratch = OpVariable %wg Workgroup\n'
    before = '%b = OpIEqual %bool %id %one\nOpSelectionMerge %merge None\nOpBranchConditional %b %then %merge\n%then = OpLabel\n' if branch else ''
    after = 'OpBranch %merge\n%merge = OpLabel\n' if branch else ''
    return f"""OpCapability Shader
OpCapability GroupNonUniformShuffle
{capabilities}OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "long_entry_name" %lane{extra}
OpExecutionMode %main LocalSize 64 1 1
OpDecorate %lane BuiltIn SubgroupLocalInvocationId
{annotations}%void = OpTypeVoid
%fn = OpTypeFunction %void
%uint = OpTypeInt 32 0
%bool = OpTypeBool
%one = OpConstant %uint 1
%scope = OpConstant %uint 3
%input = OpTypePointer Input %uint
%lane = OpVariable %input Input
{variables}%main = OpFunction %void None %fn
%entry = OpLabel
%id = OpLoad %uint %lane
{before}%value = OpGroupNonUniformShuffle %uint %scope %id %one
{after}OpReturn
OpFunctionEnd
"""


@unittest.skipUnless(shutil.which('spirv-as') and shutil.which('spirv-val'), 'requires SPIR-V tools')
class ComputeWarp(unittest.TestCase):
    def module(self, **parameters):
        raw = subprocess.run(['spirv-as', '--target-env', 'vulkan1.1', '-', '-o', '-'],
                             input=assembly(**parameters).encode(), capture_output=True, check=True).stdout
        subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', '-'], input=raw, check=True)
        return raw

    def test_straight_line_uses_workgroup_exchange_and_one_linear_index(self):
        for existing_index, declared_only in ((False, False), (True, False), (True, True)):
            with self.subTest(existing_index=existing_index, declared_only=declared_only):
                result = spv_warp.lower(self.module(existing_index=existing_index, declared_index_only=declared_only))
                subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', '-'], input=result, check=True)
                _, code = decode(result)
                self.assertEqual(sum(opcode(i) == 224 for i in code), 2)
                self.assertFalse(any(opcode(i) == 345 for i in code))
                self.assertEqual(sum(opcode(i) == 71 and i[2:4] == (11, 29) for i in code), 1)
                self.assertEqual(spv_warp.lower(result), result)

    def test_divergence_shared_memory_and_specialized_group_sizes_keep_native_path(self):
        for option in ('branch', 'shared', 'specialize_size'):
            with self.subTest(option=option):
                raw = self.module(**{option: True})
                self.assertEqual(spv_warp.lower(raw), raw)

    def test_subgroup_mask_and_shuffle_keep_a_consistent_width(self):
        for mask in ('SubgroupEqMask', 'SubgroupGeMask', 'SubgroupGtMask', 'SubgroupLeMask', 'SubgroupLtMask'):
            with self.subTest(mask=mask):
                raw = self.module(mask=mask)
                self.assertEqual(spv_warp.lower(raw), raw)


if __name__ == '__main__':
    unittest.main()
