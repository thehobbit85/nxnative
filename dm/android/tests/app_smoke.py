#!/usr/bin/env python3
"""Exercise a synthetic APK on an explicitly selected ARM64 Android emulator.

Installs/updates only the fixture's .test package. Requires app_fixture.py's modules
and a --test APK with bundled or already-deployed matching assets. Artifacts contain no commercial game data.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import shlex
import subprocess
import time
import zipfile

PACKAGE = 'com.nxnative.t0100633007d48000.test'
EXTERNAL = '/sdcard/Android/data/' + PACKAGE + '/files'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--serial', required=True); p.add_argument('--apk', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--stress-cycles', type=int, default=0, help='cycles per game process for the --lifecycle fixture on an AOSP emulator')
    p.add_argument('--adb', default=str(Path(os.environ.get('ANDROID_SDK_ROOT', '')) / 'platform-tools/adb'))
    a = p.parse_args()
    if not a.serial.startswith('emulator-'): p.error('this automated install test is restricted to an emulator')
    a.output.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(a.apk) as apk:
        manifest = 'assets/game/files.txt' if 'assets/game/files.txt' in apk.namelist() else 'assets/game/external-files.txt'
        assets_id = hashlib.sha256(apk.read(manifest)).hexdigest()
    base = [a.adb, '-s', a.serial]

    def adb(*args, binary=False):
        return subprocess.check_output([*base, *args], text=not binary, stderr=subprocess.STDOUT, timeout=30)

    def shell(command): return adb('shell', command)
    def log(): return shell('cat ' + shlex.quote(EXTERNAL + '/log.txt') + ' 2>/dev/null || true')
    def wait_for(predicate, name, seconds=20):
        until = time.monotonic() + seconds
        while time.monotonic() < until:
            if predicate(): return
            time.sleep(0.2)
        raise AssertionError('Timed out: ' + name + '\n' + log()[-5000:])

    print(adb('install', '-r', str(a.apk.resolve())), flush=True)
    details = shell('dumpsys package ' + PACKAGE)
    match = re.search(r'versionCode=(\d+)', details)
    assert match is not None, 'installed package has no version code'
    version = match.group(1)
    adb('shell', 'am', 'force-stop', PACKAGE)
    adb('shell', 'am', 'start', '-W', '-n', PACKAGE + '/com.nxnative.MenuActivity')
    wait_for(lambda: shell('cat ' + EXTERNAL + '/.unpacked 2>/dev/null || true').strip() == assets_id, 'matching assets installed')
    shell("printf 'DM_DBG=1\\n" + ("DM_AUDIO=1\\n" if a.stress_cycles else "") + "' > " + EXTERNAL + '/env.txt')

    def start_guest():
        shell('rm -f ' + EXTERNAL + '/log.txt')
        adb('shell', 'input', 'keyevent', 'KEYCODE_BUTTON_START')
        wait_for(lambda: 'APP_FIXTURE_FRAME' in log(), 'SDK, save commit and first presented frame')
        current = log()
        assert 'APP_FIXTURE_SAVE_COMMITTED' in current and 'APP_FIXTURE_ASSETS_READY' in current and 'APP_FIXTURE_FAILED' not in current
        return current

    if a.stress_cycles:
        if a.stress_cycles < 3: p.error('--stress-cycles requires at least three cycles')
        from app_lifecycle_stress import run
        try:
            result = run(base, PACKAGE, start_guest, shell, log, wait_for, a.output, a.stress_cycles)
            (a.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
            print(json.dumps(result), flush=True)
        finally:
            (a.output / 'last-process.log').write_text(log())
            adb('shell', 'am', 'force-stop', PACKAGE)
        return

    start_guest()
    # Framework key input -> native frontend -> shared HID -> relocated guest import.
    adb('shell', 'input', 'keyevent', 'KEYCODE_BUTTON_A')
    wait_for(lambda: 'APP_FIXTURE_BUTTON_DOWN' in log() and 'APP_FIXTURE_BUTTON_UP' in log(), 'guest button press and release')
    adb('shell', 'input', 'keyevent', 'KEYCODE_HOME')
    wait_for(lambda: 'front end: paused' in log(), 'background graphics drain')
    paused = log(); frame_count = paused.count('APP_FIXTURE_FRAME')
    time.sleep(1.2)
    assert log().count('APP_FIXTURE_FRAME') == frame_count, 'guest kept presenting after pause'
    adb('shell', 'am', 'start', '-W', '-n', PACKAGE + '/com.nxnative.MenuActivity')
    wait_for(lambda: log().count('front end: resumed') >= 2 and log().count('APP_FIXTURE_FRAME') > frame_count, 'resume rendering')
    resumed = log()
    assert resumed.count('APP_FIXTURE_MAIN') == 1, 'resume restarted the guest'
    assert resumed.count('swapchain ') >= 2, 'surface recreation was not exercised'
    (a.output / 'lifecycle.log').write_text(resumed)
    (a.output / 'frame.png').write_bytes(adb('exec-out', 'screencap', '-p', binary=True))
    # A process death and an APK update must both retain a committed save.
    adb('shell', 'am', 'force-stop', PACKAGE)
    print(adb('install', '-r', str(a.apk.resolve())), flush=True)
    adb('shell', 'am', 'start', '-W', '-n', PACKAGE + '/com.nxnative.MenuActivity')
    wait_for(lambda: PACKAGE in shell('dumpsys activity activities'), 'menu relaunched')
    second = start_guest()
    assert 'APP_FIXTURE_SAVE_RETAINED' in second, 'committed save was lost across process death / APK update'
    assert 'CRASH' not in second and 'APP_FIXTURE_FAILED' not in second
    (a.output / 'relaunch.log').write_text(second)
    adb('shell', 'input', 'keyevent', 'KEYCODE_HOME')
    wait_for(lambda: 'front end: paused' in log(), 'final pause')
    result = dict(serial=a.serial, version=version, bootstrap=True, vulkan_present=True, input=True, dlc_and_fonts=True,
                  pause_resume=True, window_recreated=True, save_retained_after_update=True)
    (a.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result), flush=True)


if __name__ == '__main__':
    main()
