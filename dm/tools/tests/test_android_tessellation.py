"""Preserve the reference hull/domain interfaces; tessellated drawing is separate."""
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

from test_txq_type import base, mov, shader

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'dm/android/tools'))
import spv_android as vk


def program(stage, vertices, location):
    header = bytearray(0x50)
    struct.pack_into('<5I', header, 0, (1 << 24) | ((2 if stage == 'hs' else 3) << 10) | (3 << 5) | 1,
                     1 << 24, vertices << 24, 0, vertices)
    def store(reg, offset, patch=False):
        return base('Ast') | (7 << 16) | (255 << 8) | (offset << 20) | (int(patch) << 31) | reg
    def load(reg, offset, patch=False):
        return base('Ald') | (7 << 16) | (255 << 8) | (255 << 39) | (offset << 20) | (int(patch) << 31) | reg
    ops = [mov(4, 0x3f800000)]
    if stage == 'hs':
        # Six factors and patch attribute0; forward the selected vertex input.
        ops += [store(4, offset, True) for offset in range(0, 28, 4)]
        ops += [load(5, 0x80 + location * 16), store(5, 0x80 + location * 16)]
    else:
        # Domain coordinates feed position.xy; patch input feeds a generic.
        ops += [load(5, 0x2f0), store(5, 0x70), load(5, 0x2f4), store(5, 0x74),
                store(0, 0x78), store(4, 0x7c), load(5, 0x18, True), store(5, 0x80 + location * 16)]
    return bytes(header) + shader(ops)


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE') and all(shutil.which(t) for t in ('spirv-opt', 'spirv-val')),
                     'requires the locked translator and SPIRV-Tools')
class TessellationExport(unittest.TestCase):
    def test_patch_and_vertex_interfaces_survive_both_translation_targets(self):
        with tempfile.TemporaryDirectory(prefix='vk_tessellation_') as tmp:
            work = Path(tmp); export = work / 'export'; export.mkdir()
            apple = work / 'apple'; apple.mkdir()
            cases = [(stage, vertices, location) for stage in ('hs', 'ds') for vertices in (1, 3, 32) for location in (0, 30, 31)]
            jobs = []
            for number, (stage, vertices, location) in enumerate(cases):
                key = f'{0xfeed1000 + number:016x}'; path = work / (key + '.bin')
                path.write_bytes(program(stage, vertices, location))
                jobs.append(dict(key=key, stage=stage, code=str(path), tex={}, img={}, attr={}, fout={}))
            path = work / 'jobs.json'; path.write_text(json.dumps(jobs))
            subprocess.run([os.environ['DMSH_TEST_XLATE'], path, apple], check=True)
            path.write_text(json.dumps([dict(job, host_target='vulkan') for job in jobs]))
            subprocess.run([os.environ['DMSH_TEST_XLATE'], path, work], check=True)
            for job, (stage, vertices, location) in zip(jobs, cases):
                with self.subTest(stage=stage, vertices=vertices, location=location):
                    key = job['key']; raw = (work / (key + '.spv')).read_bytes()
                    self.assertEqual(raw, (apple / (key + '.spv')).read_bytes())
                    vk.write_stage(key, raw, dict(stage=stage, bindings=[]), export, shutil.which('spirv-val'), shutil.which('spirv-opt'))
                    data = (export / (key + '.spv')).read_bytes(); instructions = vk.decode(data)[1]
                    self.assertEqual([i[1] for i in instructions if vk.opcode(i) == 15], [1 if stage == 'hs' else 2])
                    modes = [i[2:] for i in instructions if vk.opcode(i) == 16]
                    self.assertEqual(modes, [(26, vertices)] if stage == 'hs' else [(22,), (1,), (4,)])
                    decorations = [i for i in instructions if vk.opcode(i) == 71]
                    builtins = {i[3] for i in decorations if i[2] == 11}
                    self.assertTrue({8, 11, 12} <= builtins if stage == 'hs' else 13 in builtins)
                    patch = {i[1] for i in decorations if i[2] == 15}
                    locations = {i[1]: i[3] for i in decorations if i[2] == 30}
                    self.assertTrue(any(locations.get(p) == 0 for p in patch))
                    self.assertTrue(any(loc == location and var not in patch for var, loc in locations.items()))
                    sidecar = (export / (key + '.vk.txt')).read_text()
                    self.assertIn(f'stage {stage}\n', sidecar); self.assertIn('cap 3\n', sidecar)
                    self.assertNotIn('\ntg ', sidecar)
                    self.assertFalse((export / (key + '.vc.spv')).exists())
                    if os.environ.get('DMSH_TEST_METAL') == '1':
                        source = work / (key + '.metal')
                        cross = ROOT / 'dm/gfx/tools/spirv_cross_bin/spirv-cross'
                        result = subprocess.run([cross, '--msl', '--msl-version', '20300', '--msl-argument-buffers',
                                                 '--msl-texture-buffer-native', work / (key + '.spv')],
                                                capture_output=True, text=True, check=True)
                        source.write_text(result.stdout)
                        for sdk, target in (('macosx', 'air64-apple-macos15.0'), ('iphoneos', 'air64-apple-ios18.0')):
                            compiled = subprocess.run(['xcrun', '-sdk', sdk, 'metal', '-target', target, '-c',
                                                       '-fmodules-cache-path=' + str(work / 'modules'), source,
                                                       '-o', source.with_suffix('.air')], capture_output=True, text=True)
                            if stage == 'hs' and location == 31:
                                # The unchanged Apple route emits attribute(31),
                                # outside Metal's0..30 stage-in range. Vulkan's
                                # legal location31 still survives export.
                                self.assertNotEqual(compiled.returncode, 0)
                                self.assertIn('must be between 0 and 30', compiled.stderr)
                            else: self.assertEqual(compiled.returncode, 0, compiled.stderr)
            self.assertEqual(len(list(export.iterdir())), 36)


if __name__ == '__main__': unittest.main()
