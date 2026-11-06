"""Actual Maxwell SULD/SUST round trips with formatted and formatless image bindings."""
import argparse
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path[:0] = [str(ROOT), str(ROOT/'dm/tools/tests')]
from test_txq_type import base, mov, shader
from test_dmsh_pool_arrays import NS
from dm.android.tools import spv_android as vk

# D encodes the image element size in the instruction. The component form uses
# the unformatted float image declaration of the shared Apple compiler.
FORMATS = (('R32Float','R32_SFLOAT',None,1), ('R32Uint','R32_UINT',4,1),
           ('R32G32Uint','R32G32_UINT',5,2), ('R32G32B32A32Uint','R32G32B32A32_UINT',6,4),
           ('R8Uint','R8_UINT',0,1), ('R8Sint','R8_SINT',1,1), ('R16Uint','R16_UINT',2,1), ('R16Sint','R16_SINT',3,1))


def generate(work):
    work.mkdir(parents=True, exist_ok=True); jobs = []; rows = []
    for i, (fmt, vkfmt, size, channels) in enumerate(FORMATS):
        key = f'{0xfeed9c00+i:016x}'
        # Read (3,1) then store into (6,2). The initial pattern makes skipped
        # stores and wrong source coordinates observable, with all other texels protected.
        tail = 'D' if size is not None else ''; data = (1 if size is None else size) << 20
        fields = (0x48<<36)|(3<<33)|(7<<16)|data|16
        operations = [mov(0,3),mov(1,1),base('Suld'+tail)|fields|(7<<30),mov(0,6),mov(1,2),base('Sust'+tail)|fields]
        file = work/(key+'.bin'); file.write_bytes(shader(operations))
        jobs.append(dict(key=key, stage='cs', code=str(file), img={'0':dict(type='2d',fmt=fmt)}, local=[1,1,1], debug=True))
        rows.append(dict(key=key, format=fmt, vk_format=vkfmt, channels=channels))
    translator = os.environ.get('DMSH_TEST_XLATE', str(ROOT/'dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate'))
    for target in ('apple','vulkan'):
        d = work/target; d.mkdir(exist_ok=True); f = d/'jobs.json'
        f.write_text(json.dumps(jobs if target=='apple' else [dict(j, host_target='vulkan') for j in jobs]))
        subprocess.run([translator, f, d],check=True,capture_output=True)
    out = work/'export'; out.mkdir(exist_ok=True)
    for row in rows:
        key = row['key']; raw = work/'apple'/(key+'.spv')
        source = subprocess.run([ROOT/'dm/gfx/tools/spirv_cross_bin/spirv-cross','--msl','--msl-version','20300','--msl-argument-buffers',raw],check=True,capture_output=True,text=True).stdout
        (work/(key+'.metal')).write_text(source); res = json.loads((work/'apple'/(key+'.res.json')).read_text())
        if res['notes']: raise ValueError(res['notes'])
        refl = dict(stage='cs',threadgroup_size=[1,1,1],bindings=NS['reflect'](key,'cs',source,res));row['metal']={b['kind']:b['arg_id'] for b in refl['bindings']}
        vk.write_stage(key,(work/'vulkan'/(key+'.spv')).read_bytes(),refl,out,shutil.which('spirv-val'),shutil.which('spirv-opt'))
        bindings = [s.split() for s in (out/(key+'.vk.txt')).read_text().splitlines() if s.startswith('b ')]
        image = [b for b in bindings if b[2:6]==['simg','1','img','0']]; support = [b for b in bindings if b[2:5]==['ubo','1','support']]
        if len(image)!=1 or len(support)>1 or len(bindings)!=len(image)+len(support): raise ValueError(bindings)
        chan = row['channels']; is_float = row['format']=='R32Float'
        # Every integer transfer width must retain bits the narrower form would
        # lose. The four 32-bit channels also carry different upper halfwords.
        pattern = {'R8_SINT':(-101,1), 'R8_UINT':(129,1), 'R16_SINT':(-28000,81), 'R16_UINT':(0xa501,33)}
        first, step = pattern.get(row['vk_format'], (0x81538001,0x00130021))
        values = [struct.unpack('<f',struct.pack('<I',0x3f813005+i*0x3281))[0] if is_float else first+i*step for i in range(32*chan)]
        expected = values[:]; expected[22*chan:23*chan] = values[11*chan:12*chan]
        row.update(values=values, expected=expected)
        scalar = {'R32_SFLOAT':'f', 'R8_SINT':'b', 'R8_UINT':'B', 'R16_SINT':'h', 'R16_UINT':'H'}.get(row['vk_format'],'I')
        for name, data in (('initial', values), ('expected', expected)):
            (work/(key+'.'+name)).write_bytes(struct.pack(f'<{len(data)}{scalar}',*data))
        script = f'#!amber\nSHADER compute guest SPIRV-BIN FILE "{key}.spv"\n'
        script += f"IMAGE im DATA_TYPE {row['vk_format']} DIM_2D WIDTH 8 HEIGHT 4 DATA\n"+' '.join(map(str,values))+'\nEND\n'
        if support: script += 'BUFFER support DATA_TYPE float SIZE 1024 FILL 1.0\n'
        caps = vk.requirements((out/(key+'.spv')).read_bytes())['capabilities']
        for cap, feature in ((49,'shaderStorageImageExtendedFormats'), (55,'shaderStorageImageReadWithoutFormat'), (56,'shaderStorageImageWriteWithoutFormat')):
            if cap in caps: script += f'DEVICE_FEATURE {feature}\n'
        script += f'PIPELINE compute test\n  ATTACH guest\n  BIND BUFFER im AS storage_image DESCRIPTOR_SET 0 BINDING {image[0][1]}\n'
        if support: script += f'  BIND BUFFER support AS uniform DESCRIPTOR_SET 0 BINDING {support[0][1]}\n'
        script += 'END\nRUN test 1 1 1\nEXPECT im IDX 0 EQ '+' '.join(map(str,expected))+'\n'
        (out/(key+'.amber')).write_text(script)
    (work/'cases.json').write_text(json.dumps(rows,indent=2)+'\n'); return out


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__); parser.add_argument('directory',type=Path)
    print(generate(parser.parse_args().directory.resolve()))
