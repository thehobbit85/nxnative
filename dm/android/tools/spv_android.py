"""Export SPIR-V and descriptor sidecars for the Android fork's NVN Vulkan executor."""
import argparse
import collections
import hashlib
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
sys.path.insert(0, str(ROOT))
from dm.android.tools.target_contract import shader_contract, stamp_export
from dm.android.tools.spv_module import decode, opcode, string_operand
from dm.android.tools.spv_capture import geometry_capture
from dm.android.tools.spv_bindless import driver_array_fields, split_handle_fields
from dm.android.tools import spv_resources
from dm.android.tools.spv_globalmem import global_memory_fields, lower_physical_addresses, drop_unused_int64
from dm.android.tools import spv_geometry, spv_meshstage, spv_pool, spv_texture_offsets
from dm.android.tools.spv_cache import capture_cache
TOOLS = ROOT / 'dm/gfx/tools'
XLATE = TOOLS / 'dmsh_xlate_bin/dmsh_xlate'


def capture(args):
    if args == ['--source-sha']:
        return subprocess.call([XLATE, *args])
    job, directory = map(Path, args)
    jobs = json.loads(job.read_text())
    directory.mkdir(parents=True, exist_ok=True)
    variants = [dict(item, key=item['key'] + f'.g{topology}', input_topology=topology)
                for item in jobs if item.get('stage') == 'gs' and 'geometry_topology' not in item
                for topology in range(5)]
    helpers = spv_geometry.compute_jobs(jobs)
    # The shared chain also translates through SPIR-V for Metal, whose allowed
    # operations differ. Request the existing translator's Vulkan lowering without
    # changing the caller's job or the normal Apple translation target.
    with tempfile.TemporaryDirectory(prefix='vk-job-', dir=directory) as temporary:
        targeted = Path(temporary) / 'jobs.json'
        targeted.write_text(json.dumps([dict(item, host_target='vulkan') for item in jobs + variants + helpers]))
        subprocess.run([XLATE, targeted, directory], check=True)
    for item in jobs + variants:
        name = item['key'] + '.spv'
        if (directory / name).is_file():
            shutil.copyfile(directory / name, Path(os.environ['DMSH_SPV_CAPTURE']) / name)
            # Keep the shared control-section decoder's complete feedback
            # layout. Metal reflection only records strides and cannot recover
            # interleaved outputs or an attribute captured into several buffers.
            # Its task/mesh summary similarly omits the full output maps.
            control = {key: item[key] for key in ('tf', 'mesh_stage', 'stage_memory') if key in item}
            (Path(os.environ['DMSH_SPV_CAPTURE']) / (item['key'] + '.control.json')).write_text(json.dumps(control))
    spv_geometry.capture_helpers(helpers, directory, Path(os.environ['DMSH_SPV_CAPTURE']))
    return 0


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
        name = names.get(x[2], '')
        resource = resources.get(name)
        count = 1
        while opcode(ty) == 28:
            count *= constants[ty[3]]
            ty = types[ty[2]]
        if opcode(ty) == 29:
            if not resource or resource.get('runtime') is not True or resource['kind'] not in spv_pool.COUNTS:
                raise ValueError('runtime array has no descriptor pool metadata')
            count = resource['pool_count']; ty = types[ty[2]]
        if resource is None:
            if name != 'support_buffer':
                raise ValueError(f'no guest resource for descriptor {binding} ({name!r})')
            resource = {'kind': 'support'}
        descriptor = descriptor_type(ty, types, dec, x[3])
        resource = spv_resources.slot_array(resource, descriptor, count)
        entry = dict(resource, binding=binding, count=count, type=descriptor)
        if entry['type'] in ('cis', 'tex', 'tbuf', 'utb', 'stb'):
            image = types[ty[2]] if entry['type'] in ('cis', 'tbuf') else ty
            # Vulkan image view type, including the array shape; used for views
            # and zero textures when an unselected bindless element is absent.
            if image[3] != 5:  # texel buffers use VkBufferView, not an image view
                entry['view'] = {(0, 0): 0, (1, 0): 1, (2, 0): 2, (3, 0): 3,
                                 (0, 1): 4, (1, 1): 5, (3, 1): 6}[image[3], image[5]]
            component = types[image[2]]
            entry['num'] = (1 if component[3] else 2) if opcode(component) == 21 else 0
        # GLSL storage blocks commonly put NonWritable on every member rather
        # than the variable. A partially read-only block still owns GPU writes.
        readonly_block = (opcode(ty) == 30 and len(ty) > 2 and
                          all(24 in member_dec[ty[1]][i] for i in range(len(ty) - 2)))
        if entry['type'] in ('simg', 'ssbo', 'stb') and (24 in dec[x[2]] or readonly_block):
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
    # The shared generator declares ImageGatherExtended unconditionally. The
    # SPIR-V grammar requires it for ImageOperands Offset/ConstOffsets, not for
    # ordinary gather or ConstOffset. Maxwell offsets often become coordinate
    # arithmetic instead, so keeping this declaration rejects capable devices.
    # Values are the word index of ImageOperands (including the opcode word).
    masks = {op: 5 for op in (87, 88, 91, 92, 95, 98, 305, 306, 309, 310, 313, 320)}
    masks.update({op: 6 for op in (89, 90, 93, 94, 96, 97, 307, 308, 311, 312, 314, 315)})
    masks.update({99: 4, 4545: 7, 5283: 7})  # write, QCOM gather, NV footprint
    if not any(len(x) > masks[opcode(x)] and x[masks[opcode(x)]] & 0x30
               for x in ins if opcode(x) in masks):
        drop.add(25)
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
    data = spv_texture_offsets.lower(data)
    data, physical = lower_physical_addresses(prepare_spirv(data))
    result = subprocess.run([optimizer, '--eliminate-dead-variables', '-', '-o', '-'],
                            input=data, stdout=subprocess.PIPE, check=True).stdout
    return drop_unused_int64(result) if physical else result


VERTEX_TYPES = {'float': 0, 'half': 0, 'uint': 1, 'ushort': 1, 'uchar': 1, 'int': 2, 'short': 2, 'char': 2}
RESOURCE_KINDS = {'cbuf', 'c1', 'support', 'drvcb', 'tex', 'tex_ubo', 'tex_c2a', 'samp_c2a', 'img', 'sbuf', 'sbuf_ubo', 'tbuf', 'ubo_ptr',
                  'samp', 'samp_ubo', 'samp_tex', 'samp_id', 'geometry', 'mesh', 'gmem_map', 'gmem_log',
                  'tex_pool', 'samp_pool', 'pool_bounds', 'tex_slots', 'samp_slots'}


def requirements(data):
    """Retain the emitted module's optional feature requirements verbatim."""
    _, instructions = decode(data)
    return dict(capabilities=sorted({x[1] for x in instructions if opcode(x) == 17}),
                extensions=sorted({string_operand(x[1:]) for x in instructions if opcode(x) == 10}))


def feedback_rows(layout, stage):
    """The shared tf_layout format, including repeated outputs and buffer order."""
    if stage not in ('vs', 'gs') or not isinstance(layout, list) or len(layout) != 4:
        raise ValueError('invalid feedback stage or buffer count')
    rows = []
    for buffer, entry in enumerate(layout):
        if entry == []: continue
        if not isinstance(entry, dict) or type(entry.get('stride')) is not int:
            raise ValueError('invalid feedback stride')
        outputs = entry.get('outputs')
        if not isinstance(outputs, list) or not 1 <= len(outputs) <= 128:
            raise ValueError('invalid feedback output count')
        for pair in outputs:
            if (not isinstance(pair, list) or len(pair) != 2 or
                    any(type(n) is not int for n in pair) or not 0 <= pair[0] < 32 or not 1 <= pair[1] <= 4):
                raise ValueError('invalid feedback location or component count')
        if entry['stride'] != 4 * sum(pair[1] for pair in outputs):
            raise ValueError('feedback stride differs from output layout')
        rows.append('tf ' + ' '.join(map(str, [buffer, entry['stride'], *(n for pair in outputs for n in pair)])))
    return rows


def vk_txt(meta):
    """Encode the stage, vertex inputs and guest source of each descriptor for the Vulkan executor."""
    # Shader packages include unused geometry/tessellation programs. Preserve
    # their real stages; exporting assets does not imply runtime stage support.
    if meta['stage'] not in ('vs', 'ps', 'cs', 'gs', 'hs', 'ds', 'ts', 'ms'):
        raise ValueError(f"unsupported Vulkan stage: {meta['stage']}")
    if meta.get('schema', 2) != 2:
        raise ValueError('incompatible Vulkan shader schema')
    lines = ['schema 2', 'target vulkan1.1', f"stage {meta['stage']}"]
    for cap in sorted(set(meta.get('capabilities', []))):
        if type(cap) is not int or not 0 <= cap <= 0xffffffff:
            raise ValueError('invalid SPIR-V capability')
        lines.append(f'cap {cap}')
    for ext in sorted(set(meta.get('extensions', []))):
        if not re.fullmatch(r'SPV_[A-Za-z0-9_]+', ext):
            raise ValueError('invalid SPIR-V extension')
        lines.append(f'ext {ext}')
    if meta.get('tg'):
        lines.append('tg ' + ' '.join(map(str, meta['tg'])))
    if meta.get('c1'):
        lines.append(f"c1 {meta['c1']}")
    if meta['stage'] in ('ts', 'ms'):
        lines += spv_meshstage.lines(meta.get('mesh_stage'), meta.get('mesh_control', {}), meta['stage'])
    elif 'mesh_stage' in meta or 'mesh_control' in meta:
        raise ValueError('mesh metadata on ordinary shader')
    if 'geometry' in meta:
        lines.append(spv_geometry.geometry_line(meta['geometry'], meta['stage']))
        if (meta['geometry']['role'] == 'capture') != ('capture' in meta):
            raise ValueError('geometry capture role needs its capture interface exclusively')
    if 'tf' in meta:
        lines += feedback_rows(meta['tf'], 'gs' if 'geometry' in meta else meta['stage'])
    if 'capture' in meta:
        capture = meta['capture']
        values = [capture.get(k) for k in ('words', 'control_words', 'output_binding', 'control_binding')]
        if (meta['stage'] != 'vs' or any(type(v) is not int for v in values) or values[:2] != [144, 6] or
                any(not 0 <= v <= 0x7fffffff for v in values[2:]) or values[2] == values[3]):
            raise ValueError('invalid vertex capture interface')
        lines.append('capture ' + ' '.join(map(str, values)))
    for vertex in meta.get('vertex_inputs', []):
        location, ty = vertex['location'], vertex['msl_type'].rstrip('1234')
        if ty not in VERTEX_TYPES or not 0 <= location < 32:
            raise ValueError(f'unsupported Vulkan vertex input {location} ({ty})')
        lines.append(f'vin {location} {VERTEX_TYPES[ty]}')
    if len(meta['bindings']) > spv_resources.MAX_BINDINGS:
        raise ValueError(f"Vulkan exports support at most {spv_resources.MAX_BINDINGS} descriptor bindings per stage")
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
        if sampler.get('kind') in ('samp_ubo', 'samp_c2a'):
            fields += (f"soff={sampler['off']}",)
        fields += driver_array_fields(bind)
        fields += global_memory_fields(bind)
        fields += spv_pool.fields(bind)
        for name in ('view', 'num'):
            if name in bind:
                fields += (f'{name}={bind[name]}',)
        fields += split_handle_fields(bind)
        fields += spv_resources.fields(bind)
        lines.append('b ' + ' '.join(map(str, fields)))
    return '\n'.join(lines) + '\n'


def capture_manifest(directory, keys, producer):
    files = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(directory.iterdir())}
    record = dict(schema=1, target='vulkan1.1', producer=hashlib.sha256(producer).hexdigest(), keys=keys, files=files)
    (directory / 'capture.json').write_text(json.dumps(record, indent=2, sort_keys=True) + '\n')


def read_prepared(directory, producer):
    """Read and verify immutable snapshots, never paths supplied by a capture manifest."""
    try:
        record = json.loads((directory / 'capture.json').read_text())
        keys = record['keys']
        if (record['schema'] != 1 or record['target'] != 'vulkan1.1' or
                record['producer'] != hashlib.sha256(producer).hexdigest() or
                not isinstance(keys, list) or not keys or
                any(not isinstance(k, str) or not re.fullmatch(r'[0-9a-f]{16}(?:\.(?:g|gc|gr)[0-4])?', k) for k in keys) or
                keys != sorted(set(keys))):
            raise ValueError('incompatible producer, target or shader keys')
        expected = {k + suffix for k in keys for suffix in ('.spv', '.reflection.json')}
        if set(record['files']) != expected or {p.name for p in directory.iterdir()} != expected | {'capture.json'}:
            raise ValueError('capture inventory differs')
        snapshots = {}
        for name in sorted(expected):
            path = directory / name
            if path.is_symlink() or not path.is_file(): raise ValueError('capture is missing a regular file: ' + name)
            data = path.read_bytes()
            if hashlib.sha256(data).hexdigest() != record['files'][name]: raise ValueError('capture content changed: ' + name)
            snapshots[name] = data
        for key in keys:
            reflection = json.loads(snapshots[key + '.reflection.json'])
            validate_stage(snapshots[key + '.spv'], reflection)
            validate_topology(key, snapshots[key + '.spv'], reflection)
            if reflection['stage'] == 'gs' or 'geometry' in reflection:
                base = key[:16]
                if (not set(spv_geometry.product_family(base)) <= set(keys) or
                        json.loads(snapshots[base + '.reflection.json'])['stage'] != 'gs'):
                    raise ValueError('incomplete geometry topology variants')
            elif len(key) != 16 and 'geometry' not in reflection:
                raise ValueError('topology variant is not a geometry stage')
        return keys, snapshots
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise ValueError('invalid prepared shader capture: ' + str(error)) from error


def validate_stage(data, reflection):
    if not isinstance(reflection, dict) or not isinstance(reflection.get('bindings'), list):
        raise ValueError('invalid shader reflection')
    _, instructions = decode(data)
    stages = {0: 'vs', 1: 'hs', 2: 'ds', 3: 'gs', 4: 'ps', 5: 'cs'}
    entries = [x for x in instructions if opcode(x) == 15]
    mesh = reflection.get('stage') in ('ts', 'ms')
    stage = 'cs' if mesh else reflection.get('stage')
    if len(entries) != 1 or len(entries[0]) < 4 or stages.get(entries[0][1]) != stage:
        raise ValueError('shader stage metadata does not match SPIR-V')
    if mesh:
        info = reflection.get('mesh_stage')
        if not isinstance(info, dict): raise ValueError('missing mesh stage metadata')
        spv_meshstage.layout(info, reflection.get('stage_memory'))
        if (info['kind'] != reflection['stage'] or reflection.get('threadgroup_size') != [info['threads'], 1, 1] or
                any(k in reflection for k in ('geometry', 'tf', 'capture'))):
            raise ValueError('invalid mesh logical stage or interface')
        spv_meshstage.validate_module(data, info, reflection.get('stage_memory'))
    elif 'mesh_stage' in reflection or 'stage_memory' in reflection:
        raise ValueError('mesh metadata on ordinary shader')
    if 'tf' in reflection:
        feedback_rows(reflection['tf'], 'gs' if 'geometry' in reflection else reflection['stage'])
    if 'geometry' in reflection:
        spv_geometry.geometry_line(reflection['geometry'], reflection['stage'])
        if reflection['geometry']['role'] == 'compute':
            sizes = [list(i[3:]) for i in instructions if opcode(i) == 16 and i[2] == 17]
            if sizes != [reflection.get('threadgroup_size')]:
                raise ValueError('geometry compute group size differs from module')


def validate_topology(key, data, reflection):
    helper = re.fullmatch(r'[0-9a-f]{16}\.(gc|gr)([0-4])', key)
    if helper:
        geometry = reflection.get('geometry', {})
        if (geometry.get('role') != ('compute' if helper[1] == 'gc' else 'reader') or
                geometry.get('topology') != int(helper[2])):
            raise ValueError('geometry helper topology or role differs from product name')
    elif 'geometry' in reflection:
        raise ValueError('geometry helper metadata on ordinary product')
    if re.fullmatch(r'[0-9a-f]{16}\.g[0-4]', key):
        _, instructions = decode(data)
        modes = [i[2] for i in instructions if opcode(i) == 16 and len(i) > 2 and 19 <= i[2] <= 23]
        if modes != [19 + int(key[-1])]:
            raise ValueError('geometry topology does not match product name')


def write_stage(key, raw, refl, destination, validator, optimizer):
    binary = destination / (key + '.spv')
    # Compact pool bindings before wrappers allocate their helper bindings;
    # otherwise their control metadata would retain the old sparse numbers.
    raw, refl = spv_pool.lower(raw, refl)
    raw = spv_resources.nonuniform_slots(raw, refl)
    mesh_control = None
    if refl['stage'] in ('ts', 'ms'):
        validate_stage(raw, refl)
        raw, mesh_control = spv_meshstage.lower(raw, refl['mesh_stage'], refl['stage_memory'])
        refl = dict(refl, bindings=refl['bindings'] + [dict(msl_name='dm_mesh_' + name, kind='mesh', reg=reg)
                                                     for reg, name in enumerate(('input', 'output', 'control'))])
    if 'geometry' in refl:
        role = refl['geometry']['role']
        raw = spv_geometry.topology_buffer(raw) if role == 'compute' else spv_geometry.viewport_mask_reader(raw)
    data = export_spirv(raw, optimizer)
    validate_stage(data, refl)
    validate_topology(key, data, refl)
    binary.write_bytes(data)
    subprocess.run([validator, '--target-env', 'vulkan1.1', binary], check=True)
    meta = {k: refl[k] for k in ('stage', 'c1', 'vertex_inputs', 'drv_reads', 'tf', 'geometry', 'mesh_stage') if k in refl}
    if mesh_control is not None: meta['mesh_control'] = mesh_control
    try:
        bindings = vk_reflect(data, refl)
    except ValueError as exc:
        raise ValueError(f'{key}: {exc}') from exc
    meta.update(key=key, tg=refl.get('threadgroup_size') if refl['stage'] in ('cs', 'ts', 'ms') else None,
                bindings=bindings, **requirements(data))
    (destination / (key + '.vk.txt')).write_text(vk_txt(meta))
    if refl['stage'] == 'vs':
        try:
            captured, control = geometry_capture(data)
        except ValueError as exc:
            raise ValueError(f'{key}: {exc}') from exc
        capture_path = destination / (key + '.vc.spv')
        capture_path.write_bytes(captured)
        subprocess.run([validator, '--target-env', 'vulkan1.1', capture_path], check=True)
        reflection = dict(refl, bindings=refl['bindings'] + [
            dict(msl_name='dm_geometry_output', kind='geometry', reg=0),
            dict(msl_name='dm_geometry_control', kind='geometry', reg=5)])
        capture_meta = meta.copy()
        capture_meta.update(capture=control, bindings=vk_reflect(captured, reflection), **requirements(captured))
        if 'geometry' in meta:
            capture_meta['geometry'] = dict(meta['geometry'], role='capture')
        (destination / (key + '.vc.vk.txt')).write_text(vk_txt(capture_meta))
    return meta['stage']


def export_prepared(directory, out, keys=None):
    if out.exists(): raise ValueError(f'output already exists: {out}')
    producer = shader_contract()
    captured, snapshots = read_prepared(directory, producer)
    selected = sorted(set(keys)) if keys else captured
    if not set(selected) <= set(captured): raise ValueError('requested shader is absent from prepared capture')
    # Selecting a program also selects its complete native topology family.
    selected = sorted({k for base in selected for k in captured if k == base or k.startswith(base + '.g')})
    validator, optimizer = shutil.which('spirv-val'), shutil.which('spirv-opt')
    if not validator or not optimizer: raise ValueError('spirv-val and spirv-opt from SPIRV-Tools are required')
    out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='vk-export-', dir=out.parent) as tmp:
        dest = Path(tmp) / 'result'; dest.mkdir()
        for key in selected:
            reflection = json.loads(snapshots[key + '.reflection.json'])
            write_stage(key, snapshots[key + '.spv'], reflection, dest, validator, optimizer)
        stamp_export(dest, producer=producer)
        dest.rename(out)
    print(f'Exported {len(selected)} validated Vulkan shader stages from prepared capture to {out}')


def export(pkg, keys, out, *, prepare_only=False):
    if not keys or any(not re.fullmatch('[0-9a-f]{16}', key) for key in keys):
        raise ValueError('expected one or more 16-digit lowercase shader keys')
    if out.exists():
        raise ValueError(f'output already exists: {out}')
    validator, optimizer = shutil.which('spirv-val'), shutil.which('spirv-opt')
    if not prepare_only and (not validator or not optimizer):
        raise ValueError('spirv-val and spirv-opt from SPIRV-Tools are required')
    producer = shader_contract()
    out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='vk-build-', dir=out.parent) as tmp:
        work = Path(tmp).resolve()
        captured, cache, dest, xlate_dir = (work / n for n in ('raw', 'cache', 'result', 'translator'))
        for d in (captured, dest, xlate_dir):
            d.mkdir()
        # Only compiled Metal libraries are shared. The shader/metadata cache
        # must remain private: a cache hit there skips raw SPIR-V capture and
        # does not distinguish the wrapper's Vulkan target from Apple's job.
        cas = work / 'cas.db'
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
        with capture_cache(cas, out.parent / 'vk-metallib.db'):
            for i in range(0, len(keys), 512):
                subprocess.run([sys.executable, TOOLS / 'own_compilers.py', pkg, cache, *keys[i:i + 512]], env=env, check=True)
        libraries, = cache.glob('b-*')
        stage_counts = collections.Counter()
        products = []
        for key in keys:
            base = json.loads((libraries / (key + '_b.json')).read_text())
            family = spv_geometry.product_family(key) if base['stage'] == 'gs' else [key]
            for product in family:
                control = json.loads((captured / (product + '.control.json')).read_text())
                refl = spv_geometry.helper_reflection(base, control)
                spv = captured / (product + '.spv')
                if prepare_only:
                    validate_stage(spv.read_bytes(), refl)
                    shutil.copyfile(spv, dest / (product + '.spv'))
                    (dest / (product + '.reflection.json')).write_text(json.dumps(refl, sort_keys=True) + '\n')
                else:
                    stage_counts[write_stage(product, spv.read_bytes(), refl, dest, validator, optimizer)] += 1
                products.append(product)
        for stage in ('gs', 'hs', 'ds', 'ts', 'ms'):
            if stage_counts[stage]:
                print(f'[RED] Vulkan runtime does not execute {stage}: preserved {stage_counts[stage]} shader assets; games using them still require that feature', flush=True)
        if prepare_only:
            if shader_contract() != producer: raise ValueError('shader inputs changed while preparing capture')
            capture_manifest(dest, sorted(products), producer)
        else:
            stamp_export(dest, producer=producer)
        dest.rename(out)
    if prepare_only:
        print(f'Prepared {len(keys)} Vulkan shader stages for portable export to {out}')
        return
    print(f'Exported {len(keys)} validated Vulkan shader stages to {out}')


def main():
    if sys.argv[1:2] == ['--capture']:
        return capture(sys.argv[2:])
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pkg', type=Path, help='the extracted NVN shader package')
    parser.add_argument('out', type=Path, help='a new output directory on the game drive')
    parser.add_argument('keys', nargs='*', help='individual shader keys')
    parser.add_argument('--keys-file', type=Path, help='additional keys, one per line')
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--prepare-only', action='store_true', help='capture shared shader preparation on macOS for later portable export')
    mode.add_argument('--from-prepared', action='store_true', help='export a verified capture using only Python and SPIRV-Tools')
    args = parser.parse_args()
    try:
        keys = args.keys + (args.keys_file.read_text().split() if args.keys_file else [])
        if args.from_prepared:
            export_prepared(args.pkg.resolve(), args.out.resolve(), keys)
        else:
            export(args.pkg.resolve(), keys, args.out.resolve(), prepare_only=args.prepare_only)
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        sys.exit(f'[RED] Vulkan export failed: {exc}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
