"""Compile the portable float custom-border sampling forms."""
from pathlib import Path
import os
import subprocess
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[4] / 'android/tools'))
from spv_android import requirements
out=Path(sys.argv[1]);out.mkdir(parents=True,exist_ok=True)
for form in range(5):
    for separate in range(2):
        binary=out/f'00000000feedb4{form*2+separate:02x}.spv'
        flags=[f'-DFORM={form}']+(['-DSEPARATE'] if separate else [])
        subprocess.run([os.environ.get('GLSLC','glslc'),'--target-env=vulkan1.1',
            '-fshader-stage=fragment' if form==3 else '-fshader-stage=compute',
            *flags,Path(__file__).with_name('sampler_border.comp'),'-o',binary],check=True)
        req=requirements(binary.read_bytes())
        lines=['schema 2','target vulkan1.1','stage ps' if form==3 else 'stage cs']
        if form!=3: lines+=['tg 1 1 1','b 1 ssbo 1 sbuf 0 -1 - -1']
        lines += [f'cap {cap}' for cap in req['capabilities']]+[f'ext {ext}' for ext in req['extensions']]
        lines += [f'b 0 {"tex" if separate else "cis"} 1 tex 0 -1 {"- -1" if separate else "samp_tex 0"} view=1 num=0',
            'b 2 ubo 1 cbuf 0 -1 - -1']
        if separate: lines+=['b 3 samp 1 samp 0 -1 - -1']
        binary.with_suffix('.vk.txt').write_text('\n'.join(lines)+'\n')
