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
    jobs = json.loads(job.read_text())
    directory.mkdir(parents=True, exist_ok=True)
    # The shared chain also translates through SPIR-V for Metal, whose allowed
    # operations differ. Request the existing translator's Vulkan lowering without
    # changing the caller's job or the normal Apple translation target.
    with tempfile.TemporaryDirectory(prefix='vk-job-', dir=directory) as temporary:
        targeted = Path(temporary) / 'jobs.json'
        targeted.write_text(json.dumps([dict(item, host_target='vulkan') for item in jobs]))
        subprocess.run([XLATE, targeted, directory], check=True)
    for item in jobs:
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
    member_dec = collections.defaultdict(lambda: collections.defaultdict(set))
    for x in ins:
        if opcode(x) == 71:
            dec[x[1]][x[2]] = x[3] if len(x) > 3 else True
        elif opcode(x) == 72:
            member_dec[x[1]][x[2]].add(x[3])
    types = {x[1]: x for x in ins if opcode(x) in (21, 22, 25, 26, 27, 28, 29, 30, 32)}
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
        if entry['type'] in ('cis', 'tex'):
            image = types[ty[2]] if entry['type'] == 'cis' else ty
            # Vulkan image view type, including the array shape; used for views
            # and zero textures when an unselected bindless element is absent.
            entry['view'] = {(0, 0): 0, (1, 0): 1, (2, 0): 2, (3, 0): 3,
                             (0, 1): 4, (1, 1): 5, (3, 1): 6}[image[3], image[5]]
            component = types[image[2]]
            entry['num'] = (1 if component[3] else 2) if opcode(component) == 21 else 0
        # GLSL storage blocks commonly put NonWritable on every member rather
        # than the variable. A partially read-only block still owns GPU writes.
        readonly_block = (opcode(ty) == 30 and len(ty) > 2 and
                          all(24 in member_dec[ty[1]][i] for i in range(len(ty) - 2)))
        if entry['type'] in ('simg', 'ssbo') and (24 in dec[x[2]] or readonly_block):
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


def export_spirv(data, optimizer):
    """Remove globals absent from Metal reflection because the shader never reads them."""
    # Keep live descriptor numbers and shader operations unchanged. The raw
    # translator can retain unused texture declarations that SPIRV-Cross drops
    # from Metal's argument buffer (Link's Awakening's uber shaders).
    return subprocess.run([optimizer, '--eliminate-dead-variables', '-', '-o', '-'],
                          input=prepare_spirv(data), stdout=subprocess.PIPE, check=True).stdout


VERTEX_TYPES = {'float': 0, 'half': 0, 'uint': 1, 'ushort': 1, 'uchar': 1, 'int': 2, 'short': 2, 'char': 2}
RESOURCE_KINDS = {'cbuf', 'c1', 'support', 'drvcb', 'tex', 'tex_ubo', 'img', 'sbuf', 'tbuf', 'ubo_ptr',
                  'samp', 'samp_ubo', 'samp_tex', 'samp_id'}


def vk_txt(meta):
    """Encode the stage, vertex inputs and guest source of each descriptor for the Vulkan executor."""
    # Shader packages include unused geometry/tessellation programs. Preserve
    # their real stages; exporting assets does not imply runtime stage support.
    if meta['stage'] not in ('vs', 'ps', 'cs', 'gs', 'hs', 'ds'):
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
        offset = bind.get('off', bind.get('bank', -1))
        if bind['kind'] == 'drvcb' and 'bank' in bind:
            offset = bind['bank']
        fields = (bind['binding'], bind['type'], bind['count'], bind['kind'], -1 if reg is None else reg,
                  offset, sampler.get('kind', '-'), -1 if sreg is None else sreg)
        if bind.get('read_only'):
            fields += ('ro',)
        if sampler.get('kind') == 'samp_ubo':
            fields += (f"soff={sampler['off']}",)
        for name in ('view', 'num'):
            if name in bind:
                fields += (f'{name}={bind[name]}',)
        lines.append('b ' + ' '.join(map(str, fields)))
    return '\n'.join(lines) + '\n'


def export(pkg, keys, out):
    if not keys or any(not re.fullmatch('[0-9a-f]{16}', key) for key in keys):
        raise ValueError('expected one or more 16-digit lowercase shader keys')
    if out.exists():
        raise ValueError(f'output already exists: {out}')
    validator, optimizer = shutil.which('spirv-val'), shutil.which('spirv-opt')
    if not validator or not optimizer:
        raise ValueError('spirv-val and spirv-opt from SPIRV-Tools are required')
    out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='vk-build-', dir=out.parent) as tmp:
        work = Path(tmp).resolve()
        captured, cache, dest, xlate_dir = (work / n for n in ('raw', 'cache', 'result', 'translator'))
        for d in (captured, dest, xlate_dir):
            d.mkdir()
        # Only compiled Metal libraries are shared. The shader/metadata cache
        # must remain private: a cache hit there skips raw SPIR-V capture and
        # does not distinguish the wrapper's Vulkan target from Apple's job.
        cas = work / 'cas'
        cas.mkdir()
        metal_cache = out.parent / 'vk-metallib'
        metal_cache.mkdir(exist_ok=True)
        (cas / 'metallib').symlink_to(metal_cache.resolve(), target_is_directory=True)
        for src in XLATE.parent.iterdir():
            if src != XLATE:
                (xlate_dir / src.name).symlink_to(src)
        wrapper = xlate_dir / XLATE.name
        wrapper.write_text('#!/bin/sh\nexec ' + shlex.join([sys.executable, str(Path(__file__).resolve()), '--capture']) + ' "$@"\n')
        wrapper.chmod(0o700)
        # Reuse compiled Metal output by its content hash, including the normal
        # Apple build's libraries. Translation and raw SPIR-V capture stay fresh:
        # the temporary reflection cache must use the Vulkan target's job.
        env = dict(os.environ, XLATE=str(wrapper), WORK=str(work), DMSH_SPV_CAPTURE=str(captured),
                   DMSH_CAS=str(cas),
                   DMSH_PREFILL=str(out.parent / 'cache'), DMSH_TARGETS='mac')
        env.pop('IOSOUT', None)
        keys = sorted(set(keys))
        for i in range(0, len(keys), 512):
            subprocess.run([sys.executable, TOOLS / 'own_compilers.py', pkg, cache, *keys[i:i + 512]], env=env, check=True)
        libraries, = cache.glob('b-*')
        stage_counts = collections.Counter()
        for key in keys:
            spv = captured / (key + '.spv')
            refl = json.loads((libraries / (key + '_b.json')).read_text())
            binary = dest / (key + '.spv')
            data = export_spirv(spv.read_bytes(), optimizer)
            binary.write_bytes(data)
            subprocess.run([validator, '--target-env', 'vulkan1.1', binary], check=True)
            meta = {k: refl[k] for k in ('stage', 'c1', 'vertex_inputs', 'drv_reads') if k in refl}
            try:
                bindings = vk_reflect(data, refl)
            except ValueError as exc:
                raise ValueError(f'{key}: {exc}') from exc
            meta.update(key=key, tg=refl.get('threadgroup_size'), bindings=bindings)
            stage_counts[meta['stage']] += 1
            (dest / (key + '.vk.txt')).write_text(vk_txt(meta))
        for stage in ('gs', 'hs', 'ds'):
            if stage_counts[stage]:
                print(f'[RED] Vulkan runtime does not execute {stage}: preserved {stage_counts[stage]} shader assets; games using them still require that feature', flush=True)
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
