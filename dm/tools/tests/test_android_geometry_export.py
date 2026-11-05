"""Native Vulkan geometry lowering uses the shared Maxwell translator."""
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

from test_txq_type import base, mov

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'android/tools'))
import spv_android as vk


def passthrough_shader():
    # A Maxwell passthrough geometry stage writes loc0.x = 1. All other
    # attributes and the input positions pass through to the next stage.
    header = bytearray(0x50)
    struct.pack_into('<5I', header, 0, (1 << 24) | (4 << 10) | (3 << 5) | 1,
                     0, 1 << 24, 7 << 24, 1)
    ops = [mov(0, 0x3f800000), base('Ast') | (7 << 16) | (255 << 8) | (0x80 << 20),
           base('Exit') | (7 << 16) | 15]
    return bytes(header) + struct.pack('<4Q', 0, *ops)


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE') and all(shutil.which(t) for t in ('spirv-val', 'spirv-opt')),
                     'requires the offline translator and SPIRV-Tools')
class GeometryExport(unittest.TestCase):
    def test_capture_keeps_compute_and_reader_products_with_shared_layout(self):
        with tempfile.TemporaryDirectory(prefix='vk_geometry_helpers_') as directory:
            work = Path(directory); captured = work / 'capture'; captured.mkdir()
            code = work / 'geometry.bin'; code.write_bytes(passthrough_shader()); key = '0123456789abcdef'
            jobs = work / 'jobs.json'
            jobs.write_text(json.dumps([dict(key=key, stage='gs', code=str(code), tex={}, img={}, attr={}, fout={})]))
            with mock.patch.object(vk, 'XLATE', Path(os.environ['DMSH_TEST_XLATE'])), \
                 mock.patch.dict(os.environ, {'DMSH_SPV_CAPTURE': str(captured)}):
                vk.capture([str(jobs), str(work)])
            for t, vertices, output in ((0, 1, 1), (1, 2, 6), (2, 2, 6), (3, 3, 7), (4, 3, 7)):
                for suffix, role, model in (('gc', 'compute', 5), ('gr', 'reader', 0)):
                    with self.subTest(topology=t, role=role):
                        name = key + f'.{suffix}{t}'
                        self.assertTrue((captured / (name + '.spv')).is_file(), name)
                        data = (captured / (name + '.spv')).read_bytes()
                        self.assertEqual([i[1] for i in vk.decode(data)[1] if vk.opcode(i) == 15], [model])
                        control = json.loads((captured / (name + '.control.json')).read_text())
                        self.assertEqual(control['geometry'], dict(role=role, topology=t, stride=133,
                            vertices=vertices, invocations=1, output=output, index_stride=vertices * 2))

    def test_capture_exports_all_native_topologies_without_replacing_compute_helpers(self):
        with tempfile.TemporaryDirectory(prefix='vk_geometry_capture_') as directory:
            work = Path(directory); captured = work / 'capture'; captured.mkdir()
            code = work / 'geometry.bin'; code.write_bytes(passthrough_shader())
            key = '0123456789abcdef'
            job = dict(key=key, stage='gs', code=str(code), tex={}, img={}, attr={}, fout={}, debug=True)
            jobs = work / 'jobs.json'; jobs.write_text(json.dumps([job]))
            with mock.patch.object(vk, 'XLATE', Path(os.environ['DMSH_TEST_XLATE'])), \
                 mock.patch.dict(os.environ, {'DMSH_SPV_CAPTURE': str(captured)}):
                self.assertEqual(vk.capture([str(jobs), str(work)]), 0)
                for t, count in enumerate((1, 2, 2, 3, 3)):
                    data = (captured / (key + f'.g{t}.spv')).read_bytes()
                    self.assertEqual(sum(vk.opcode(i) == 218 for i in vk.decode(data)[1]), count)
                # The shared Metal chain separately asks for geometry-as-compute.
                # Its _gN helpers must retain their execution model and filenames.
                helper = dict(job, key=key + '_g0', geometry_topology=0)
                jobs.write_text(json.dumps([helper]))
                self.assertEqual(vk.capture([str(jobs), str(work)]), 0)
                data = (work / (key + '_g0.spv')).read_bytes()
                self.assertEqual([i[1] for i in vk.decode(data)[1] if vk.opcode(i) == 15], [5])

    def test_portable_selection_exports_complete_topology_family(self):
        with tempfile.TemporaryDirectory(prefix='vk_geometry_prepared_') as directory:
            work = Path(directory); raw = work / 'raw'; raw.mkdir(); prepared = work / 'prepared'; prepared.mkdir()
            code = work / 'geometry.bin'; code.write_bytes(passthrough_shader()); key = '0123456789abcdef'
            jobs = work / 'jobs.json'
            jobs.write_text(json.dumps([dict(key=key, stage='gs', code=str(code), tex={}, img={}, attr={}, fout={})]))
            with mock.patch.object(vk, 'XLATE', Path(os.environ['DMSH_TEST_XLATE'])), \
                 mock.patch.dict(os.environ, {'DMSH_SPV_CAPTURE': str(raw), 'DM_GAME': '0100633007D48000'}):
                vk.capture([str(jobs), str(work)])
                family = [key] + [key + f'.{kind}{t}' for kind in ('g', 'gc', 'gr') for t in range(5)]
                for name in family:
                    shutil.copyfile(raw / (name + '.spv'), prepared / (name + '.spv'))
                    base = dict(stage='gs', bindings=[], threadgroup_size=[32, 1, 1])
                    control = json.loads((raw / (name + '.control.json')).read_text())
                    refl = vk.spv_geometry.helper_reflection(base, control)
                    (prepared / (name + '.reflection.json')).write_text(json.dumps(refl))
                vk.capture_manifest(prepared, sorted(family), vk.shader_contract())
                vk.export_prepared(prepared, work / 'export', [key])
                self.assertEqual(len(list((work / 'export').glob('*.spv'))), 16)
                for name in family:
                    sidecar = (work / 'export' / (name + '.vk.txt')).read_text()
                    if '.gc' in name:
                        self.assertIn('stage cs\n', sidecar)
                        self.assertIn('\ntg 32 1 1\n', sidecar)
                        self.assertIn('geometry compute ', sidecar)
                        topology = next(line for line in sidecar.splitlines() if ' geometry 4 ' in line)
                        self.assertIn('num=2', topology)
                    elif '.gr' in name:
                        self.assertIn('stage vs\n', sidecar)
                        self.assertIn('geometry reader ', sidecar)
                        self.assertNotIn('\ncapture ', sidecar)
                    else: self.assertNotIn('\ntg ', sidecar)
                orphan = work / 'orphan'; orphan.mkdir(); helper = key + '.gc0'
                for suffix in ('.spv', '.reflection.json'):
                    shutil.copyfile(prepared / (helper + suffix), orphan / (helper + suffix))
                vk.capture_manifest(orphan, [helper], vk.shader_contract())
                with self.assertRaisesRegex(ValueError, 'incomplete geometry'):
                    vk.read_prepared(orphan, vk.shader_contract())
                # A corrupt manifest cannot relabel one input topology as another.
                shutil.copyfile(prepared / (key + '.g3.spv'), prepared / (key + '.g0.spv'))
                (prepared / 'capture.json').unlink()
                vk.capture_manifest(prepared, sorted(family), vk.shader_contract())
                with self.assertRaisesRegex(ValueError, 'topology'):
                    vk.export_prepared(prepared, work / 'bad', [key])

    def test_apple_native_and_compute_routes_keep_their_stage_models(self):
        with tempfile.TemporaryDirectory(prefix='vk_geometry_targets_') as directory:
            work = Path(directory); code = work / 'geometry.bin'; code.write_bytes(passthrough_shader())
            for name, options, model in (('apple', {}, 3), ('compute', {'geometry_topology': 3}, 5)):
                with self.subTest(target=name):
                    job = dict(key=name, stage='gs', code=str(code), tex={}, img={}, attr={}, fout={}, debug=True, **options)
                    jobs = work / 'jobs.json'; jobs.write_text(json.dumps([job]))
                    result = subprocess.run([os.environ['DMSH_TEST_XLATE'], jobs, work], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    data = (work / (name + '.spv')).read_bytes()
                    self.assertEqual([i[1] for i in vk.decode(data)[1] if vk.opcode(i) == 15], [model])
                    if name == 'apple':
                        self.assertIn('SPV_NV_geometry_shader_passthrough', vk.requirements(data)['extensions'])
                    else:
                        self.assertTrue((work / (name + '.reader.spv')).is_file())

    def test_native_passthrough_emits_vertices_for_each_input_topology(self):
        with tempfile.TemporaryDirectory(prefix='vk_geometry_') as directory:
            work = Path(directory); code = work / 'geometry.bin'; code.write_bytes(passthrough_shader())
            for topology, count, mode in ((0, 1, 19), (1, 2, 20), (2, 2, 21), (3, 3, 22), (4, 3, 23)):
                with self.subTest(topology=topology):
                    job = dict(key='geometry', stage='gs', code=str(code), input_topology=topology,
                               host_target='vulkan', tex={}, img={}, attr={}, fout={}, debug=True)
                    jobs = work / 'jobs.json'; jobs.write_text(json.dumps([job]))
                    result = subprocess.run([os.environ['DMSH_TEST_XLATE'], jobs, work], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    data = vk.export_spirv((work / 'geometry.spv').read_bytes(), shutil.which('spirv-opt'))
                    instructions = vk.decode(data)[1]
                    self.assertEqual(sum(vk.opcode(i) == 218 for i in instructions), count)  # OpEmitVertex
                    self.assertTrue(any(vk.opcode(i) == 16 and i[2] == mode for i in instructions))
                    self.assertNotIn('SPV_NV_geometry_shader_passthrough', vk.requirements(data)['extensions'])
                    # Location 0 is written by the GS; location 31 must survive
                    # without a linked fragment shader during offline export.
                    locations = {i[3] for i in instructions if vk.opcode(i) == 71 and i[2] == 30}
                    self.assertIn(31, locations)
                    binary = work / 'export.spv'; binary.write_bytes(data)
                    check = subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', binary], capture_output=True, text=True)
                    self.assertEqual(check.returncode, 0, check.stderr)


if __name__ == '__main__': unittest.main()
