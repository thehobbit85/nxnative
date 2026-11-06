"""Native Vulkan subgroup operations are partitioned into guest warps of 32."""
from pathlib import Path
import shutil
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
from dm.android.tools import spv_warp_native
from dm.android.tools.spv_module import decode, opcode
from dm.tools.tests import test_android_compute_warp as compute_warp
from dm.tools.tests.test_android_compute_warp import assembly


@unittest.skipUnless(shutil.which('spirv-as') and shutil.which('spirv-val'), 'requires SPIR-V tools')
class NativeWarp(unittest.TestCase):
    assemble = compute_warp.ComputeWarp.assemble

    def test_source_index_includes_physical_warp_base(self):
        raw = self.assemble(assembly(branch=True))
        result = spv_warp_native.lower(raw)
        subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', '-'], input=result, check=True)
        code = decode(result)[1]
        shuffle = next(i for i in code if opcode(i) == 345)
        self.assertTrue(any(opcode(i) == 197 and i[2] == shuffle[5] for i in code))
        self.assertEqual(spv_warp_native.lower(result), result)

    def test_ballot_and_votes_use_one_word_and_active_lanes(self):
        source = assembly().replace('OpCapability GroupNonUniformShuffle', 'OpCapability GroupNonUniformVote\nOpCapability GroupNonUniformBallot')
        source = source.replace('%input =', '%v = OpTypeVector %uint 4\n%input =')
        source = source.replace('%value = OpGroupNonUniformShuffle %uint %scope %id %one',
            '%pred = OpIEqual %bool %id %one\n%mask = OpGroupNonUniformBallot %v %scope %pred\n'
            '%a = OpGroupNonUniformAll %bool %scope %pred\n%b = OpGroupNonUniformAny %bool %scope %pred\n'
            '%c = OpGroupNonUniformAllEqual %bool %scope %pred')
        result = spv_warp_native.lower(self.assemble(source))
        subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', '-'], input=result, check=True)
        ops = [opcode(i) for i in decode(result)[1]]
        self.assertIn(77, ops)  # extract the lane's 32-bit word
        self.assertFalse({334, 335, 336}.intersection(ops))

    def test_other_subgroup_semantics_remain_intact(self):
        source = assembly().replace('OpCapability GroupNonUniformShuffle','OpCapability GroupNonUniformArithmetic')
        source = source.replace('OpGroupNonUniformShuffle %uint %scope %id %one','OpGroupNonUniformIAdd %uint %scope Reduce %id')
        raw = self.assemble(source)
        self.assertEqual(spv_warp_native.lower(raw), raw)
