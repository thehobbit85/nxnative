#!/usr/bin/env python3
"""Verify managed write-watch diagnostics in the actual release-linked APK."""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time
import zipfile

PACKAGE = 'com.nxnative.t0100633007d48000.test'
EXTERNAL = '/sdcard/Android/data/' + PACKAGE + '/files'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--serial', required=True)
    p.add_argument('--apk', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--region', choices=('heap', 'alias', 'stack'), default='heap')
    p.add_argument('--accounting', action='store_true', help='also verify ordinary-app memory and per-thread CPU reports')
    p.add_argument('--adb', default=str(Path(os.environ.get('ANDROID_SDK_ROOT', '')) / 'platform-tools/adb'))
    a = p.parse_args()
    if not a.serial.startswith('emulator-'): p.error('select an emulator')
    a.output.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(a.apk) as apk: identity = hashlib.sha256(apk.read('assets/game/files.txt')).hexdigest()
    base = [a.adb, '-s', a.serial]
    def adb(*args): return subprocess.check_output([*base, *args], text=True, stderr=subprocess.STDOUT, timeout=30)
    def shell(cmd): return adb('shell', cmd)
    def log(): return shell('cat ' + EXTERNAL + '/log.txt 2>/dev/null || true')
    def wait(test, name):
        end = time.monotonic() + 45
        while time.monotonic() < end:
            text = log(); (a.output / 'log.txt').write_text(text)
            assert 'APP_FIXTURE_FAILED' not in text and 'CRASH' not in text and 'FATAL' not in text, text[-6000:]
            if test(text): return text
            time.sleep(0.2)
        raise AssertionError('timed out: ' + name + '\n' + log()[-6000:])
    print(adb('install', '-r', str(a.apk.resolve())), flush=True)
    adb('shell', 'am', 'force-stop', PACKAGE)
    try:
        shell('rm -f ' + EXTERNAL + '/log.txt')
        adb('shell', 'am', 'start', '-W', '-n', PACKAGE + '/com.nxnative.MenuActivity')
        wait(lambda _: shell('cat ' + EXTERNAL + '/.unpacked 2>/dev/null || true').strip() == identity, 'installed watch assets')
        settings = 'DM_DBG=1\\nDM_PROBE_WATCH=' + a.region + '+80+8\\n'
        if a.accounting:
            shell('rm -f ' + EXTERNAL + '/memstat.tsv ' + EXTERNAL + '/memstat.tsv.regions ' + EXTERNAL + '/thrcpu.tsv')
            settings += 'DM_MEMSTAT=' + EXTERNAL + '/memstat.tsv\\nDM_MEMSTAT_DUMP=2\\nDM_THRCPU=' + EXTERNAL + '/thrcpu.tsv,1\\n'
        shell("printf '" + settings + "' > " + EXTERNAL + '/env.txt')
        adb('shell', 'input', 'keyevent', 'KEYCODE_BUTTON_START')
        text = wait(lambda s: 'WATCH_FIXTURE_DONE' in s and 'APP_FIXTURE_FRAME' in s and
                    len(re.findall(r'WATCH .*store of 8 bytes.*-> 0xabc[0-9a-f]+ pc=main\+', s)) == 32 and
                    'disabled for this allocation: unsupported or crossing store' in s, 'guest stores and presentation')
        generations = {1} if a.region == 'stack' else set(range(1, 35))
        assert set(map(int, re.findall(r'armed generation=(\d+)', text))) == generations
        assert text.count('disabled for this allocation: unsupported or crossing store') == 1
        assert ('-> 0xbeef pc=main+' in text) == (a.region != 'stack')
        assert 'tid=' in text and 'WATCH dropped' not in text
        if a.accounting:
            def rows(name):
                value = shell('cat ' + EXTERNAL + '/' + name + ' 2>/dev/null || true')
                (a.output / name).write_text(value)
                return list(csv.DictReader([line for line in value.splitlines() if not line.startswith('#')], delimiter='\t'))
            wait(lambda _: len(rows('memstat.tsv')) >= 3 and len(rows('memstat.tsv.regions')) > 0 and
                 sum(row.get('thread name') == 'total' for row in rows('thrcpu.tsv')) >= 2, 'memory/CPU accounting artifacts')
            memory = rows('memstat.tsv')
            assert any(int(row['guest.virtual_kib']) > 0 and int(row['code.virtual_kib']) > 0 and int(row['total.pss_kib']) > 0 for row in memory)
            for row in memory:
                for metric in ('virtual', 'rss', 'pss', 'private_dirty', 'swap'):
                    assert int(row['total.' + metric + '_kib']) == sum(int(row[c + '.' + metric + '_kib']) for c in
                                                                    ('guest', 'code', 'heap', 'gpu_mapping', 'stack', 'file', 'other', 'mixed'))
            cpu = rows('thrcpu.tsv')
            assert any(row['thread name'] == 'total' and float(row['CPU%']) > 0 for row in cpu)
            assert any(row['thread ID'].isdigit() and float(row['CPU%']) > 0 for row in cpu)
            regions = rows('memstat.tsv.regions')
            assert {'guest', 'code'} <= {row['class'] for row in regions}
            assert '[RED] DM_MEMSTAT' not in log() and '[RED] DM_THRCPU' not in log()
        (a.output / 'result.json').write_text(json.dumps({'passed': True, 'region': a.region, 'allocations': len(generations),
                                                       'unsupported_fallbacks': 1, 'accounting': a.accounting}, indent=2))
        print('PASS: release-linked Android APK, ' + a.region + ' watch lifetimes, guest stores/copies, unsupported-store fallback and continued presentation', flush=True)
    finally:
        (a.output / 'log.txt').write_text(log())
        adb('shell', 'am', 'force-stop', PACKAGE)


if __name__ == '__main__': main()
