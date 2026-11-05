"""Compile a direct GLSL driver diagnostic; no guest translator or lowering."""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
from dm.android.tools.spv_android import requirements


def generate(work, compiler):
    out = work / 'export'; out.mkdir(parents=True, exist_ok=True)
    binary = out / '00000000feed7003.spv'
    subprocess.run([compiler, '--target-env=vulkan1.1', ROOT / 'dm/android/tests/native_mip_offset.comp', '-o', binary], check=True)
    subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', binary], check=True)
    needs = requirements(binary.read_bytes())
    lines = ['schema 2', 'target vulkan1.1', 'stage cs']
    lines += ['cap ' + str(value) for value in needs['capabilities']]
    lines += ['ext ' + value for value in needs.get('extensions', [])]
    lines += ['b 2 ubo 1 cbuf 0 -1 - -1', 'b 3 ssbo 1 sbuf_ubo 0 56 - -1',
              'b 4 cis 1 tex 128 -1 samp 1 view=1 num=0']
    binary.with_suffix('.vk.txt').write_text('\n'.join(lines) + '\n')
    return out


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__); parser.add_argument('directory', type=Path)
    parser.add_argument('--glslc', default=shutil.which('glslc')); args = parser.parse_args()
    if not args.glslc: parser.error('glslc is required')
    print(generate(args.directory.resolve(), args.glslc))
