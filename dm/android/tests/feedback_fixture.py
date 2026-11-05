"""Compile the Vulkan feedback gather and the shared Apple vs_tf reference."""
import argparse
import ast
from pathlib import Path
import re
import subprocess
import sys
from typing import Any

ROOT = Path(__file__).resolve().parents[3]


def feedback_entry(source, layout, ordered):
    builder = (ROOT / 'dm/gfx/tools/dmsh_build.py').read_text()
    node = next(n for n in ast.parse(builder).body if isinstance(n, ast.FunctionDef) and n.name == 'vs_tf')
    helper = ast.get_source_segment(builder, node)
    assert helper is not None
    namespace: dict[str, Any] = {'re': re, 'sys': sys, 'TF_SLOT': 8}
    exec(helper, namespace)
    return namespace['vs_tf'](source, layout, ordered)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--metal', action='store_true')
    args = parser.parse_args(); work = args.directory.resolve(); work.mkdir(parents=True, exist_ok=True)
    shader = work / 'feedback_gather.spv'
    subprocess.run(['glslc', '--target-env=vulkan1.1', ROOT / 'dm/gfx/nvn/vulkan/feedback_gather.comp', '-o', shader], check=True)
    subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', shader], check=True)
    if not args.metal: return
    fields = ''.join(f'    float4 a{i} [[user(locn{i})]];\n' for i in range(32))
    body = ''.join(f'    out.a{i} = as_type<float4>(uint4(source[vid*144+{16+i*4}], source[vid*144+{17+i*4}], '
                   f'source[vid*144+{18+i*4}], source[vid*144+{19+i*4}]));\n' for i in range(32))
    source = ('#include <metal_stdlib>\nusing namespace metal;\nstruct main0_out\n{\n' + fields + '};\n'
              'vertex main0_out main0(uint vid [[vertex_id]], device const uint* source [[buffer(0)]])\n{\n'
              '    main0_out out = {};\n' + body + '    return out;\n}\n')
    layouts = [[dict(stride=36, outputs=[[7, 2], [0, 3], [7, 4]]), dict(stride=4, outputs=[[31, 1]]), [],
                dict(stride=20, outputs=[[0, 1], [31, 4]])],
               [dict(stride=2048, outputs=[[31, 4]] * 128), [], [], dict(stride=16, outputs=[[0, 4]])]]
    for layout, tf in enumerate(layouts):
        for ordered in (0, 1):
            path = work / f'feedback{layout}_{ordered}.metal'
            path.write_text(source[:source.index('vertex main0_out')] + feedback_entry(source, tf, bool(ordered)))
            subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-c', '-w', '-fmodules-cache-path=' + str(work / 'modules'),
                            path, '-o', path.with_suffix('.air')], check=True)
            subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', path.with_suffix('.air'), '-o', path.with_suffix('.metallib')], check=True)


if __name__ == '__main__': main()
