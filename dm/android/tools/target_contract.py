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


def host_contract(profile=None):
    g = game.load(profile)
    match = re.search(r'#define DMSH_CHAIN "([a-f0-9]+)"', (ROOT / 'dm/gfx/nvn/dmsh_chain.h').read_text())
    if not match: raise ValueError('missing shared shader chain version')
    chain = match.group(1)
    # The generated header's first comment names an absolute path for derived
    # profiles. Hash the actual definitions so relocating identical inputs does
    # not change the portable contract.
    header = '\n'.join(line for line in game.header(g).splitlines() if not line.startswith('//'))
    rows = [('schema', 1), ('platform', 'android'), ('arch', 'arm64-v8a'), ('host_abi', 1), ('tp_slot', 2),
            ('shader_schema', 1), ('shader_chain', chain), ('exporter', digest(ROOT / 'dm/android/tools/spv_android.py')),
            ('profile', g.id), ('profile_header', hashlib.sha256(header.encode()).hexdigest()), ('feature', 'guest-nvn')]
    profiles = [g] + [game.load(gid) for _, gid in g.pairs('programs')] if g.get('programs', '') else [g]
    for sub in profiles:
        for path in sorted(sub.dir.iterdir()):
            if path.is_file() and (path.suffix == '.tsv' or path.name == 'patches.py') and path.name not in ('c1_full.tsv', 'cb_extent.tsv'):
                rows.append(('profile_input', sub.id + '/' + path.name, digest(path)))
    return rows_bytes(rows)


def host_record(data):
    if data.count(START) != 1 or data.count(END) != 1:
        raise ValueError('missing or ambiguous Android host contract; rebuild the native host')
    start = data.index(START) + len(START); end = data.index(END)
    if end <= start: raise ValueError('invalid Android host contract; rebuild the native host')
    return data[start:end]


def stamp_export(directory, profile=None, producer=None):
    directory = Path(directory)
    current = host_contract(profile)
    if producer is not None and producer != current:
        raise ValueError('Android shader inputs changed while exporting; retry with stable inputs')
    files = {p.name: digest(p) for p in sorted(directory.iterdir()) if p.suffix == '.spv' or p.name.endswith('.vk.txt')}
    record = {'schema': 1, 'producer': hashlib.sha256(current).hexdigest(), 'files': files}
    (directory / 'export-contract.json').write_text(json.dumps(record, sort_keys=True, indent=2) + '\n')


def target_manifest(a, lib, files):
    expected = host_contract(a.profile or a.id)
    if host_record(Path(lib).read_bytes()) != expected:
        raise ValueError('Android host contract differs from selected profile/tools; rebuild the native host')
    rows = [('title', a.id.upper()), ('host_sha256', digest(lib))]
    assets = {name: (Path(source), digest(source)) for name, source in files}
    shaders = {name[3:]: sha for name, (_, sha) in assets.items() if name.startswith('vk/')}
    if shaders:
        path = Path(a.build) / 'shader/vk/export-contract.json'
        try:
            saved = json.loads(path.read_text())
            if saved != {'schema': 1, 'producer': hashlib.sha256(expected).hexdigest(), 'files': shaders}:
                raise ValueError('stale producer or products')
        except (OSError, ValueError) as error:
            raise ValueError('Android shader provenance missing or incompatible; export shaders again') from error
        rows.append(('shader_export', digest(path)))
    else: rows.append(('shader_export', 'empty'))
    for name, (path, sha) in sorted(assets.items()):
        rows.append(('asset', name, path.stat().st_size, sha))
        if name.startswith('dm/') and name.endswith('.patched.bin'):
            module = name[3:-12]; fixup = assets.get('dm/' + module + '.fixups.tsv')
            text = fixup[0].read_text() if fixup else ''
            bid = re.findall(r'^# BUILD_ID\t([0-9a-fA-F]{16,64})$', text, re.M)
            tp = re.findall(r'^# TP_SLOT\t(\d+)$', text, re.M)
            if fixup is None or len(bid) != 1 or tp != ['2']:
                raise ValueError('Android module needs one build ID and TP_SLOT 2: ' + module)
            rows.append(('module', module, bid[0].lower(), sha, fixup[1]))
    if not any(row[0] == 'module' and row[1] == 'main' for row in rows):
        raise ValueError('Android target manifest needs a main module')
    modules = {row[1]: row[2] for row in rows if row[0] == 'module'}
    g = game.load(a.profile or a.id)
    required = g.build_modules().get(modules['main'][:32], g.words('modules'))
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
