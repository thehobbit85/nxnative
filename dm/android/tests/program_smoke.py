#!/usr/bin/env python3
"""Exercise app_fixture.py --program-index 0/1/2 in a bundled test APK on an emulator."""
import argparse
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
    p.add_argument('--adb', default=str(Path(os.environ.get('ANDROID_SDK_ROOT', '')) / 'platform-tools/adb'))
    a = p.parse_args()
    if not a.serial.startswith('emulator-'): p.error('use an explicitly selected emulator')
    a.output.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(a.apk) as apk: identity = hashlib.sha256(apk.read('assets/game/files.txt')).hexdigest()
    base = [a.adb, '-s', a.serial]

    def adb(*args): return subprocess.check_output([*base, *args], text=True, stderr=subprocess.STDOUT, timeout=30)
    def shell(cmd): return adb('shell', cmd)
    def log(): return shell('cat ' + EXTERNAL + '/log.txt 2>/dev/null || true')
    def wait(test, name, seconds=60):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            if test(): return
            time.sleep(0.2)
        raise AssertionError(name + '\n' + log()[-6000:])

    print(adb('install', '-r', str(a.apk.resolve())), flush=True)
    adb('shell', 'am', 'force-stop', PACKAGE)
    try:
        adb('shell', 'am', 'start', '-W', '-n', PACKAGE + '/com.nxnative.MenuActivity')
        wait(lambda: shell('cat ' + EXTERNAL + '/.unpacked 2>/dev/null || true').strip() == identity, 'assets installed')
        shell("printf 'DM_DBG=1\\n' > " + EXTERNAL + '/env.txt')
        shell('rm -f ' + EXTERNAL + '/log.txt')
        adb('shell', 'input', 'keyevent', 'KEYCODE_BUTTON_START')
        seen = {}

        def finished():
            text = log()
            assert 'APP_FIXTURE_FAILED' not in text and 'PROGRAM_EXECUTE_RETURNED' not in text and 'CRASH' not in text, text[-6000:]
            m = re.search(r'boot: program (\d+), previous (-?\d+), (\d+) launch parameter bytes, pid (\d+)', text)
            if m:
                program, previous, size, pid = map(int, m.groups())
                if pid not in seen:
                    seen[pid] = {'program': program, 'previous': previous, 'size': size}
                    print(dict(pid=pid, **seen[pid]), flush=True)
                (a.output / (str(pid) + '.log')).write_text(text)
            return 'PROGRAM_HANDOFF_DONE' in text and 'APP_FIXTURE_FRAME' in text

        wait(finished, 'four complete process handoffs')
        assert [r['program'] for r in seen.values()] == [0, 1, 2, 0, 0], seen
        assert [r['previous'] for r in seen.values()] == [-1, 0, 1, 2, 0], seen
        assert [r['size'] for r in seen.values()] == [0, 6, 6, 0, 0], seen
        for pid, row in seen.items():
            text = (a.output / (str(pid) + '.log')).read_text()
            assert 'PROGRAM_' + str(row['program']) + '_READY' in text and 'APP_FIXTURE_SAVE_COMMITTED' in text, text
            if row['previous'] != -1: assert 'APP_FIXTURE_SAVE_RETAINED' in text, text
        # A subsequent HOME/menu launch discards the consumed transition context.
        adb('shell', 'am', 'force-stop', PACKAGE)
        shell('rm -f ' + EXTERNAL + '/log.txt')
        adb('shell', 'am', 'start', '-W', '-n', PACKAGE + '/com.nxnative.MenuActivity')
        adb('shell', 'input', 'keyevent', 'KEYCODE_BUTTON_START')
        wait(lambda: 'PROGRAM_0_READY' in log(), 'ordinary launcher restart')
        text = log(); assert 'boot: program 0, previous -1, 0 launch parameter bytes' in text
        assert 'APP_FIXTURE_SAVE_RETAINED' in text
        (a.output / 'ordinary-restart.log').write_text(text)
        (a.output / 'result.json').write_text(json.dumps({'handoffs': seen, 'ordinary_restart': True}, indent=2))
        print('PASS: isolated program processes, parameters, resources, shared save, fallback and normal restart', flush=True)
    finally:
        adb('shell', 'am', 'force-stop', PACKAGE)


if __name__ == '__main__': main()
