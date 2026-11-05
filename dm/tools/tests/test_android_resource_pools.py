"""Whole ID pools retain their logical range while allocating only live descriptors."""
import os
import json
from pathlib import Path
import shutil
import tempfile
import struct
import subprocess
import unittest

from dm.android.tests.resource_pool_fixture import generate
from dm.android.tools import spv_android as vk
from dm.android.tools import spv_pool
from dm.android.tools.spv_module import decode, opcode
from dm.android.tools.spv_capture import instruction, Module


class ResourcePoolMetadata(unittest.TestCase):
    def test_metadata_requires_runtime_range_and_valid_bounds_slot(self):
        good = dict(kind='tex_pool', type='tex', count=1048576, runtime=True, pool_slot=0)
        self.assertEqual(spv_pool.fields(good), ('runtime=1', 'pool_slot=0'))
        for changes in ({'runtime': False}, {'count': 32}, {'type': 'cis'}, {'pool_slot': -1}, {'pool_slot': 127}, {'pool_slot': True}):
            with self.subTest(changes=changes), self.assertRaises(ValueError): spv_pool.fields(dict(good, **changes))
        for count in (None, False, 0, 128):
            with self.subTest(count=count), self.assertRaises(ValueError):
                spv_pool.fields(dict(kind='pool_bounds', type='ubo', count=1, reg=count))

    def test_pointer_use_detection_does_not_treat_literals_as_ids(self):
        self.assertEqual(spv_pool.pointer_operands(instruction(81, 1, 2, 3, 97)), ())
        self.assertEqual(spv_pool.pointer_operands(instruction(65, 1, 2, 97, 4)), (97,))
        self.assertEqual(spv_pool.pointer_operands(instruction(57, 1, 2, 3, 97)), (97,))


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE') and all(shutil.which(t) for t in ('spirv-opt', 'spirv-val')),
                     'requires locked translator and SPIRV-Tools')
class ResourcePoolExport(unittest.TestCase):
    def test_task_helper_bindings_follow_pool_binding_compaction(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp); export = generate(work)
            raw = (work / '00000000feed6000.spv').read_bytes(); m = Module(raw)
            m.variable(m.ty(28, m.uint, m.const(20)), 4, 'stage_memory')
            words = list(m.header); words[3] = m.next_id
            inserted = False
            for x in m.code:
                if opcode(x) == 54 and not inserted:
                    for declaration in m.declarations: words.extend(declaration)
                    inserted = True
                words.extend(x)
            reflection = json.loads((work / '00000000feed6000.reflection.json').read_text())
            reflection.update(stage='ts', mesh_stage=dict(kind='ts', threads=8, payload=32), stage_memory=[0, 16, 48, 80])
            vk.write_stage('00000000feed6001', struct.pack(f'<{len(words)}I', *words), reflection, export,
                           shutil.which('spirv-val'), shutil.which('spirv-opt'))
            text = (export / '00000000feed6001.vk.txt').read_text()
            control = next(line.split()[-3:] for line in text.splitlines() if line.startswith('mesh '))
            bindings = {int(line.split()[5]): line.split()[1] for line in text.splitlines()
                        if line.startswith('b ') and line.split()[4] == 'mesh'}
            self.assertEqual(control[1:], [bindings[i] for i in (1, 2)])

    def test_pool_image_shapes_survive_runtime_array_lowering(self):
        with tempfile.TemporaryDirectory() as tmp:
            # The shared translator samples 1D as a one-row 2D texture on both
            # targets (Sample1DAs2D); its array form likewise stays 2D-array.
            for dimension, view in ((0, 1), (1, 5), (2, 1), (3, 5), (4, 2), (6, 3), (7, 6)):
                with self.subTest(dimension=dimension):
                    export = generate(Path(tmp) / str(dimension), dimension)
                    text = (export / '00000000feed6000.vk.txt').read_text()
                    self.assertIn(f'runtime=1 pool_slot=0 view={view} num=0', text)

    def test_maxwell_loaded_handles_keep_full_runtime_pool_ranges(self):
        with tempfile.TemporaryDirectory() as tmp:
            export = generate(Path(tmp)); text = (export / '00000000feed6000.vk.txt').read_text()
            self.assertIn('tex 1048576 tex_pool', text)
            self.assertIn('samp 4096 samp_pool', text)
            self.assertIn('ubo 1 pool_bounds', text)
            self.assertEqual(text.count('runtime=1 pool_slot='), 2)
            self.assertIn('cap 5302\n', text)
            self.assertIn('cap 5307\n', text)
            self.assertIn('ext SPV_EXT_descriptor_indexing\n', text)
            self.assertIn(5302, vk.requirements((export / '00000000feed6000.spv').read_bytes())['capabilities'])
            # A literal equal to an array variable ID is not a pointer escape.
            raw = vk.export_spirv((Path(tmp) / '00000000feed6000.spv').read_bytes(), shutil.which('spirv-opt'))
            reflection = json.loads((Path(tmp) / '00000000feed6000.reflection.json').read_text())
            header, code = decode(raw)
            uint = next(x[1] for x in code if opcode(x) == 21 and x[2:] == (32, 0))
            arrays = {x[1] for x in code if opcode(x) == 28}
            pointers = {x[1] for x in code if opcode(x) == 32 and x[3] in arrays}
            pool = next(x[2] for x in code if opcode(x) == 59 and x[1] in pointers and x[3] == 0)
            at = next(i for i, x in enumerate(code) if opcode(x) == 54)
            literal = instruction(43, uint, header[3], pool)
            words = [*header[:3], header[3]+1, header[4]]
            for x in code[:at]+[literal]+code[at:]: words.extend(x)
            lowered, _ = spv_pool.lower(struct.pack(f'<{len(words)}I', *words), reflection)
            subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', '-'], input=lowered, check=True)
            # A source with an incompatible logical count must not be silently
            # treated as the shared whole-pool layout.
            wrong = []
            for x in code:
                if opcode(x) == 43 and len(x) == 4 and x[3] == 1048576: x = instruction(43, x[1], x[2], 1048575)
                wrong.extend(x)
            data = struct.pack(f'<{len(header)+len(wrong)}I', *header, *wrong)
            with self.assertRaisesRegex(ValueError, 'unexpected shared descriptor pool array'): spv_pool.lower(data, reflection)


if __name__ == '__main__': unittest.main()
