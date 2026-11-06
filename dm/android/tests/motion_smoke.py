#!/usr/bin/env python3
"""Change the emulator sensors and verify six-axis values read by the guest API."""
import argparse
import hashlib
import math
from pathlib import Path
import re
import shlex
import subprocess
import time
import zipfile

from app_smoke import EXTERNAL, PACKAGE


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--serial', required=True)
    p.add_argument('--apk', type=Path, required=True)
    p.add_argument('--adb', required=True)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    if not args.serial.startswith('emulator-'):
        p.error('requires a dedicated emulator')
    args.output.mkdir(parents=True, exist_ok=True)
    def adb(*cmd):
        return subprocess.check_output([args.adb, '-s', args.serial, *cmd], text=True, timeout=30)
    def log():
        return adb('shell', 'cat ' + EXTERNAL + '/log.txt 2>/dev/null || true')
    def wait(check, label):
        end = time.monotonic() + 15
        while time.monotonic() < end:
            if check():
                return
            time.sleep(.15)
        raise AssertionError(label + '\n' + log()[-1600:])
    def set_sensors(acc, gyro):
        for key, values in [('acceleration', acc), ('gyroscope', gyro)]:
            assert 'OK' in adb('emu', 'sensor', 'set', key, ':'.join(map(str, values)))
    def expect(acc, gyro):
        rotation = int(re.search(r'mCurrentOrientation=(\d+)', adb('shell', 'dumpsys display')).group(1))
        def screen(vec):
            x, y, z = vec
            return [(x,y,z), (-y,x,z), (-x,-y,z), (y,-x,z)][rotation]
        want = [v / -9.80665 for v in screen(acc)] + [v / math.tau for v in screen(gyro)]
        def seen():
            rows = re.findall(r'GUEST_MOTION ((?:[a-f0-9]{8} ){5}[a-f0-9]{8})', log())
            if not rows: return False
            words = [int(x, 16) for x in rows[-1].split()]
            got = [(x if x < 1 << 31 else x - (1 << 32)) / 1000 for x in words]
            return all(abs(a-b) < .035 for a,b in zip(got,want))
        wait(seen, f'guest acceleration in G and rotation in rev/s: {want}')
    original = {}
    for key in ['acceleration', 'gyroscope']:
        original[key] = re.search(r'= ([^\r\n]+)', adb('emu', 'sensor', 'get', key)).group(1)
    with zipfile.ZipFile(args.apk) as apk:
        stamp = hashlib.sha256(apk.read('assets/game/files.txt')).hexdigest()
    adb('install', '-r', str(args.apk))
    env = EXTERNAL + '/env.txt'
    changed = False
    old_env = None
    try:
        adb('shell', 'am', 'force-stop', PACKAGE)
        adb('shell', 'rm', '-f', EXTERNAL + '/log.txt')
        adb('shell', 'am', 'start', '-W', '-n', PACKAGE + '/com.nxnative.MenuActivity')
        wait(lambda: adb('shell','cat ' + EXTERNAL + '/.unpacked 2>/dev/null || true').strip() == stamp, 'assets')
        if adb('shell', 'test -f ' + env + ' && echo yes || true').strip():
            old_env = adb('exec-out', 'cat', env)
        changed = True
        adb('shell', "printf 'DM_DBG=1\\nDM_OPMODE=0\\nDM_PHONE_MOTION=1\\n' > " + env)
        adb('shell', 'input', 'keyevent', 'KEYCODE_BUTTON_START')
        wait(lambda: 'GUEST_MOTION' in log(), 'guest polling')
        acc, gyro = (3, -6, 8), (.5, -.75, 1)
        set_sensors(acc, gyro)
        expect(acc, gyro)
        adb('shell', 'input', 'keyevent', 'KEYCODE_HOME')
        wait(lambda: 'front end: paused' in log(), 'app suspended')
        acc, gyro = (-4, 7, -8), (-1, .25, -.5)
        set_sensors(acc, gyro)
        adb('shell', 'am', 'start', '-W', '-n', PACKAGE + '/com.nxnative.MenuActivity')
        wait(lambda: log().count('front end: resumed') >= 2, 'same guest resumed')
        expect(acc, gyro)
        assert log().count('APP_FIXTURE_MAIN') == 1
        (args.output / 'enabled.log').write_text(log())
        adb('shell', 'am', 'force-stop', PACKAGE)
        adb('shell', 'rm', '-f', EXTERNAL + '/log.txt')
        adb('shell', "printf 'DM_DBG=1\\nDM_OPMODE=0\\nDM_PHONE_MOTION=0\\n' > " + env)
        adb('shell', 'am', 'start', '-W', '-n', PACKAGE + '/com.nxnative.MenuActivity')
        adb('shell', 'input', 'keyevent', 'KEYCODE_BUTTON_START')
        wait(lambda: log().count('GUEST_MOTION') >= 8, 'disabled sensor guest polling')
        quiet = '00000000 00000000 fffffc18 00000000 00000000 00000000'
        assert set(re.findall(r'GUEST_MOTION ([0-9a-f ]+)', log())) == {quiet}
        print('PASS: guest six-axis vectors, units, rotation, resume and disabled phone')
    finally:
        (args.output / 'guest.log').write_text(log())
        adb('shell', 'am', 'force-stop', PACKAGE)
        for key, value in original.items():
            adb('emu', 'sensor', 'set', key, value)
        if changed and old_env is None:
            adb('shell', 'rm', '-f', env)
        elif changed:
            adb('shell', 'printf %s ' + shlex.quote(old_env) + ' > ' + env)


if __name__ == '__main__':
    main()
