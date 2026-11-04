"""Prepared program inputs for one Android application; never runtime paths."""
import json
from pathlib import Path
import re


def program_inputs(a):
    programs = {0: {'profile': getattr(a, 'profile', None) or a.id, 'build': a.build,
                    'patched': getattr(a, 'patched', None), 'romfs': getattr(a, 'romfs', None), 'aoc': getattr(a, 'aoc', None)}}
    source = getattr(a, 'programs', None)
    if not source: return programs
    source = Path(source).resolve()
    try: catalog = json.loads(source.read_text())
    except (OSError, ValueError) as error: raise ValueError('invalid Android programs catalog: ' + str(source)) from error
    if not isinstance(catalog, dict) or catalog.get('schema') != 1 or not isinstance(catalog.get('programs'), list):
        raise ValueError('Android programs catalog needs schema 1 and a programs list')
    for row in catalog['programs']:
        if not isinstance(row, dict): raise ValueError('invalid Android program row')
        index = row.get('index')
        if type(index) is not int or not 1 <= index <= 2147483647 or index in programs:
            raise ValueError('Android program index must be a unique positive int32')
        if not isinstance(row.get('profile'), str) or not re.fullmatch('[0-9A-F]{16}', row['profile']):
            raise ValueError('Android program profile must be a 16-digit uppercase title ID')
        if row.get('gfx') != 'nvn': raise ValueError('Android program currently requires the NVN guest backend')
        item = {'profile': row['profile']}
        for key in ('build', 'patched', 'romfs', 'aoc'):
            value = row.get(key)
            if value is None and key != 'build': item[key] = None; continue
            if not isinstance(value, str) or not value: raise ValueError('Android program needs a valid ' + key + ' path')
            path = Path(value).expanduser()
            item[key] = str((source.parent / path).resolve())
        programs[index] = item
    return programs


def program_root(index):
    return 'programs/' + str(index) + '/' if index else ''
