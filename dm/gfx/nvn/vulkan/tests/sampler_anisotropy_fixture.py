"""Compile sampled 2D/volume/cube anisotropic reduction interfaces."""
from pathlib import Path
import os
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[4] / 'android/tools'))
from spv_android import requirements

out = Path(sys.argv[1]); out.mkdir(parents=True, exist_ok=True)
source = Path(__file__).with_name('sampler_anisotropy.comp')
for shape in range(5):
    for form in range(4):
        for separate in range(2):
            variant = shape * 8 + form * 2 + separate
            binary = out / f'00000000feedb2{variant:02x}.spv'
            flags = [f'-DSHAPE={shape}', f'-DFORM={form}'] + (['-DSEPARATE'] if separate else [])
            subprocess.run([os.environ.get('GLSLC', 'glslc'), '--target-env=vulkan1.1',
                            '-fshader-stage=fragment' if form >= 2 else '-fshader-stage=compute',
                            *flags, source, '-o', binary], check=True)
            req = requirements(binary.read_bytes())
            lines = ['schema 2', 'target vulkan1.1', 'stage ps' if form >= 2 else 'stage cs']
            if form < 2: lines += ['tg 1 1 1']
            lines += [f'cap {cap}' for cap in req['capabilities']] + [f'ext {ext}' for ext in req['extensions']]
            lines += [f'b 0 {"tex" if separate else "cis"} 1 tex 0 -1 {"- -1" if separate else "samp_tex 0"} view={[1,5,2,3,6][shape]} num=0',
                      'b 2 ubo 1 cbuf 0 -1 - -1']
            if form < 2: lines += ['b 1 ssbo 1 sbuf 0 -1 - -1']
            if separate: lines += ['b 3 samp 1 samp 0 -1 - -1']
            binary.with_suffix('.vk.txt').write_text('\n'.join(lines) + '\n')
for separate in range(2):
    binary = out / f'00000000feedb2{40+separate:02x}.spv'
    flags = ['-DSHAPE=0', '-DFORM=1', '-DDESCRIPTOR_ARRAY'] + (['-DSEPARATE'] if separate else [])
    subprocess.run([os.environ.get('GLSLC', 'glslc'), '--target-env=vulkan1.1', *flags, source, '-o', binary], check=True)
    req = requirements(binary.read_bytes())
    lines = ['schema 2', 'target vulkan1.1', 'stage cs', 'tg 1 1 1']
    lines += [f'cap {cap}' for cap in req['capabilities']] + [f'ext {ext}' for ext in req['extensions']]
    lines += [f'b 0 {"tex" if separate else "cis"} 3 tex 0 -1 {"- -1" if separate else "samp_tex 0"} view=1 num=0',
              'b 1 ssbo 1 sbuf 0 -1 - -1', 'b 2 ubo 1 cbuf 0 -1 - -1']
    if separate: lines += ['b 3 samp 3 samp 0 -1 - -1']
    binary.with_suffix('.vk.txt').write_text('\n'.join(lines) + '\n')
