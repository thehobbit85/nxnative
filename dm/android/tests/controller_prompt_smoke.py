#!/usr/bin/env python3
"""Check the controller applet in an installed synthetic --controller-prompt APK."""
import argparse
from pathlib import Path
import shlex
import subprocess
import time

from app_smoke import EXTERNAL, PACKAGE


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--adb', required=True)
    p.add_argument('--serial', required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=True)
    def adb(*args):
        return subprocess.check_output([a.adb, '-s', a.serial, *args], text=True, timeout=25)
    assert adb('shell', 'getprop', 'ro.kernel.qemu').strip() == '1', 'requires an emulator'
    def log():
        return adb('shell', 'cat ' + EXTERNAL + '/log.txt 2>/dev/null || true')
    def wait(fn, label):
        end = time.monotonic() + 20
        while time.monotonic() < end:
            if fn(): return
            text = log()
            assert 'APP_FIXTURE_FAILED' not in text and 'CRASH' not in text, text[-2500:]
            time.sleep(.1)
        raise AssertionError('timed out: ' + label + '\n' + log()[-2500:])
    dump = '/data/local/tmp/nx-controller-prompt.xml'
    def dialog():
        adb('shell', 'uiautomator', 'dump', dump)
        return 'Connect controllers' in adb('shell', 'cat', dump)
    env = EXTERNAL + '/env.txt'
    existed = adb('shell', 'test -f ' + env + ' && echo yes || true').strip() == 'yes'
    previous = adb('exec-out', 'cat', env) if existed else None
    setting = adb('shell', 'settings', 'get', 'secure', 'immersive_mode_confirmations').strip()
    try:
        adb('shell', 'am', 'force-stop', PACKAGE)
        adb('shell', 'settings', 'put', 'secure', 'immersive_mode_confirmations', 'confirmed')
        adb('shell', "printf 'DM_DBG=1\\n' > " + env)
        adb('shell', 'rm', '-f', EXTERNAL + '/log.txt')
        adb('shell', 'am', 'start', '-W', '-n', PACKAGE + '/com.nxnative.MenuActivity')
        adb('shell', 'input', 'keyevent', 'KEYCODE_BUTTON_START')
        wait(lambda: 'APP_CTRL_WAIT1' in log(), 'first guest controller request')
        wait(dialog, 'first controller prompt')
        adb('shell', 'input', 'keyevent', 'KEYCODE_BACK')
        wait(lambda: 'APP_CTRL_WAIT2' in log(), 'cancellation returned to guest')
        wait(dialog, 'second controller prompt')
        adb('shell', 'input', 'keyevent', 'KEYCODE_HOME')
        wait(lambda: 'front end: paused' in log(), 'background')
        assert 'APP_CTRL_PASSED' not in log(), 'background canceled the pending prompt'
        adb('shell', 'am', 'start', '-W', '-n', PACKAGE + '/com.nxnative.MenuActivity')
        adb('shell', 'input', 'keyevent', 'KEYCODE_BUTTON_START')
        wait(dialog, 'controller prompt after resume')
        adb('shell', 'input', 'keyevent', 'KEYCODE_BACK')
        wait(lambda: 'APP_CTRL_PASSED' in log(), 'guest cancellation result after resume')
        print('PASS: controller applet cancellation, background retention, resume and guest results')
    finally:
        (a.output / 'guest.log').write_text(log())
        adb('shell', 'am', 'force-stop', PACKAGE)
        adb('shell', 'rm', '-f', dump)
        if previous is None: adb('shell', 'rm', '-f', env)
        else: adb('shell', 'printf %s ' + shlex.quote(previous) + ' > ' + env)
        if setting == 'null': adb('shell', 'settings', 'delete', 'secure', 'immersive_mode_confirmations')
        else: adb('shell', 'settings put secure immersive_mode_confirmations ' + shlex.quote(setting))


if __name__ == '__main__':
    main()
