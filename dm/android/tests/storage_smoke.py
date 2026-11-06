#!/usr/bin/env python3
"""Verify storage selection in the real menu using the synthetic test game.

Build the bundled and optional companion APK with the same --version-code
to allow repeated update tests while preserving the existing fixture save.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import time
import xml.etree.ElementTree as ET
import zipfile

from app_smoke import PACKAGE


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--adb', required=True); parser.add_argument('--serial', required=True)
    parser.add_argument('--apk', required=True, type=Path); parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--companion-apk', type=Path, help='same synthetic game built with APK_GAME_LIMIT=1')
    a = parser.parse_args()
    assert a.serial.startswith('emulator-')
    with zipfile.ZipFile(a.apk) as apk:
        manifest = apk.read('assets/game/files.txt')
    identity = hashlib.sha256(manifest).hexdigest()
    fixture = hashlib.sha256(b'Synthetic APK integration fixture\n').hexdigest().encode()
    assert any(row.startswith(b'romfs/fixture.txt\t') and row.endswith(fixture) for row in manifest.splitlines())
    a.output.mkdir(parents=True, exist_ok=True)
    def adb(*args):
        return subprocess.check_output([a.adb,'-s',a.serial,*args], text=True, timeout=45)
    def shell(cmd): return adb('shell',cmd).strip()
    def root(cmd): return shell('su 0 sh -c ' + shlex.quote(cmd))
    def nodes():
        xml = '/data/local/tmp/nx-storage-window.xml'
        shell('uiautomator dump ' + xml)
        return list(ET.fromstring(shell('cat ' + xml)).iter('node'))
    def wait(test, why):
        end = time.monotonic() + 24
        while time.monotonic() < end:
            if test(): return
            time.sleep(.18)
        raise AssertionError(why)
    def tap(prefix, scroll_files=False):
        def locate():
            window = nodes()
            for node in reversed(window):
                label = node.get('text') or node.get('content-desc','')
                if label.lower().startswith(prefix):
                    x,y,r,b = map(int,re.findall(r'\d+',node.get('bounds')))
                    shell(f'input tap {(x+r)//2} {(y+b)//2}'); return True
            if scroll_files:
                for node in window:
                    if node.get('resource-id') == 'com.android.documentsui:id/dir_list' and node.get('scrollable') == 'true':
                        x,y,r,b = map(int,re.findall(r'\d+',node.get('bounds')))
                        mid = (x+r)//2
                        shell(f'input swipe {mid} {b-30} {mid} {(y+b)//2} 350')
                        break
            return False
        wait(locate,'missing menu control: ' + prefix)
    def choose(name):
        tap('game data location'); tap(name)

    external = '/storage/emulated/0/Android/data/' + PACKAGE + '/files'
    files = '/data/user/0/' + PACKAGE + '/files'
    private = files + '/game-data'; selector = files + '/game-storage.txt'
    def menu():
        shell('am force-stop ' + PACKAGE)
        # A system file picker can outlive its caller after a test is interrupted.
        shell('am start -W --activity-clear-task -n ' + PACKAGE + '/com.nxnative.MenuActivity')
        wait(lambda: 'MenuActivity' in shell('dumpsys window | grep mCurrentFocus'), 'menu did not open')
    def selected(): return root('cat ' + selector + ' 2>/dev/null || true')
    def save(): return root('od -An -tu4 ' + files + '/save/integration.bin').split()
    def launch(label):
        shell('rm -f ' + external + '/log.txt')
        shell('input keyevent KEYCODE_BUTTON_START')
        def log(): return shell('cat ' + external + '/log.txt 2>/dev/null || true')
        wait(lambda: 'APP_FIXTURE_FRAME' in log(), 'game could not read assets from ' + label)
        data = log(); assert 'APP_FIXTURE_ASSETS_READY' in data and 'APP_FIXTURE_FAILED' not in data
        (a.output / (label+'.log')).write_text(data)
        return save()

    # Hide only the synthetic game's modules while it runs from its private copy.
    # Refuse to overwrite any backup left by an interrupted test.
    hidden = external + '/.nx-storage-test-dm'
    assert root('id -u') == '0' and root('test -e ' + hidden + ' && echo exists || true') == ''
    adb('install','-r',str(a.apk.resolve())); menu()
    if selected() not in ('','primary',external):
        choose('primary shared storage'); wait(lambda: selected() == 'primary','select initial primary')
    wait(lambda: shell('cat ' + external + '/.unpacked 2>/dev/null || true') == identity,'prepare initial assets')
    first = launch('primary'); menu()
    try:
        # Force a new copy even if a previous run prepared a private set. The
        # source file and existing target marker are both restored on failure.
        asset = external + '/romfs/fixture.txt'
        marker = private + '/.unpacked'
        asset_bak = external + '/.nx-test-fixture'
        marker_bak = private + '/.nx-test-stamp'
        assert root(f'if test -e {asset_bak} || test -e {marker_bak}; then echo backup; fi') == ''
        assert root('sha256sum ' + asset).split()[0] == fixture.decode()
        try:
            root(f'if test -f {marker}; then mv {marker} {marker_bak}; fi')
            root(f'mv {asset} {asset_bak}')
            choose('private app storage')
            wait(lambda: any(n.get('text','').startswith('Could not switch game storage') for n in nodes()), 'copy error not shown')
            assert selected() in ('','primary',external) and save() == first
            root(f'mv {asset_bak} {asset}')
            choose('private app storage')
            wait(lambda: selected() == 'private','retry damaged copy')
        finally:
            root(f'if test -f {asset_bak}; then mv {asset_bak} {asset}; fi')
            root(f'if test -f {marker_bak}; then if test -f {marker}; then rm {marker_bak}; else mv {marker_bak} {marker}; fi; fi')
        assert root('cat ' + private + '/.unpacked') == identity
        settings = root('sha256sum ' + files + '/settings.env').split()[0]
        root('mv ' + external + '/dm ' + hidden)
        second = launch('private')
        assert second[0] == first[0] and int(second[1]) == int(first[1])+1
        assert root('sha256sum ' + files + '/settings.env').split()[0] == settings
        menu()
        # Invalid persisted media selection must leave the chooser reachable.
        root('printf missing-volume > ' + selector); menu()
        choose('private app storage'); wait(lambda: selected() == 'private','recover missing volume')
        assert save() == second
    finally:
        shell('am force-stop ' + PACKAGE)
        root('if test -d ' + hidden + '; then mv ' + hidden + ' ' + external + '/dm; fi')
    menu(); choose('primary shared storage'); wait(lambda: selected() == 'primary','switch back to primary')
    third = launch('primary-again')
    assert int(third[1]) == int(second[1])+1 and root('sha256sum ' + files + '/settings.env').split()[0] == settings
    shell('am force-stop ' + PACKAGE)
    result = dict(primary=True,private=True,copy_retry=True,unavailable_volume_recovered=True,save_counters=[first[1],second[1],third[1]])
    if a.companion_apk:
        with zipfile.ZipFile(a.companion_apk) as apk:
            data_name = apk.read('assets/game/datos.txt').decode().split('\t')[0]
            assert Path(data_name).name == data_name
            companion_id = hashlib.sha256(apk.read('assets/game/files.txt')).hexdigest()
        bad = a.output/'wrong.zip'
        with zipfile.ZipFile(bad,'w') as z: z.writestr('nxdatos.txt','wrong.package\t1\n')
        folder = shell('mktemp -d /sdcard/Download/nx-storage-XXXXXX')
        assert re.fullmatch(r'/sdcard/Download/nx-storage-[a-zA-Z0-9]+',folder)
        def pick(name):
            tap('choose data file'); tap('show roots'); tap(shell('getprop ro.product.model').lower())
            tap('download',scroll_files=True); tap(Path(folder).name.lower(),scroll_files=True)
            if any(n.get('content-desc') == 'List view' for n in nodes()):
                tap('list view')
            tap(name)
        try:
            adb('push',str(bad),folder+'/wrong.zip')
            adb('push',str(a.companion_apk.parent/data_name),folder+'/correct.zip')
            adb('install','-r',str(a.companion_apk)); menu()
            tap('choose data file')
            # Cancel at a known root after the system picker owns input.
            tap('show roots'); tap('recent')
            wait(lambda: any(n.get('content-desc') == 'Show roots' and n.get('enabled') == 'true' for n in nodes()),
                 'picker did not finish navigation')
            shell('input keyevent KEYCODE_BACK')
            wait(lambda: 'MenuActivity' in shell('dumpsys window | grep mCurrentFocus'),'cancel file picker')
            pick('wrong.zip')
            wait(lambda: any('different game build' in n.get('text','') for n in nodes()),'wrong archive rejected')
            assert save() == third and selected() == 'primary'
            pick('correct.zip')
            wait(lambda: shell('cat '+external+'/.unpacked') == companion_id,'retry companion import')
            last = launch('companion'); assert int(last[1]) == int(third[1])+1
            assert root('sha256sum '+files+'/settings.env').split()[0] == settings
            result['save_counters'].append(last[1])
            result.update(companion_cancel=True,companion_retry=True)
        finally:
            shell('am force-stop ' + PACKAGE)
            shell(f'rm -f {folder}/wrong.zip {folder}/correct.zip; rmdir {folder}')
    (a.output/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result))


if __name__ == '__main__': main()
