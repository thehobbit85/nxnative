"""Task/mesh export fixtures: bounded stage memory, payloads and early returns."""
import argparse
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
from dm.android.tools import spv_android as vk


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--apple', action='store_true', help='Also emit the shared Apple stages and their raw-memory adapters')
    args = parser.parse_args(); work = args.directory.resolve(); work.mkdir(parents=True, exist_ok=True)
    export = work / 'export'; export.mkdir(exist_ok=True)
    for variant, (kind, threads, task) in enumerate((('ts', 8, False), ('ts', 129, False),
                                                   ('ms', 32, False), ('ms', 8, True), ('ts', 1, False))):
        info = dict(kind=kind, threads=threads, payload=32)
        if kind == 'ms': info.update(task=task, max_v=33, max_p=35,
            vertex=[[None, c] for c in range(4)] + [[2, 0]], primitive=[[0, 1]])
        # Literal layouts independently calculated from the shared ABI:
        # 16-byte header,32-byte payload; mesh has two32-element blocks of
        # six components, then4+35*3 primitive bytes rounded to a word.
        offsets, words = ([0, 16, 48, 80], 20) if kind == 'ts' else ([0, 16, 48, 1584], 424)
        key = f'00000000feed001{variant}'
        raw = work / (key + '.spv')
        if variant == 4:
            code = b''.join(struct.pack('<QQ', lo, hi) for hi, lo in (
                (0x000fe20000007800, 0x00000000ff007923),
                (0x0041e8000000b800, 0x00001000ff007927),
                (0x000fea0003800000, 0x000000000000794d)))
            source = work / 'task.bin'; source.write_bytes(code)
            jobs = work / 'jobs.json'
            jobs.write_text(json.dumps([dict(key=key, stage='cs', arch='sm86', code=str(source),
                tex={}, img={}, attr={}, fout={}, local=[threads, 1, 1], shared=words * 4,
                mesh_stage=info, stage_memory=offsets)]))
            captured = work / 'capture'; captured.mkdir(exist_ok=True)
            os.environ['DMSH_SPV_CAPTURE'] = str(captured)
            vk.capture([str(jobs), str(work)])
            raw = captured / raw.name
            if args.apple:
                apple = work / 'apple-raw'; apple.mkdir(exist_ok=True)
                subprocess.run([ROOT / 'dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate', jobs, apple], check=True)
                if raw.read_bytes() != (apple / raw.name).read_bytes():
                    raise ValueError('Ampere task translation differs between Apple and Vulkan targets')
        else:
            body = 'memory[12] = memory[0]; memory[13] = 2048u; memory[14] = 0x7fc12345u; memory[16] = memory[5];'
            if kind == 'ms':
                body = ('memory[12] = memory[0]; memory[44] = 0x7fc12345u; memory[76] = memory[4]; '
                        'memory[108] = memory[5]; memory[140] = gl_WorkGroupID.x; '
                        'memory[396] = 0x23000000u; memory[397] = 0x03020100u;')
            source = (f'#version 450\nlayout(local_size_x={threads}) in; shared uint memory[{words}];\n'
                      'void main() { if (gl_LocalInvocationID.x != 0) return; ' + body + ' }')
            subprocess.run(['glslc', '-fshader-stage=compute', '--target-env=vulkan1.1', '-', '-o', raw],
                           input=source, text=True, check=True)
        reflection = dict(stage=kind, bindings=[], threadgroup_size=[threads, 1, 1], mesh_stage=info, stage_memory=offsets)
        vk.write_stage(key, raw.read_bytes(), reflection, export, shutil.which('spirv-val'), shutil.which('spirv-opt'))
        if args.apple:
            from meshstage_apple import sources
            cross = ROOT / 'dm/gfx/tools/spirv_cross_bin/spirv-cross'
            msl = subprocess.run([cross, raw, '--msl', '--msl-version', '30100'], capture_output=True, text=True, check=True).stdout
            native, adapter = sources(msl, info)
            (work / (key + '.native.metal')).write_text(native)
            (work / (key + '.metal')).write_text(adapter)
    print(f'Exported five task/mesh memory fixtures to {export}')


if __name__ == '__main__': main()
