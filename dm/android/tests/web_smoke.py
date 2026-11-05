#!/usr/bin/env python3
"""Drive the real offline HTML guest fixture in an explicitly selected emulator."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import struct
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
    with zipfile.ZipFile(a.apk) as apk: identity = hashlib.sha256(apk.read('assets/game/files.txt')).hexdigest()
    base = [a.adb, '-s', a.serial]
    def adb(*args): return subprocess.check_output([*base, *args], text=True, stderr=subprocess.STDOUT, timeout=30)
    def shell(cmd): return adb('shell', cmd)
    def log(): return shell('cat ' + EXTERNAL + '/log.txt 2>/dev/null || true')
    def wait(test, name):
        end = time.monotonic() + 45
        while time.monotonic() < end:
            text = log()
            (a.output / 'log.txt').write_text(text)
            assert 'APP_FIXTURE_FAILED' not in text and 'CRASH' not in text, text[-6000:]
            if test(text): return
            time.sleep(0.2)
        raise AssertionError('timed out: ' + name + '\n' + log()[-6000:])
    print(adb('install', '-r', str(a.apk.resolve())), flush=True)
    adb('shell', 'am', 'force-stop', PACKAGE)
    try:
        shell('rm -f ' + EXTERNAL + '/log.txt')
        adb('shell', 'am', 'start', '-W', '-n', PACKAGE + '/com.nxnative.MenuActivity')
        wait(lambda _: shell('cat ' + EXTERNAL + '/.unpacked 2>/dev/null || true').strip() == identity, 'installed HTML assets')
        ctl = EXTERNAL + '/web-ctl.txt'
        shot = EXTERNAL + '/webshot.png'
        shell('rm -f ' + ctl + ' ' + shot)
        shell("printf 'DM_DBG=1\\nDM_WEB_LOG=1\\nDM_CTL=" + ctl + "\\n' > " + EXTERNAL + '/env.txt')
        adb('shell', 'input', 'keyevent', 'KEYCODE_BUTTON_START')
        wait(lambda text: 'WEB_WAIT_A' in text, 'hidden page ready and bidirectional messages')
        adb('shell', 'input', 'keyevent', 'KEYCODE_HOME')
        wait(lambda text: 'front end: paused' in text, 'page backgrounded')
        adb('shell', 'am', 'start', '-W', '-n', PACKAGE + '/com.nxnative.MenuActivity')
        wait(lambda text: text.count('front end: resumed') >= 2, 'page resumed')
        assert log().count('APP_FIXTURE_MAIN') == 1, 'browser resume restarted the guest'
        def page_visible(_):
            # The native fixture is solid green with black bars. Require the
            # page's white text/button pixels, excluding the navigation area;
            # successful JavaScript alone cannot prove the WebView was drawn.
            raw = subprocess.check_output([*base, 'exec-out', 'screencap'], timeout=30)
            width, height, fmt = struct.unpack_from('<III', raw)
            offset = len(raw) - width * height * 4
            assert fmt in (1, 2) and offset in (12, 16), 'unexpected screencap format'
            pixels = memoryview(raw)[offset:offset + width * (height * 2 // 3) * 4]
            return sum(pixels[i] > 220 and pixels[i + 1] > 220 and pixels[i + 2] > 220 for i in range(0, len(pixels), 4)) > 1000
        wait(page_visible, 'visible HTML text over the native surface')
        (a.output / 'page.png').write_bytes(subprocess.check_output([*base, 'exec-out', 'screencap', '-p'], timeout=30))
        shell("printf 'webshot /proc/nxnative-no-output/page.png\\n' >> " + ctl)
        wait(lambda text: '[RED] webshot: nothing written' in text, 'unwritable diagnostic output is reported')
        shell("printf 'webshot " + shot + "\\n' >> " + ctl)
        wait(lambda _: shell('test -s ' + shot + ' && echo ready || true').strip() == 'ready', 'offline page PNG diagnostic')
        png = subprocess.check_output([*base, 'exec-out', 'cat', shot], timeout=30)
        assert png[:8] == b'\x89PNG\r\n\x1a\n' and png[-8:-4] == b'IEND', 'incomplete page PNG'
        width, height = struct.unpack_from('>II', png, 16)
        assert width >= 1280 and width * 9 == height * 16, (width, height)
        (a.output / 'webshot.png').write_bytes(png)
        shell("printf 'dump\\n' >> " + ctl)
        wait(lambda text: 'thread snapshot tid=' in text and 'pc=' in text, 'live native thread diagnostics in the APK')
        adb('shell', 'input', 'keyevent', 'KEYCODE_BUTTON_B')  # Android's right button = Switch A.
        wait(lambda text: 'WEB_WAIT_B' in text, 'A delivered only to the page; requested exit')
        adb('shell', 'input', 'keyevent', 'KEYCODE_BUTTON_A')  # Android's bottom button = Switch B.
        wait(lambda text: 'WEB_WAIT_SYSTEM_BACK' in text, 'controller Back returned its result')
        adb('shell', 'input', 'keyevent', 'KEYCODE_BACK')
        wait(lambda text: 'WEB_WAIT_EXIT_INPUT' in text, 'system Back ended the guest session')
        wait(lambda _: 'mCurrentFocus=' in (state := shell('dumpsys window')) and
             '/com.nxnative.GameActivity}' in next(line for line in state.splitlines() if 'mCurrentFocus=' in line),
             'ended page releases its input window before WaitForExit')
        adb('shell', 'input', 'keyevent', 'KEYCODE_BUTTON_Y')  # Switch X goes to the game again.
        wait(lambda text: 'WEB_FIXTURE_DONE' in text, 'Back, callback URL, script end and missing document')
        shell("printf 'webshot " + shot + "\\n' >> " + ctl)
        wait(lambda text: 'webshot: no page' in text or 'webshot: no ready page' in text, 'diagnostic after applet closure')
        assert subprocess.check_output([*base, 'exec-out', 'cat', shot], timeout=30) == png, 'closed applet replaced the capture'
        text = log()
        expected = ['WEB_REQUEST_EXIT_OK', 'WEB_BACK_OK', 'WEB_SYSTEM_BACK_OK', 'WEB_EXIT_INPUT_OK',
                    'WEB_CALLBACK_OK', 'WEB_SCRIPT_EXIT_OK', 'WEB_HTTP_FAILURE_OK', 'WEB_FIXTURE_DONE']
        for marker in expected: assert marker in text, marker
        (a.output / 'log.txt').write_text(text)
        (a.output / 'result.json').write_text(json.dumps({'checks': [*expected, 'webshot PNG'], 'passed': True}, indent=2))
        print('PASS: offline HTML, messages, input isolation and all four exit reasons', flush=True)
    finally:
        (a.output / 'log.txt').write_text(log())
        adb('shell', 'am', 'force-stop', PACKAGE)


if __name__ == '__main__': main()
