"""Generate real Maxwell dynamic C2 arrays through the locked shader chain."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT)); sys.path.insert(0, str(ROOT / 'dm/tools/tests'))
from dm.android.tools import spv_android as vk
from test_txq_type import base, mov, shader
from test_dmsh_pool_arrays import NS


def generate(work):
    work.mkdir(parents=True, exist_ok=True); export = work / 'export'; export.mkdir(exist_ok=True)
    apple = work / 'apple'; apple.mkdir(exist_ok=True)
    replay = (ROOT / 'dm/gfx/nvn/nvn_mtl.m').read_text()
    def function(signature):
        start = replay.index(signature); end = replay.index('{', start)+1; depth = 1
        while depth:
            depth += (replay[end] == '{') - (replay[end] == '}'); end += 1
        return replay[start:end]
    (work / 'c2_reference.h').write_text('#define NTEX 256\n#define NVN_SEPTEX_SLOT0 DMSH_SEPTEX_SLOT0\n'
        'static struct { uint64_t tex[6][256],img[6][32],samp[6][128]; } S;\n' +
        function('static inline uint64_t tex_slot_h(') + '\n' + function('static uint64_t c2_handle(int stage, unsigned b) {') + '\n')
    translator = os.environ.get('DMSH_TEST_XLATE', str(ROOT / 'dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate'))
    for separate in (0, 1):
        def load(reg, slot, offset, src=255, width=4):
            return base('Ldc') | (width << 48) | (slot << 36) | (offset << 20) | (7 << 16) | (src << 8) | reg
        ops = [load(10, 3, 0), load(0, 3, 4), mov(1, 0x3f000000), load(6, 2, 0, 10)]
        if separate:
            ops += [load(11, 2, 0x570), base('Lop') | (1 << 41) | (11 << 20) | (7 << 16) | (6 << 8) | 6]
        ops += [base('TexB') | (1 << 37) | (1 << 31) | (2 << 28) | (6 << 20) | (7 << 16) | 8,
                load(4, 3, 56, width=5), base('Stg') | (4 << 48) | (1 << 45) | (7 << 16) | (4 << 8) | 8]
        key = f'{0xfeed4000+separate:016x}'; code = work / (key + '.bin'); code.write_bytes(shader(ops))
        jobs = work / (key + '.jobs.json')
        job = dict(key=key, stage='cs', code=str(code), local=[1, 1, 1])
        jobs.write_text(json.dumps([job])); subprocess.run([translator, jobs, apple], check=True)
        jobs.write_text(json.dumps([dict(job, host_target='vulkan')]))
        subprocess.run([translator, jobs, work], check=True)
        binary = work / (key + '.spv')
        if binary.read_bytes() != (apple / binary.name).read_bytes():
            raise ValueError('Maxwell C2 fixture differs between Apple and Vulkan translation targets')
        source = subprocess.run([ROOT / 'dm/gfx/tools/spirv_cross_bin/spirv-cross', '--msl', '--msl-version', '20300',
                                 '--msl-argument-buffers', apple / binary.name], capture_output=True, text=True, check=True).stdout
        (work / (key + '.metal')).write_text(source)
        resources = json.loads((work / (key + '.res.json')).read_text())
        reflection = dict(stage='cs', bindings=NS['reflect'](key, 'cs', source, resources))
        (work / (key + '.reflection.json')).write_text(json.dumps(reflection, indent=2))
        vk.write_stage(key, binary.read_bytes(), reflection, export, shutil.which('spirv-val'), shutil.which('spirv-opt'))
    return export


def main():
    parser = argparse.ArgumentParser(description=__doc__); parser.add_argument('directory', type=Path)
    args = parser.parse_args(); print(generate(args.directory.resolve()))


if __name__ == '__main__': main()
