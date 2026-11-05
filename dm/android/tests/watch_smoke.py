#!/usr/bin/env python3
"""Verify managed write-watch diagnostics in the actual release-linked APK."""
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
    p.add_argument('--region', choices=('heap', 'alias', 'stack'), default='heap')
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
        shell("printf 'DM_DBG=1\\nDM_PROBE_WATCH=" + a.region + "+80+8\\n' > " + EXTERNAL + '/env.txt')
        adb('shell', 'input', 'keyevent', 'KEYCODE_BUTTON_START')
        text = wait(lambda s: 'WATCH_FIXTURE_DONE' in s and 'APP_FIXTURE_FRAME' in s and
                    len(re.findall(r'WATCH .*store of 8 bytes.*-> 0xabc[0-9a-f]+ pc=main\+', s)) == 32 and
                    'disabled for this allocation: unsupported or crossing store' in s, 'guest stores and presentation')
        generations = {1} if a.region == 'stack' else set(range(1, 35))
        assert set(map(int, re.findall(r'armed generation=(\d+)', text))) == generations
        assert text.count('disabled for this allocation: unsupported or crossing store') == 1
        assert ('-> 0xbeef pc=main+' in text) == (a.region != 'stack')
        assert 'tid=' in text and 'WATCH dropped' not in text
        (a.output / 'result.json').write_text(json.dumps({'passed': True, 'region': a.region, 'allocations': len(generations), 'unsupported_fallbacks': 1}, indent=2))
        print('PASS: release-linked Android APK, ' + a.region + ' watch lifetimes, guest stores/copies, unsupported-store fallback and continued presentation', flush=True)
    finally:
        (a.output / 'log.txt').write_text(log())
        adb('shell', 'am', 'force-stop', PACKAGE)


if __name__ == '__main__': main()
