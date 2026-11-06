#!/usr/bin/env python3
"""Sample two independent Android input devices through a release-linked guest (--controllers)."""
import argparse
import hashlib
from contextlib import contextmanager
from pathlib import Path
import re
import shlex
import subprocess
import time
import zipfile

from app_smoke import EXTERNAL, PACKAGE


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--adb', required=True)
    parser.add_argument('--serial', required=True)
    parser.add_argument('--apk', type=Path, required=True)
    parser.add_argument('--injector', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if not args.serial.startswith('emulator-'):
        parser.error('requires a dedicated emulator')
    args.output.mkdir(parents=True, exist_ok=True)
    base = [args.adb, '-s', args.serial]
    def adb(*command):
        return subprocess.check_output([*base, *command], text=True, timeout=30)
    remote = '/data/local/tmp/nxnative-controller-test'
    def log():
        return adb('shell', 'cat ' + EXTERNAL + '/log.txt 2>/dev/null || true')
    def wait(check, label):
        until = time.monotonic() + 15
        while time.monotonic() < until:
            value = check()
            if value:
                return value
            time.sleep(.1)
        focus = adb('shell', 'dumpsys window | grep mCurrentFocus')
        raise AssertionError(label + '\n' + focus + '\n' + log()[-2600:])
    def expect(first, second):
        def matches():
            samples = re.findall(r'GUEST_PAD_([01])=([0-9a-f]{8})', log())
            latest = {int(slot): int(word, 16) for slot, word in samples}
            return latest == {0: first, 1: second}
        wait(matches, f'guest players expected {first:08x} and {second:08x}')
    @contextmanager
    def pad(number):
        name = f'nxnative test controller {number}'
        proc = subprocess.Popen([*base, 'shell', '-T', remote, str(number)], stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, text=True)
        try:
            assert proc.stdout.readline().strip() == 'connected', f'could not register controller {number}'
            wait(lambda: re.search(r'Device \d+: ' + name + r'\n', adb('shell', 'dumpsys input')), 'input reader attached ' + name)
            def send(kind, code, value):
                proc.stdin.write(f'{kind} {code} {value}\n')
                proc.stdin.flush()
            yield send
        finally:
            proc.stdin.close()
            try:
                assert proc.wait(timeout=5) == 0
            finally:
                proc.kill()
                proc.stdout.close()
    adb('push', str(args.injector), remote)
    adb('install', '-r', str(args.apk))
    with zipfile.ZipFile(args.apk) as apk:
        stamp = hashlib.sha256(apk.read('assets/game/files.txt')).hexdigest()
    setting = 'immersive_mode_confirmations'
    previous = adb('shell', 'settings', 'get', 'secure', setting).strip()
    changed_env = False
    previous_env = None
    env_path = EXTERNAL + '/env.txt'
    try:
        # SystemUI's first-use confirmation can take focus when a gamepad appears.
        adb('shell', 'settings', 'put', 'secure', setting, 'confirmed')
        adb('shell', 'am', 'force-stop', PACKAGE)
        adb('shell', 'rm', '-f', EXTERNAL + '/log.txt')
        adb('shell', 'am', 'start', '-W', '-n', PACKAGE + '/com.nxnative.MenuActivity')
        wait(lambda: adb('shell', 'cat ' + EXTERNAL + '/.unpacked 2>/dev/null || true').strip() == stamp, 'installed assets')
        wait(lambda: 'MenuActivity' in adb('shell', 'dumpsys window | grep mCurrentFocus'), 'menu focus')
        if adb('shell', 'test -f ' + env_path + ' && echo yes || true').strip():
            previous_env = adb('exec-out', 'cat', env_path)
        changed_env = True
        adb('shell', "printf 'DM_DBG=1\\nDM_OPMODE=0\\n' > " + EXTERNAL + '/env.txt')
        adb('shell', 'input', 'keyevent', 'KEYCODE_BUTTON_START')
        C = 1 << 16
        expect(C, 0)
        with pad(1) as first, pad(2) as second:
            expect(C, C)
            first(1, 304, 1)   # south = Switch B
            second(1, 305, 1)  # east = Switch A
            expect(C | 2, C | 1)
            first(3, 0, 1)     # left stick right
            second(3, 1, -1)   # left stick up
            expect(C | 2 | 1 << 17, C | 1 | 1 << 19)
            first(1, 304, 0)
            first(3, 0, 0)
            expect(C, C | 1 | 1 << 19)
        expect(C, 0)
        # Reconnect in reverse order. The first controller's slot is free too.
        with pad(2) as second:
            expect(C, C)
            second(3, 16, -1)  # hat left
            expect(C, C | 1 << 12)
            with pad(1) as first:
                first(1, 304, 1)
                expect(C | 2, C | 1 << 12)
                adb('shell', 'cmd', 'statusbar', 'expand-notifications')
                expect(C, C)
                adb('shell', 'cmd', 'statusbar', 'collapse')
                wait(lambda: 'GameActivity' in adb('shell', 'dumpsys window | grep mCurrentFocus'), 'game focus')
                first(1, 304, 0)
                second(3, 16, 0)
                second(1, 305, 1)
                expect(C, C | 1)
        expect(C, 0)
        print('PASS: two guest npads, independent buttons and sticks, hat, unplug while held, reverse reconnect and focus release')
    finally:
        (args.output / 'guest.log').write_text(log())
        adb('shell', 'cmd', 'statusbar', 'collapse')
        adb('shell', 'am', 'force-stop', PACKAGE)
        if changed_env and previous_env is None:
            adb('shell', 'rm', '-f', env_path)
        elif changed_env:
            adb('shell', "printf %s " + shlex.quote(previous_env) + ' > ' + env_path)
        if previous == 'null':
            adb('shell', 'settings', 'delete', 'secure', setting)
        else:
            adb('shell', 'settings put secure ' + setting + ' ' + shlex.quote(previous))
        adb('shell', 'rm', '-f', remote)


if __name__ == '__main__':
    main()
