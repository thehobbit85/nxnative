#!/usr/bin/env python3
"""Drive guest rumble and check the dedicated emulator's vibrator service."""
import argparse
from contextlib import contextmanager
import hashlib
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
    p.add_argument('--injector', type=Path, help='uinput_pad executable for the controller actuator checks')
    args = p.parse_args()
    if not args.serial.startswith('emulator-'):
        p.error('requires a dedicated emulator')
    args.output.mkdir(parents=True, exist_ok=True)
    def adb(*args_):
        return subprocess.check_output([args.adb, '-s', args.serial, *args_], text=True, timeout=30)
    def log():
        return adb('shell', 'cat ' + EXTERNAL + '/log.txt 2>/dev/null || true')
    def wait(check, msg):
        end = time.monotonic() + 15
        while time.monotonic() < end:
            if check():
                return
            time.sleep(.15)
        raise AssertionError(msg + '\n' + log()[-1700:])
    def button(key, marker):
        before = log().count(marker)
        adb('shell', 'input', 'keyevent', 'KEYCODE_BUTTON_' + key)
        wait(lambda: log().count(marker) > before, marker)
    def service():
        return adb('shell', 'dumpsys', 'vibrator_manager')
    def active():
        return 'isVibrating = true' in service()
    def stop():
        wait(lambda: not active(), 'vibrator stopped')
    def start(label, sharp=False):
        def expected():
            dump = service().partition('CurrentVibration:')[2].partition('NextVibration:')[0]
            steps = re.findall(r'Step\{amplitude=([\d.]+), frequencyHz=[\d.]+, duration=(\d+)\}', dump)
            return (PACKAGE in dump and len(steps) == 2 and abs(float(steps[0][0]) - .79) < .01
                    and sum(int(s[1]) for s in steps) == (11 if sharp else 30))
        wait(expected, 'vibrator amplitude and sharpness for ' + label)
        dump = service()
        (args.output / (label + '.log')).write_text(dump)
    remote = '/data/local/tmp/nxnative-haptics-test'
    @contextmanager
    def pad(number, label):
        path = args.output / (label + '.log')
        with path.open('w') as out:
            proc = subprocess.Popen([args.adb, '-s', args.serial, 'shell', '-T', remote, str(number), '--rumble'],
                                    stdin=subprocess.PIPE, stdout=out)
            try:
                wait(lambda: 'connected' in path.read_text(), 'kernel gamepad ' + label)
                wait(lambda: re.search(r'Device \d+: nxnative test controller ' + str(number) + r'\n',
                                       adb('shell', 'dumpsys input')), 'Android gamepad ' + label)
                yield path
            finally:
                proc.stdin.close()
                try:
                    assert proc.wait(timeout=5) == 0
                finally:
                    proc.kill()

    with zipfile.ZipFile(args.apk) as apk:
        stamp = hashlib.sha256(apk.read('assets/game/files.txt')).hexdigest()
    adb('install', '-r', str(args.apk))
    env = EXTERNAL + '/env.txt'
    changed = False
    previous = None
    setting = 'immersive_mode_confirmations'
    immersive = adb('shell', 'settings', 'get', 'secure', setting).strip()
    try:
        adb('shell', 'settings', 'put', 'secure', setting, 'confirmed')
        adb('shell', 'am', 'force-stop', PACKAGE)
        adb('shell', 'rm', '-f', EXTERNAL + '/log.txt')
        adb('shell', 'am', 'start', '-W', '-n', PACKAGE + '/com.nxnative.MenuActivity')
        wait(lambda: adb('shell','cat ' + EXTERNAL + '/.unpacked 2>/dev/null || true').strip() == stamp, 'assets')
        if adb('shell', 'test -f ' + env + ' && echo yes || true').strip():
            previous = adb('exec-out', 'cat', env)
        changed = True
        adb('shell', "printf 'DM_DBG=1\\nDM_OPMODE=0\\nDM_RUMBLE_LOG=1\\n' > " + env)
        adb('shell', 'input', 'keyevent', 'KEYCODE_BUTTON_START')
        wait(lambda: 'APP_FIXTURE_FRAME' in log(), 'guest polling')
        button('A', 'GUEST_HAPTIC_SOFT')
        start('soft')
        button('Y', 'GUEST_HAPTIC_SHARP')
        start('sharp', True)
        button('B', 'GUEST_HAPTIC_STOP')
        stop()
        button('A', 'GUEST_HAPTIC_SOFT')
        start('before-background')
        adb('shell', 'input', 'keyevent', 'KEYCODE_HOME')
        wait(lambda: 'front end: paused' in log(), 'app paused')
        stop()
        adb('shell', 'am', 'start', '-W', '-n', PACKAGE + '/com.nxnative.MenuActivity')
        wait(lambda: log().count('front end: resumed') >= 2, 'return to guest')
        stop()
        button('Y', 'GUEST_HAPTIC_SHARP')
        start('after-resume', True)
        assert log().count('APP_FIXTURE_MAIN') == 1
        print('PASS: two guest motors mixed on the phone, guest stop, HOME cancellation and fresh send on resume')
        if args.injector:
            adb('push', str(args.injector), remote)
            with pad(1, 'first') as first, pad(2, 'second') as second:
                stop()
                button('A', 'GUEST_HAPTIC_SOFT')
                wait(lambda: 'play 0 1' in first.read_text(), 'kernel player one vibrating')
                assert 'play 0 1' not in second.read_text()
                button('X', 'GUEST_HAPTIC_SECOND')
                wait(lambda: 'play 0 1' in second.read_text(), 'kernel player two vibrating')
                def distinct_periods():
                    pairs = [(int(l), int(r)) for l,r in re.findall(r'upload \d+ (\d+) (\d+)', second.read_text())]
                    return any(l >= 15000 and 20000 < r < 30000 for l,r in pairs) and any(0 < l < 10000 and r >= 45000 for l,r in pairs)
                wait(distinct_periods, 'player two has independent band frequencies on both motors')
                offset = first.stat().st_size
                button('L1', 'GUEST_HAPTIC_RIGHT_ONLY')
                wait(lambda: re.search(r'upload \d+ 0 [1-9]\d+', first.read_text()[offset:]),
                     'left motor stopped and unchanged right motor still running')
                button('B', 'GUEST_HAPTIC_STOP')
                wait(lambda: 'play 0 0' in first.read_text(), 'kernel player one stopped')
            with pad(2, 'reconnected') as connected:
                assert 'play 0 1' not in connected.read_text()
                button('X', 'GUEST_HAPTIC_SECOND')
                wait(lambda: 'play 0 1' in connected.read_text(), 'reconnected player two vibrating')
                button('A', 'GUEST_HAPTIC_SOFT')
                start('phone-restored')
            print('PASS: separate force-feedback devices, stop, reconnect and handset route restoration')
        (args.output / 'enabled.log').write_text(log())
        adb('shell', 'am', 'force-stop', PACKAGE)
        adb('shell', 'rm', '-f', EXTERNAL + '/log.txt')
        adb('shell', "printf 'DM_DBG=1\\nDM_OPMODE=0\\nDM_RUMBLE=0\\n' > " + env)
        adb('shell', 'am', 'start', '-W', '-n', PACKAGE + '/com.nxnative.MenuActivity')
        adb('shell', 'input', 'keyevent', 'KEYCODE_BUTTON_START')
        wait(lambda: 'APP_FIXTURE_FRAME' in log(), 'disabled guest polling')
        button('A', 'GUEST_HAPTIC_UNAVAILABLE')
        stop()
        print('PASS: disabled rumble unavailable to the guest and handset idle')
    finally:
        (args.output / 'guest.log').write_text(log())
        adb('shell', 'am', 'force-stop', PACKAGE)
        if changed and previous is None:
            adb('shell', 'rm', '-f', env)
        elif changed:
            adb('shell', 'printf %s ' + shlex.quote(previous) + ' > ' + env)
        if immersive == 'null':
            adb('shell', 'settings', 'delete', 'secure', setting)
        else:
            adb('shell', 'settings put secure ' + setting + ' ' + shlex.quote(immersive))
        if args.injector:
            adb('shell', 'rm', '-f', remote)


if __name__ == '__main__':
    main()
