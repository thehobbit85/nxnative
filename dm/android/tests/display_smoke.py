#!/usr/bin/env python3
"""Compare the guest display query with an explicitly selected emulator's physical mode."""
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
    if not a.serial.startswith('emulator-'): p.error('select an emulator')
    a.output.mkdir(parents=True, exist_ok=True)
    base = [a.adb, '-s', a.serial]
    def adb(*args): return subprocess.check_output([*base, *args], text=True, stderr=subprocess.STDOUT, timeout=30)
    def shell(command): return adb('shell', command)
    def log(): return shell('cat ' + EXTERNAL + '/log.txt 2>/dev/null || true')
    def wait(test, name):
        end = time.monotonic() + 30
        while time.monotonic() < end:
            if test(): return
            time.sleep(0.2)
        raise AssertionError(name + '\n' + log()[-4000:])
    size = re.search(r'Physical size: (\d+)x(\d+)', shell('wm size'))
    assert size, 'physical display size unavailable'
    width, height = sorted(map(int, size.groups()), reverse=True)
    resolution = max(i for i, (w, h) in enumerate([(0, 0), (1920, 1080), (2560, 1440), (3840, 2160)])
                     if width >= w and height >= h)
    with zipfile.ZipFile(a.apk) as apk: identity = hashlib.sha256(apk.read('assets/game/files.txt')).hexdigest()
    print(adb('install', '-r', str(a.apk.resolve())), flush=True)
    try:
        for mode, expected in [(0, 1), (1, resolution)]:
            adb('shell', 'am', 'force-stop', PACKAGE)
            adb('shell', 'am', 'start', '-W', '-n', PACKAGE + '/com.nxnative.MenuActivity')
            wait(lambda: shell('cat ' + EXTERNAL + '/.unpacked 2>/dev/null || true').strip() == identity, 'installed assets')
            shell("printf 'DM_DBG=1\\nDM_OPMODE=" + str(mode) + "\\n' > " + EXTERNAL + '/env.txt')
            shell('rm -f ' + EXTERNAL + '/log.txt')
            adb('shell', 'input', 'keyevent', 'KEYCODE_BUTTON_START')
            wait(lambda: 'DISPLAY_MODE_' in log() or 'APP_FIXTURE_FAILED' in log(), 'guest display result')
            text = log(); (a.output / f'mode-{mode}.log').write_text(text)
            assert 'APP_FIXTURE_FAILED' not in text and 'CRASH' not in text, text[-4000:]
            assert f'DISPLAY_MODE_{expected}' in text, f'operation mode {mode}, physical {width}x{height}: expected {expected}\n' + text[-3000:]
        (a.output / 'result.json').write_text(json.dumps({'physical': [width, height], 'docked': resolution, 'handheld': 1, 'passed': True}, indent=2))
        print('PASS: handheld and docked display queries match their physical-mode contract', flush=True)
    finally:
        adb('shell', 'am', 'force-stop', PACKAGE)


if __name__ == '__main__': main()
