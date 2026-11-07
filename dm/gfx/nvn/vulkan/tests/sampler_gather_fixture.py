"""Gather instructions retain their individual texels under reduction samplers."""
from pathlib import Path
import os
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[4] / 'android/tools'))
from spv_android import requirements

out = Path(sys.argv[1]); out.mkdir(parents=True, exist_ok=True)
source = Path(__file__).with_name('sampler_gather.comp')
for shape in range(4):
    for form in range(3 if shape < 2 else 1):
        for shadow in range(2):
            for separate in range(2):
                variant = (shape * 3 + form) * 4 + shadow * 2 + separate
                binary = out / f'00000000feedb3{variant:02x}.spv'
                flags = [f'-DSHAPE={shape}', f'-DFORM={form}']
                flags += ['-DSHADOW'] if shadow else []
                flags += ['-DSEPARATE'] if separate else []
                subprocess.run([os.environ.get('GLSLC', 'glslc'), '--target-env=vulkan1.1', *flags, source, '-o', binary], check=True)
                req = requirements(binary.read_bytes())
                lines = ['schema 2', 'target vulkan1.1', 'stage cs', 'tg 1 1 1']
                lines += [f'cap {cap}' for cap in req['capabilities']] + [f'ext {ext}' for ext in req['extensions']]
                lines += [f'b 0 {"tex" if separate else "cis"} 1 tex 0 -1 {"- -1" if separate else "samp_tex 0"} view={[1,5,3,6][shape]} num=0',
                          'b 1 ssbo 1 sbuf 0 -1 - -1', 'b 2 ubo 1 cbuf 0 -1 - -1']
                if separate: lines += ['b 3 samp 1 samp 0 -1 - -1']
                binary.with_suffix('.vk.txt').write_text('\n'.join(lines) + '\n')
