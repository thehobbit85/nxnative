"""Export a helper called with both wrapped-buffer and ordinary-image arguments."""
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[5]
sys.path.insert(0, str(ROOT))
from dm.android.tools import spv_android as vk

output = Path(sys.argv[1]).resolve()
source = Path(__file__).with_name('wrapped_buffer.comp')
raw = subprocess.check_output([os.environ.get('GLSLC', 'glslc'), '--target-env=vulkan1.1', '-DHELPERS', source, '-o', '-'])
assert any(vk.opcode(item) == 55 for item in vk.decode(raw)[1])
reflection = dict(stage='cs', threadgroup_size=[1, 1, 1], bindings=[
    dict(msl_name='source', kind='tex', reg=0, samp=dict(kind='samp_tex', reg=0), fetch1d=True),
    dict(msl_name='ordinary', kind='tex', reg=1, samp=dict(kind='samp_tex', reg=1)),
    dict(msl_name='result', kind='sbuf', reg=0), dict(msl_name='raw', kind='sbuf', reg=1)])
key = '00000000feed9e06'
vk.write_stage(key, raw, reflection, output, shutil.which('spirv-val'), shutil.which('spirv-opt'))
# The two calls now keep independent resource identities for runtime lowering.
assert not any(vk.opcode(item) in (55, 57) for item in vk.decode((output / (key + '.spv')).read_bytes())[1])
