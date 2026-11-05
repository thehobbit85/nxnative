"""Portable producer/target contracts for Android host libraries and APKs."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
from dm.games import game
from dm.android.tools.program_inputs import program_inputs, program_root

START = b'NXNATIVE_ANDROID_HOST_V1\n'
END = b'NXNATIVE_ANDROID_HOST_END\n'


def digest(path):
    h = hashlib.sha256()
    with open(path, 'rb') as stream:
        while block := stream.read(1 << 20): h.update(block)
    return h.hexdigest()


def rows_bytes(rows):
    if any(any(c in str(value) for c in '\t\r\n') for row in rows for value in row):
        raise ValueError('invalid target contract field')
    return ''.join('\t'.join(map(str, row)) + '\n' for row in rows).encode()


def exporter_digest():
    # Capture/export helpers evolve independently of the entry point. Hash
    # their contents and relative names so a moved checkout has the same ABI,
    # while edits or newly added lowering modules cannot reuse an old producer.
    paths = set((ROOT / 'dm/android/tools').rglob('*.py'))
    paths.add(ROOT / 'dm/gfx/nvn/vulkan/shader_contract.h')
    paths.add(ROOT / 'dm/gfx/nvn/vulkan/geometry_contract.h')
    paths.add(ROOT / 'dm/gfx/nvn/vulkan/meshstage_contract.h')
    h = hashlib.sha256()
    for path in sorted(paths):
        h.update(str(path.relative_to(ROOT)).encode() + b'\0')
        h.update(path.read_bytes() if path.is_file() else b'missing')
        h.update(b'\0')
    return h.hexdigest()


def host_contract(profile=None, *, programs=True):
    g = game.load(profile)
    match = re.search(r'#define DMSH_CHAIN "([a-f0-9]+)"', (ROOT / 'dm/gfx/nvn/dmsh_chain.h').read_text())
    if not match: raise ValueError('missing shared shader chain version')
    chain = match.group(1)
    # The generated header's first comment names an absolute path for derived
    # profiles. Hash the actual definitions so relocating identical inputs does
    # not change the portable contract.
    header = '\n'.join(line for line in (game.header(g) if programs else game.header_one(g)).splitlines() if not line.startswith('//'))
    rows = [('schema', 1), ('platform', 'android'), ('arch', 'arm64-v8a'), ('host_abi', 1), ('tp_slot', 2),
            ('shader_schema', 2), ('shader_chain', chain), ('exporter', exporter_digest()),
            ('profile', g.id), ('profile_header', hashlib.sha256(header.encode()).hexdigest()), ('feature', 'guest-nvn')]
    profiles = [(0, g)] + [(int(n), game.load(gid)) for n, gid in g.pairs('programs')] if programs and g.get('programs', '') else [(0, g)]
    if len(profiles) > 1:
        rows += [('program_profile', index, sub.id) for index, sub in profiles]
    for _, sub in profiles:
        for path in sorted(sub.dir.iterdir()):
            if path.is_file() and (path.suffix == '.tsv' or path.name == 'patches.py') and path.name not in ('c1_full.tsv', 'cb_extent.tsv'):
                rows.append(('profile_input', sub.id + '/' + path.name, digest(path)))
    return rows_bytes(rows)


def shader_contract(profile=None):
    # Each export precedes preparation of the remaining programs. Its producer
    # depends only on its own profile; the final native host binds all of them.
    return host_contract(profile, programs=False)


def host_record(data):
    if data.count(START) != 1 or data.count(END) != 1:
        raise ValueError('missing or ambiguous Android host contract; rebuild the native host')
    start = data.index(START) + len(START); end = data.index(END)
    if end <= start: raise ValueError('invalid Android host contract; rebuild the native host')
    return data[start:end]


def stamp_export(directory, profile=None, producer=None):
    directory = Path(directory)
    current = shader_contract(profile)
    if producer is not None and producer != current:
        raise ValueError('Android shader inputs changed while exporting; retry with stable inputs')
    files = {p.name: digest(p) for p in sorted(directory.iterdir()) if p.suffix == '.spv' or p.name.endswith('.vk.txt')}
    record = {'schema': 1, 'producer': hashlib.sha256(current).hexdigest(), 'files': files}
    (directory / 'export-contract.json').write_text(json.dumps(record, sort_keys=True, indent=2) + '\n')


def target_manifest(a, lib, files):
    expected = host_contract(a.profile or a.id)
    if host_record(Path(lib).read_bytes()) != expected:
        raise ValueError('Android host contract differs from selected profile/tools; rebuild the native host')
    programs = program_inputs(a)
    g = game.load(a.profile or a.id)
    declared = {0: g.id}
    if g.get('programs', ''): declared.update((int(n), gid) for n, gid in g.pairs('programs'))
    if {index: game.load(item['profile']).id for index, item in programs.items()} != declared:
        raise ValueError('Android prepared inputs do not match declared programs')
    rows = [('title', a.id.upper()), ('host_sha256', digest(lib))]
    assets = {name: (Path(source), digest(source)) for name, source in files}
    for name, (path, sha) in sorted(assets.items()):
        rows.append(('asset', name, path.stat().st_size, sha))
        if name.startswith('programs/') and not any(name.startswith(program_root(i)) for i in programs if i):
            raise ValueError('asset belongs to an undeclared Android program: ' + name)
    for index, item in sorted(programs.items()):
        prefix = program_root(index)
        profile = game.load(item['profile'])
        if len(programs) > 1: rows.append(('program', index, profile.id, prefix.rstrip('/') or '.'))
        shaders = {name[len(prefix) + 3:]: sha for name, (_, sha) in assets.items() if name.startswith(prefix + 'vk/')}
        proof = 'empty'
        if shaders:
            path = Path(item['build']) / 'shader/vk/export-contract.json'
            try:
                saved = json.loads(path.read_text())
                if saved != {'schema': 1, 'producer': hashlib.sha256(shader_contract(item['profile'])).hexdigest(), 'files': shaders}:
                    raise ValueError('stale producer or products')
            except (OSError, ValueError) as error:
                raise ValueError('Android shader provenance missing or incompatible; export shaders again') from error
            proof = digest(path)
        rows.append(('program_shader', index, proof) if index else ('shader_export', proof))
        modules = {}
        for name, (_, sha) in sorted(assets.items()):
            if not name.startswith(prefix + 'dm/') or not name.endswith('.patched.bin'): continue
            module = name[len(prefix) + 3:-12]; fixup = assets.get(prefix + 'dm/' + module + '.fixups.tsv')
            text = fixup[0].read_text() if fixup else ''
            bid = re.findall(r'^# BUILD_ID\t([0-9a-fA-F]{16,64})$', text, re.M)
            tp = re.findall(r'^# TP_SLOT\t(\d+)$', text, re.M)
            if fixup is None or len(bid) != 1 or tp != ['2']:
                raise ValueError('Android module needs one build ID and TP_SLOT 2: ' + module)
            rows.append(('module', (str(index) + '/' if index else '') + module, bid[0].lower(), sha, fixup[1]))
            modules[module] = bid[0].lower()
        if 'main' not in modules: raise ValueError(f'Android program {index} needs a main module')
        required = profile.build_modules().get(modules['main'][:32], profile.words('modules'))
        missing = set(required) - modules.keys()
        if missing: raise ValueError('missing required Android modules: ' + ', '.join(sorted(missing)))
    return expected + rows_bytes(rows)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('profile'); p.add_argument('header', type=Path)
    a = p.parse_args()
    value = (START + host_contract(a.profile) + END).decode()
    text = '/* Generated Android producer contract. */\nstatic const char dm_android_host_contract[] __attribute__((used)) = ' + json.dumps(value) + ';\n'
    if not a.header.exists() or a.header.read_text() != text:
        a.header.parent.mkdir(parents=True, exist_ok=True); a.header.write_text(text)


if __name__ == '__main__': main()
