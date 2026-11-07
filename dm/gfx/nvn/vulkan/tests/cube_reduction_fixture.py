"""Compile cube/cube-array interfaces for native and emulated min/max filtering."""
from pathlib import Path
import os
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[4] / 'android/tools'))
from spv_android import requirements

out = Path(sys.argv[1]); out.mkdir(parents=True, exist_ok=True)
source = Path(__file__).with_name('cube_reduction.comp')
for variant in range(16):
    array, separate, form = variant & 1, variant & 2, variant >> 2
    fragment = form >= 2
    flags = [flag for enabled, flag in [(array, '-DARRAY'), (separate, '-DSEPARATE'),
             (form == 1, '-DGRADIENTS'), (fragment, '-DFRAGMENT'), (form == 3, '-DQUERY')] if enabled]
    binary = out / f'00000000feedb1{variant:02x}.spv'
    subprocess.run([os.environ.get('GLSLC', 'glslc'), '--target-env=vulkan1.1',
                    '-fshader-stage=fragment' if fragment else '-fshader-stage=compute', *flags, source, '-o', binary], check=True)
    req = requirements(binary.read_bytes())
    lines = ['schema 2', 'target vulkan1.1', 'stage ps' if fragment else 'stage cs']
    if not fragment: lines += ['tg 1 1 1']
    lines += [f'cap {cap}' for cap in req['capabilities']] + [f'ext {ext}' for ext in req['extensions']]
    lines += [f'b 0 {"tex" if separate else "cis"} 1 tex 0 -1 {"- -1" if separate else "samp_tex 0"} view={6 if array else 3} num=0',
              'b 2 ubo 1 cbuf 0 -1 - -1']
    if not fragment: lines += ['b 1 ssbo 1 sbuf 0 -1 - -1']
    if separate: lines += ['b 3 samp 1 samp 0 -1 - -1']
    binary.with_suffix('.vk.txt').write_text('\n'.join(lines) + '\n')
