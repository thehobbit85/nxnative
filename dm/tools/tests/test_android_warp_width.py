"""Native Vulkan exchanges require enough physical lanes for every observable use."""
import ctypes
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
from dm.android.tools import spv_android, spv_warp_native, spv_warp_width


@unittest.skipUnless(shutil.which('glslc') and shutil.which('spirv-opt'), 'requires GLSL and SPIR-V tools')
class WarpWidth(unittest.TestCase):
    def requirement(self, body, partition=True):
        src = """#version 450
#extension GL_KHR_shader_subgroup_shuffle : require
#extension GL_KHR_shader_subgroup_ballot : require
layout(location=0) out uint result;
void main() {
    uint lane = gl_SubgroupInvocationID;
    uint data = uint(gl_FragCoord.x);
    BODY
}
"""
        raw = subprocess.check_output(['glslc', '--target-env=vulkan1.1', '-fshader-stage=frag', '-', '-o', '-'],
                                      input=src.replace('BODY', body).encode())
        data = spv_warp_native.lower(raw) if partition else raw
        return spv_warp_width.requirement(data, shutil.which('spirv-opt'))

    def test_partitions_and_observable_lane_id(self):
        for offset, width in ((1, 4), (3, 4), (7, 8), (15, 16), (31, 32)):
            self.assertEqual(self.requirement(f'result = subgroupShuffle(data, lane ^ {offset}u);'), (width, 128))
        self.assertEqual(self.requirement('result = (lane & 3u) + subgroupShuffle(data, lane ^ 1u);'), (4, 128))
        self.assertEqual(self.requirement('result = lane + subgroupShuffle(data, lane ^ 1u);'), (32, 128))
        self.assertEqual(self.requirement('result = subgroupShuffle(lane, lane ^ 1u);'), (32, 128))

    def test_dynamic_selector_votes_and_unpartitioned_operations(self):
        self.assertEqual(self.requirement('result = subgroupShuffle(data, data & 31u);'), (32, 128))
        self.assertEqual(self.requirement('result = subgroupBallot(data != 0u).x;'), (32, 128))
        self.assertEqual(self.requirement('result = subgroupShuffle(data, 31u);', partition=False), (32, 32))

    def test_declared_but_dead_lane_is_irrelevant(self):
        self.assertIsNone(self.requirement('result = data;'))


def replay_fixture(out):
    out.mkdir(exist_ok=True, parents=True)
    # The donor is the result of a conditional atomic. Hoisting the exchange
    # across the branch would increment addresses whose lanes are inactive.
    source = """#version 450
#extension GL_KHR_shader_subgroup_shuffle : require
layout(local_size_x=32) in;
layout(set=0, binding=0, std430) buffer Records { uint words[]; } records;
void main() {
    uint id = gl_LocalInvocationIndex;
    if ((id & 4u) == 0u) {
        uint old = atomicAdd(records.words[id], 1u);
        records.words[32u + id] = subgroupShuffle(old, gl_SubgroupInvocationID ^ PEER);
    }
}
"""
    reflection = dict(stage='cs', threadgroup_size=[32, 1, 1], bindings=[dict(msl_name='records', kind='sbuf', reg=0)])
    files = []
    for index, peer in enumerate((3, 16)):
        key = f'{0x52 + index:016x}'
        raw = subprocess.check_output([os.environ.get('GLSLC', 'glslc'), '--target-env=vulkan1.1', '-fshader-stage=comp',
                                       f'-DPEER={peer}u', '-', '-o', '-'], input=source.encode())
        spv_android.write_stage(key, raw, reflection, out, 'spirv-val', 'spirv-opt')
        files.append((out / (key + '.spv'), out / (key + '.vk.txt')))
    return files


@unittest.skipUnless(all(shutil.which(t) for t in ('cc', 'glslc', 'spirv-val', 'spirv-opt')), 'requires C and SPIR-V tools')
class WidthContract(unittest.TestCase):
    def test_exported_ranges_and_pipeline_selection(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            source = work / 'contract.c'
            source.write_text('''#include "dm/gfx/nvn/vulkan/shader_contract.h"
const char *choose(const char *side, size_t bytes, const uint32_t *words, size_t count,
                   unsigned native, unsigned first, unsigned last, unsigned max_groups, int *selected) {
    dm_vk_warp_width width;
    uint32_t local;
    const char *error = dm_vk_shader_contract_width(side, bytes, words, count, &width, &local);
    if (!error) *selected = dm_vk_warp_select(width, native, first, last, local, max_groups);
    return error;
}
''')
            lib = work / 'check.so'
            subprocess.run(['cc', '-shared', '-fPIC', '-std=c11', '-Werror', '-Wall', '-Wextra', '-I', ROOT,
                            source, '-o', lib], check=True)
            choose = ctypes.CDLL(str(lib)).choose
            choose.restype = ctypes.c_char_p
            choose.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t,
                               *([ctypes.c_uint] * 4), ctypes.POINTER(ctypes.c_int)]
            for paths, minimum in zip(replay_fixture(work), (4, 32)):
                raw, side = (p.read_bytes() for p in paths)
                row = f'warp {minimum} 128\n'.encode()
                self.assertIn(row, side)
                def select(text, default, first, last, groups, raw=raw):
                    result = ctypes.c_int()
                    error = choose(text, len(text), raw, len(raw) // 4, default, first, last, groups, ctypes.byref(result))
                    return error or result.value
                self.assertEqual(select(side, 4, 0, 0, 0), 0 if minimum == 4 else -1)
                self.assertEqual(select(side, 64, 0, 0, 0), 0)
                self.assertEqual(select(side, 2, 4, 64, 1), 32)  # smaller groups exceed the workgroup limit
                self.assertEqual(select(side, 2, 4, 16, 1), -1)
                self.assertEqual(select(side.replace(b'tg 32 1 1', b'tg 1 1 1'), 2, 4, 16, 1), -1)
                self.assertEqual(select(side.replace(row, b''), 64, 4, 64, 1), 32)  # legacy export
                for invalid in (side + row, side + b'warp independent\n', side.replace(row, b'warp 7 128\n')):
                    self.assertIsInstance(select(invalid, 32, 0, 0, 0), bytes)

    def test_width_metadata_rejects_malformed_ranges(self):
        for malformed in (4, '4 128', (4.0, 128), [4], [4, False], [4, 32], [32, 256]):
            with self.subTest(malformed=malformed), self.assertRaisesRegex(ValueError, 'subgroup'):
                spv_android.vk_txt(dict(stage='cs', bindings=[], warp_width=malformed))
        self.assertIn('warp independent\n', spv_android.vk_txt(dict(stage='cs', bindings=[], warp_width=None)))


if __name__ == '__main__':
    if sys.argv[1:2] == ['--fixture']:
        replay_fixture(Path(sys.argv[2]))
    else:
        unittest.main()
