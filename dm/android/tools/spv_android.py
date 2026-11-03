"""Export SPIR-V and descriptor sidecars for the Android fork's NVN Vulkan executor."""
import argparse
import collections
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
TOOLS = ROOT / 'dm/gfx/tools'
XLATE = TOOLS / 'dmsh_xlate_bin/dmsh_xlate'


def capture(args):
    if args == ['--source-sha']:
        return subprocess.call([XLATE, *args])
    job, directory = map(Path, args)
    subprocess.run([XLATE, job, directory], check=True)
    for item in json.loads(job.read_text()):
        name = item['key'] + '.spv'
        if (directory / name).is_file():
            shutil.copyfile(directory / name, Path(os.environ['DMSH_SPV_CAPTURE']) / name)
    return 0


def decode(data):
    if len(data) < 20 or len(data) % 4:
        raise ValueError('truncated SPIR-V module')
    words = struct.unpack(f'<{len(data) // 4}I', data)
    if words[0] != 0x07230203:
        raise ValueError('invalid SPIR-V magic')
    at = 5
    instructions = []
    while at < len(words):
        count = words[at] >> 16
        if not count or at + count > len(words):
            raise ValueError(f'invalid SPIR-V instruction at word {at}')
        instructions.append(words[at:at + count])
        at += count
    return words[:5], instructions


def opcode(instruction):
    return instruction[0] & 0xffff


def string_operand(words):
    return struct.pack(f'<{len(words)}I', *words).split(b'\0', 1)[0].decode()


def descriptor_type(ty, types, decorations, storage):
    kind = opcode(ty)
    if kind == 27:  # OpTypeSampledImage
        return 'tbuf' if types[ty[2]][3] == 5 else 'cis'
    if kind == 25:  # OpTypeImage: DimBuffer = 5, Sampled = 1 and Storage = 2
        if ty[3] == 5:
            return 'utb' if ty[7] == 1 else 'stb'
        return 'simg' if ty[7] == 2 else 'tex'
    if kind == 26:
        return 'samp'
    if kind == 30:
        return 'ssbo' if storage == 12 or 3 in decorations[ty[1]] else 'ubo'
    raise ValueError(f'unsupported descriptor type opcode {kind}')


def vk_reflect(data, reflection):
    """Match the binary's set-zero descriptor bindings to the translator's guest resource metadata."""
    _, ins = decode(data)
    names = {x[1]: string_operand(x[2:]) for x in ins if opcode(x) == 5}
    dec = collections.defaultdict(dict)
    for x in ins:
        if opcode(x) == 71:
            dec[x[1]][x[2]] = x[3] if len(x) > 3 else True
    types = {x[1]: x for x in ins if opcode(x) in (25, 26, 27, 28, 29, 30, 32)}
    constants = {x[2]: x[3] for x in ins if opcode(x) == 43}
    resources = {b['msl_name']: b for b in reflection['bindings']}
    bindings = {}
    for x in ins:
        if opcode(x) != 59 or x[3] not in (0, 2, 12) or 33 not in dec[x[2]]:
            continue
        binding = dec[x[2]][33]
        if dec[x[2]].get(34) != 0 or binding in bindings:
            raise ValueError(f'unsupported descriptor set or duplicate binding {binding}')
        ty = types[types[x[1]][3]]
        count = 1
        while opcode(ty) == 28:
            count *= constants[ty[3]]
            ty = types[ty[2]]
        name = names.get(x[2], '')
        resource = resources.get(name)
        if resource is None:
            if name != 'support_buffer':
                raise ValueError(f'no guest resource for descriptor {binding} ({name!r})')
            resource = {'kind': 'support'}
        entry = dict(resource, binding=binding, count=count, type=descriptor_type(ty, types, dec, x[3]))
        if entry['type'] == 'simg' and 24 in dec[x[2]]:  # NonWritable on the descriptor variable
            entry['read_only'] = True
        if name + 'Smplr' in resources:
            entry['samp'] = resources[name + 'Smplr']
        bindings[binding] = entry
    return [bindings[k] for k in sorted(bindings)]


def prepare_spirv(data):
    """Retain capabilities used by guest operations and remove Metal-only unused declarations."""
    header, ins = decode(data)
    f64 = any(opcode(x) == 22 and x[2] == 64 for x in ins)
    interlock = any(opcode(x) in (5364, 5365) for x in ins)
    drop = set() if f64 else {10}  # Float64
    if not interlock:
        drop.update((5363, 5372, 5378))
    # The translator can emit OpImageWrite to an Unknown-format storage image
    # without declaring StorageImageWriteWithoutFormat (Legends Z-A). Only
    # require the feature for a shader that actually writes such a loaded image.
    unformatted = {x[1] for x in ins if opcode(x) == 25 and x[7] == 2 and x[8] == 0}
    loads = {x[2] for x in ins if opcode(x) == 61 and x[1] in unformatted}
    writes = any(opcode(x) == 99 and x[1] in loads for x in ins)
    words = list(header)
    if writes and not any(opcode(x) == 17 and x[1] == 56 for x in ins):
        words.extend((2 << 16 | 17, 56))
    for x in ins:
        op = opcode(x)
        if op == 17 and x[1] in drop:
            continue
        if not interlock and op == 10 and string_operand(x[1:]) == 'SPV_EXT_fragment_shader_interlock':
            continue
        if not interlock and op == 16 and x[2] in (5366, 5367, 5368, 5369, 5370, 5371):
            continue
        words.extend(x)
    return struct.pack(f'<{len(words)}I', *words)


VERTEX_TYPES = {'float': 0, 'half': 0, 'uint': 1, 'ushort': 1, 'uchar': 1, 'int': 2, 'short': 2, 'char': 2}
RESOURCE_KINDS = {'cbuf', 'c1', 'support', 'drvcb', 'tex', 'img', 'sbuf', 'tbuf', 'ubo_ptr'}


def vk_txt(meta):
    """Encode the stage, vertex inputs and guest source of each descriptor for the Vulkan executor."""
    if meta['stage'] not in ('vs', 'ps', 'cs'):
        raise ValueError(f"unsupported Vulkan stage: {meta['stage']}")
    lines = [f"stage {meta['stage']}"]
    if meta.get('tg'):
        lines.append('tg ' + ' '.join(map(str, meta['tg'])))
    if meta.get('c1'):
        lines.append(f"c1 {meta['c1']}")
    for vertex in meta.get('vertex_inputs', []):
        location, ty = vertex['location'], vertex['msl_type'].rstrip('1234')
        if ty not in VERTEX_TYPES or not 0 <= location < 32:
            raise ValueError(f'unsupported Vulkan vertex input {location} ({ty})')
        lines.append(f'vin {location} {VERTEX_TYPES[ty]}')
    if len(meta['bindings']) > 64:
        raise ValueError('Vulkan executor supports at most 64 descriptor bindings per stage')
    for bind in meta['bindings']:
        if bind['kind'] not in RESOURCE_KINDS:
            raise ValueError(f"unsupported guest binding kind: {bind['kind']}")
        sampler = bind.get('samp', {})
        reg, sreg = bind.get('reg'), sampler.get('reg')
        fields = (bind['binding'], bind['type'], bind['count'], bind['kind'], -1 if reg is None else reg,
                  bind.get('off', bind.get('bank', -1)), sampler.get('kind', '-'), -1 if sreg is None else sreg)
        if bind.get('read_only'):
            fields += ('ro',)
        lines.append('b ' + ' '.join(map(str, fields)))
    return '\n'.join(lines) + '\n'


def export(pkg, keys, out):
    if not keys or any(not re.fullmatch('[0-9a-f]{16}', key) for key in keys):
        raise ValueError('expected one or more 16-digit lowercase shader keys')
    if out.exists():
        raise ValueError(f'output already exists: {out}')
    validator = shutil.which('spirv-val')
    if not validator:
        raise ValueError('spirv-val from SPIRV-Tools is required')
    out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='vk-build-', dir=out.parent) as tmp:
        work = Path(tmp).resolve()
        captured, cache, dest, xlate_dir = (work / n for n in ('raw', 'cache', 'result', 'translator'))
        for d in (captured, dest, xlate_dir):
            d.mkdir()
        for src in XLATE.parent.iterdir():
            if src != XLATE:
                (xlate_dir / src.name).symlink_to(src)
        wrapper = xlate_dir / XLATE.name
        wrapper.write_text('#!/bin/sh\nexec ' + shlex.join([sys.executable, str(Path(__file__).resolve()), '--capture']) + ' "$@"\n')
        wrapper.chmod(0o700)
        env = dict(os.environ, XLATE=str(wrapper), WORK=str(work), DMSH_SPV_CAPTURE=str(captured),
                   DMSH_CAS=str(work / 'cas'), DMSH_PREFILL='')
        env.pop('IOSOUT', None)
        keys = sorted(set(keys))
        for i in range(0, len(keys), 512):
            subprocess.run([sys.executable, TOOLS / 'own_compilers.py', pkg, cache, *keys[i:i + 512]], env=env, check=True)
        libraries, = cache.glob('b-*')
        for key in keys:
            spv = captured / (key + '.spv')
            refl = json.loads((libraries / (key + '_b.json')).read_text())
            binary = dest / (key + '.spv')
            data = prepare_spirv(spv.read_bytes())
            binary.write_bytes(data)
            subprocess.run([validator, '--target-env', 'vulkan1.1', binary], check=True)
            meta = {k: refl[k] for k in ('stage', 'c1', 'vertex_inputs', 'drv_reads') if k in refl}
            meta.update(key=key, tg=refl.get('threadgroup_size'), bindings=vk_reflect(data, refl))
            (dest / (key + '.vk.txt')).write_text(vk_txt(meta))
        dest.rename(out)
    print(f'Exported {len(keys)} validated Vulkan shader stages to {out}')


def main():
    if sys.argv[1:2] == ['--capture']:
        return capture(sys.argv[2:])
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pkg', type=Path, help='the extracted NVN shader package')
    parser.add_argument('out', type=Path, help='a new output directory on the game drive')
    parser.add_argument('keys', nargs='*', help='individual shader keys')
    parser.add_argument('--keys-file', type=Path, help='additional keys, one per line')
    args = parser.parse_args()
    try:
        keys = args.keys + (args.keys_file.read_text().split() if args.keys_file else [])
        export(args.pkg.resolve(), keys, args.out.resolve())
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        sys.exit(f'[RED] Vulkan export failed: {exc}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
