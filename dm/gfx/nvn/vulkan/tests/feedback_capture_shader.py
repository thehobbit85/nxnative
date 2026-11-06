"""Build the replay fixture's ordinary production vertex-capture variant."""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[5]))
from dm.android.tools.spv_capture import geometry_capture

source, dest = map(Path, sys.argv[1:])
data, capture = geometry_capture(source.read_bytes())
dest.write_bytes(data)
output, control = capture['output_binding'], capture['control_binding']
dest.with_suffix('.vk.txt').write_text(source.with_suffix('.vk.txt').read_text() +
    f'capture 144 6 {output} {control}\nb {output} ssbo 1 geometry 0 -1 - -1\nb {control} ubo 1 geometry 5 -1 - -1\n')
