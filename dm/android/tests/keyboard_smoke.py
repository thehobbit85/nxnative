#!/usr/bin/env python3
"""Operate the Android software keyboard in a synthetic release APK built with --keyboard."""
import argparse
import hashlib
from pathlib import Path
import re
import shlex
import subprocess
import time
from xml.etree import ElementTree
import zipfile

from app_smoke import EXTERNAL, PACKAGE


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--adb', required=True)
    parser.add_argument('--serial', required=True)
    parser.add_argument('--apk', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if not args.serial.startswith('emulator-'):
        parser.error('requires a dedicated emulator')
    args.output.mkdir(parents=True, exist_ok=True)
    def adb(*command):
        return subprocess.check_output([args.adb, '-s', args.serial, *command], text=True, timeout=25)
    def log():
        return adb('shell', 'cat ' + EXTERNAL + '/log.txt 2>/dev/null || true')
    def wait(fn, label):
        end = time.monotonic() + 20
        while time.monotonic() < end:
            value = fn()
            if value:
                return value
            text = log()
            if 'APP_FIXTURE_FAILED' in text or 'CRASH' in text:
                raise AssertionError(label + '\n' + text[-2200:])
            time.sleep(.1)
        raise AssertionError('timed out: ' + label)
    dump = '/data/local/tmp/nxnative-keyboard-ui.xml'
    def page(title):
        def read():
            adb('shell', 'uiautomator', 'dump', dump)
            xml = adb('shell', 'cat ' + dump + ' 2>/dev/null || true')
            if not xml.startswith('<?xml'):
                return None
            nodes = list(ElementTree.fromstring(xml).iter('node'))
            return nodes if any(n.get('text') == title for n in nodes) else None
        return wait(read, 'keyboard dialog ' + title)
    def button(nodes, name):
        return next(n for n in nodes if n.get('resource-id') == 'android:id/' + name)
    def tap(node):
        x0, y0, x1, y1 = map(int, re.findall(r'\d+', node.get('bounds')))
        adb('shell', 'input', 'tap', str((x0+x1)//2), str((y0+y1)//2))
    with zipfile.ZipFile(args.apk) as apk:
        stamp = hashlib.sha256(apk.read('assets/game/files.txt')).hexdigest()
    adb('install', '-r', str(args.apk))
    setting = 'immersive_mode_confirmations'
    previous = adb('shell', 'settings', 'get', 'secure', setting).strip()
    changed_env = False
    env_path = EXTERNAL + '/env.txt'
    previous_env = None
    try:
        adb('shell', 'settings', 'put', 'secure', setting, 'confirmed')
        adb('shell', 'am', 'force-stop', PACKAGE)
        adb('shell', 'rm', '-f', EXTERNAL + '/log.txt')
        adb('shell', 'am', 'start', '-W', '-n', PACKAGE + '/com.nxnative.MenuActivity')
        wait(lambda: adb('shell', 'cat ' + EXTERNAL + '/.unpacked 2>/dev/null || true').strip() == stamp, 'installed assets')
        if adb('shell', 'test -f ' + env_path + ' && echo yes || true').strip():
            previous_env = adb('exec-out', 'cat', env_path)
        changed_env = True
        adb('shell', "printf 'DM_DBG=1\\n' > " + EXTERNAL + '/env.txt')
        adb('shell', 'input', 'keyevent', 'KEYCODE_BUTTON_START')
        wait(lambda: 'APP_FIXTURE_MAIN' in log(), 'guest boot')
        for title in ('Unicode', 'UTF16'):
            nodes = page(title)
            field = next(n for n in nodes if n.get('class') == 'android.widget.EditText')
            assert field.get('text') == 'A\U0001f680e\u0301Z'
            tap(button(nodes, 'button1'))
        nodes = page('Minimum')
        assert button(nodes, 'button1').get('enabled') == 'false'
        adb('shell', 'input', 'text', 'ab')
        nodes = page('Minimum')
        assert button(nodes, 'button1').get('enabled') == 'true'
        tap(button(nodes, 'button1'))
        tap(button(page('Cancel'), 'button2'))
        page('Background')
        adb('shell', 'input', 'keyevent', 'KEYCODE_HOME')
        wait(lambda: 'front end: paused' in log(), 'keyboard background')
        adb('shell', 'am', 'start', '-W', '-n', PACKAGE + '/com.nxnative.MenuActivity')
        wait(lambda: 'APP_KEYBOARD_PASSED' in log(), 'guest validated keyboard results')
        print('PASS: UTF-8 and UTF-16, supplementary code point, combining mark, minimum and maximum, cancel and background dismissal')
    finally:
        (args.output / 'guest.log').write_text(log())
        adb('shell', 'rm', '-f', dump)
        adb('shell', 'am', 'force-stop', PACKAGE)
        if changed_env and previous_env is None:
            adb('shell', 'rm', '-f', env_path)
        elif changed_env:
            adb('shell', "printf %s " + shlex.quote(previous_env) + ' > ' + env_path)
        if previous == 'null':
            adb('shell', 'settings', 'delete', 'secure', setting)
        else:
            adb('shell', 'settings put secure ' + setting + ' ' + shlex.quote(previous))


if __name__ == '__main__':
    main()
