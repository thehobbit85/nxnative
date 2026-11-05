"""Attach the compiled fixture module's requirements to its hand-authored guest bindings."""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[4] / 'android/tools'))
from spv_android import requirements

for binary in Path(sys.argv[1]).glob('*.spv'):
    sidecar = binary.with_suffix('.vk.txt')
    body = [line for line in sidecar.read_text().splitlines() if not line.startswith(('schema ', 'target ', 'cap ', 'ext '))]
    req = requirements(binary.read_bytes())
    lines = ['schema 2', 'target vulkan1.1'] + [f'cap {n}' for n in req['capabilities']] + [f'ext {e}' for e in req['extensions']]
    sidecar.write_text('\n'.join(lines + body) + '\n')
