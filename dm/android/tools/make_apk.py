#!/usr/bin/env python3
# The Android app of one game: the menu (app/java), the host library (make -C dm/android GAME=<id>), the game's icon, and optionally a save to start from and a cheat file,
# signed with a development key. The game files are not in the APK (several GB): the deploy step copies them into the app's external folder over adb.
#   make_apk.py <id> <NSP build folder> <out.apk> [--save <folder>] [--cheats <file>] [--label <name>] [--bundle]
#     --bundle: the game files go inside the APK too (assets/game/, deflated, listed in assets/game/files.txt) and the menu unpacks them on the first start: one file, no
#     computer needed. Every file is stored as pieces, each piece once (assets/game/p/<hash>): archives of (hash, start, end) entries, such as Metroid Dread's .pkg files, are cut at
#     their entries, which repeat across archives (Dread's 4.7 GB deflated becomes 3.7 GB), and whole files that repeat are stored once. A game that still does not fit
#     (Super Mario Odyssey: 5.3 GB of already compressed archives) fills the APK to 3.8 GB and puts the rest, whole files, into <out>-datos.zip beside it (marked =d in
#     files.txt; assets/game/datos.txt names it): the menu asks the player to pick that file once, on the first start. --test: package <package>.test, label "(test)": installs beside the real app without touching it. Android takes APKs below 4 GB only; past 2 GB the ZIP stays without ZIP64 records (apksigner and the installer read it as a plain ZIP)
#   make_apk.py --deploy <id> <NSP build folder> [--from-device <folder on the device that already has romfs/dm/vk/font>]
# Tools: ANDROID_SDK_ROOT (build-tools and platform installed), JAVA_HOME, and --library.
# A development signing key is kept in the prepared build folder so updates retain app data.
import shlex
import argparse, glob, hashlib, os, re, shutil, struct, subprocess, sys, tempfile, zipfile, time
from pathlib import Path
from xml.sax.saxutils import escape
HERE = os.path.dirname(os.path.abspath(__file__)); ANDROID = os.path.dirname(HERE)
SDK = BT = JAR = JAVA = ''
ENV = os.environ.copy()
APK_GAME_LIMIT = int(os.environ.get('APK_GAME_LIMIT', '3800000000'))


def configure_tools(a):
    global SDK, BT, JAR, JAVA, ENV
    SDK = a.sdk or os.environ.get('ANDROID_SDK_ROOT') or os.environ.get('ANDROID_HOME') or os.environ.get('ANDROID_SDK') or ''
    if not SDK:
        raise ValueError('set ANDROID_SDK_ROOT or --sdk to an installed Android SDK')
    def newest(pattern):
        matches = glob.glob(os.path.join(SDK, pattern))
        if not matches:
            raise ValueError('Android SDK is missing ' + pattern)
        return max(matches, key=lambda p: tuple(int(x) for x in re.findall(r'\d+', os.path.relpath(p, SDK))))
    BT = a.build_tools or newest('build-tools/*/aapt2').rsplit('/', 1)[0]
    JAR = a.android_jar or newest('platforms/android-*/android.jar')
    JAVA = os.environ.get('JAVA_HOME', '')
    if not JAVA:
        javac = shutil.which('javac')
        if not javac:
            raise ValueError('set JAVA_HOME or put javac on PATH')
        JAVA = str(Path(javac).resolve().parent.parent)
    ENV = dict(os.environ, JAVA_HOME=JAVA, PATH=os.path.join(JAVA, 'bin') + os.pathsep + os.environ.get('PATH', ''))

def run(*a, **k): subprocess.run([str(x) for x in a], check=True, env=ENV, **k)
def package(gid, test=False): return 'com.nxnative.t' + gid.lower() + ('.test' if test else '')   # test: installs beside the real app, its own data

MANIFEST = '''<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="{pkg}" android:versionCode="{vc}" android:versionName="{vn}">
  <uses-sdk android:minSdkVersion="30" android:targetSdkVersion="33"/>
  <uses-feature android:name="android.hardware.vulkan.version" android:version="0x401000" android:required="true"/>
  <application android:label="{label}" android:icon="@mipmap/icon" android:hasCode="true" android:extractNativeLibs="true" android:isGame="true"
               android:appCategory="game" android:theme="@android:style/Theme.DeviceDefault.NoActionBar.Fullscreen">
    <activity android:name="com.nxnative.MenuActivity" android:exported="true" android:screenOrientation="sensorLandscape"
              android:configChanges="orientation|screenSize|keyboard|keyboardHidden|navigation|uiMode|screenLayout|smallestScreenSize">
      <intent-filter><action android:name="android.intent.action.MAIN"/><category android:name="android.intent.category.LAUNCHER"/></intent-filter>
    </activity>
    <activity android:name="android.app.NativeActivity" android:process=":game" android:exported="false" android:screenOrientation="sensorLandscape"
              android:configChanges="orientation|screenSize|keyboard|keyboardHidden|navigation|uiMode|screenLayout|smallestScreenSize|density">
      <meta-data android:name="android.app.lib_name" android:value="dmhost"/>
    </activity>
  </application>
</manifest>
'''

def build(a):
    configure_tools(a)
    lib = a.library or os.path.join(ANDROID, 'gen', a.id, 'libdmhost.so')
    if not os.path.exists(lib): sys.exit(f'[RED] {lib} missing: make -C dm/android GAME={a.id} first')
    with tempfile.TemporaryDirectory(prefix='apk_', dir=os.environ.get('WORK')) as w:
        label = (a.label or a.id) + (' (test)' if a.test else '')
        vc = a.version_code or int(time.time())   # increase --version-code for an update; seconds avoid the donor's hourly collisions
        if not 1 <= vc <= 2100000000: raise ValueError('version code must be between 1 and 2100000000')
        open(os.path.join(w, 'AndroidManifest.xml'), 'w').write(MANIFEST.format(pkg=package(a.id, a.test), vc=vc, vn='0.1.' + str(vc), label=escape(label, {'"': '&quot;'})))
        res = os.path.join(w, 'res', 'mipmap'); os.makedirs(res)
        icon = (glob.glob(os.path.join(a.build, 'extract', '*', 'icon.jpg')) or [None])[0]
        if icon: shutil.copyfile(icon, os.path.join(res, 'icon.jpg'))
        else: Path(res, 'icon.xml').write_text('<shape xmlns:android="http://schemas.android.com/apk/res/android" android:shape="rectangle"><solid android:color="#24344B"/><corners android:radius="24dp"/></shape>')
        run(os.path.join(BT, 'aapt2'), 'compile', '--dir', os.path.join(w, 'res'), '-o', os.path.join(w, 'res.zip'))
        base = os.path.join(w, 'base.apk')
        run(os.path.join(BT, 'aapt2'), 'link', '-o', base, '-I', JAR, '--manifest', os.path.join(w, 'AndroidManifest.xml'), os.path.join(w, 'res.zip'))
        cls = os.path.join(w, 'classes'); os.makedirs(cls)
        run(os.path.join(JAVA, 'bin', 'javac'), '--release', '11', '-cp', JAR, '-d', cls, *glob.glob(os.path.join(ANDROID, 'app', 'java', '**', '*.java'), recursive=True))
        run(os.path.join(BT, 'd8'), '--release', '--min-api', '30', '--lib', JAR, '--output', w, *glob.glob(os.path.join(cls, '**', '*.class'), recursive=True))
        with zipfile.ZipFile(base, 'a', zipfile.ZIP_DEFLATED) as z:
            z.write(os.path.join(w, 'classes.dex'), 'classes.dex')
            z.write(lib, 'lib/arm64-v8a/libdmhost.so')
            if a.cheats:
                fixups = Path(a.build, 'patched_android', 'main.fixups.tsv').read_text()
                match = re.search(r'^# BUILD_ID\t([0-9a-fA-F]{16,64})', fixups, re.M)
                if not match:
                    raise ValueError('main.fixups.tsv has no build_id for the supplied cheats')
                bid = match.group(1)[:16].upper()
                z.write(a.cheats, 'assets/cheats.txt')
                z.write(a.cheats, 'assets/cheats/' + bid + '.txt')
                z.writestr('assets/cheats-build.txt', bid)
            if a.save:
                for root, _, fs in os.walk(a.save):
                    for f in sorted(fs):
                        if not f.startswith('.'): p = os.path.join(root, f); z.write(p, 'assets/save/' + os.path.relpath(p, a.save))
            if a.bundle:
                listing = []; stored = set(); raw = 0; dz = None; dname = os.path.splitext(os.path.basename(a.out))[0] + '-datos.zip'; dn = dbytes = 0
                apk_file = z.fp
                assert apk_file is not None
                for rel, src in game_files(a.build):
                    if dz is None and apk_file.tell() > APK_GAME_LIMIT:   # the APK is full: the rest goes whole into the data file the menu asks for (see the header)
                        dz = zipfile.ZipFile(os.path.join(os.path.dirname(os.path.abspath(a.out)), dname), 'w', zipfile.ZIP_DEFLATED, allowZip64=False)
                        dz.writestr('nxdatos.txt', f'{package(a.id, a.test)}\t{vc}\n')
                    if dz is not None:
                        dz.write(src, rel); listing.append(f'{rel}\t{os.path.getsize(src)}\t=d'); dn += 1; dbytes += os.path.getsize(src); continue
                    d = open(src, 'rb').read(); names = []
                    for off, n in pieces(d):
                        k = hashlib.sha1(d[off:off + n]).hexdigest()[:24]; names.append(k)
                        if k not in stored: stored.add(k); z.writestr(zipfile.ZipInfo('assets/game/p/' + k, (2020, 1, 1, 0, 0, 0)), d[off:off + n], zipfile.ZIP_DEFLATED); raw += n
                    listing.append(f'{rel}\t{len(d)}\t' + ' '.join(names))
                z.writestr('assets/game/files.txt', '\n'.join(listing) + '\n')
                print(f'game: {len(listing)} files, {len(stored)} pieces, {raw / 1e9:.2f} GB stored once')
                if dz is not None:
                    dz.close(); z.writestr('assets/game/datos.txt', f'{dname}\t{dn}\t{dbytes}\n')
                    print(f'data file {dname}: {dn} files, {dbytes / 1e9:.2f} GB, {os.path.getsize(os.path.join(os.path.dirname(os.path.abspath(a.out)), dname)) / 1e9:.2f} GB compressed')
        if os.path.getsize(base) >= 0xFFFFFFFF: sys.exit(f'[RED] the APK would be {os.path.getsize(base) / 1e9:.1f} GB: Android installs APKs below 4 GB only')
        aligned = os.path.join(w, 'aligned.apk')
        run(os.path.join(BT, 'zipalign'), '-f', '-P', '16', '4', base, aligned)
        ks = os.path.abspath(a.keystore or os.path.join(a.build, 'android-debug.keystore'))
        if not os.path.exists(ks):
            run(os.path.join(JAVA, 'bin', 'keytool'), '-genkeypair', '-keystore', ks, '-storepass', 'android', '-keypass', 'android', '-alias', 'nxnative', '-keyalg', 'RSA', '-keysize', '2048',
                '-validity', '10000', '-dname', 'CN=nxnative development', stdout=subprocess.DEVNULL)
        run(os.path.join(BT, 'apksigner'), 'sign', '--ks', ks, '--ks-pass', 'pass:android', '--key-pass', 'pass:android', '--ks-key-alias', 'nxnative', '--out', a.out, aligned)
        print(f'{a.out}: {package(a.id, a.test)} "{label}", {os.path.getsize(a.out) / 1e6:.1f} MB' + (f', save from {a.save}' if a.save else '') + (f', cheats {os.path.basename(a.cheats)}' if a.cheats else ''))

def pieces(d):   # (offset, length) ranges that cover the file: an archive of (u64 hash, u32 start, u32 end) entries after (u32 header size past this word, u32 data size, u32 count)
    if len(d) >= 12:   # is cut at its entries
        hs, ds, n = struct.unpack_from('<III', d, 0); hs += 4
        if 0 < n and 12 + 16 * n <= hs and hs + ds == len(d):
            ents = sorted(struct.unpack_from('<QII', d, 12 + 16 * i)[1:] for i in range(n)); out = []; pos = 0
            if all(hs <= s <= e <= len(d) for s, e in ents):
                for s, e in ents:
                    if s < pos: break   # overlapping entries: kept whole
                    if s > pos: out.append((pos, s - pos))
                    if e > s: out.append((s, e - s))
                    pos = e
                else:
                    if pos < len(d): out.append((pos, len(d) - pos))
                    return out
    return [(0, len(d))]

def game_files(b):   # (path in the app's external folder, source) for every game file
    vk = os.path.join(b, 'shader', 'vk'); out = []
    if not os.path.isdir(vk): raise ValueError('missing shader/vk: export Android shaders first')
    if not glob.glob(os.path.join(b, 'patched_android', '*.patched.bin')): raise ValueError('missing patched_android modules: rewrite with --thread-slots android first')
    for module in Path(b, 'patched_android').glob('*.patched.bin'):
        metadata = module.with_name(module.name.replace('.patched.bin', '.fixups.tsv'))
        if not metadata.is_file() or not re.search(r'^# TP_SLOT\t2$', metadata.read_text(), re.M):
            raise ValueError(str(module) + ': not an Android rewrite; run patch_nso.py --thread-slots android')
    for f in sorted(glob.glob(os.path.join(b, 'patched_android', '*.patched.bin')) + glob.glob(os.path.join(b, 'patched_android', '*.tsv'))): out.append(('dm/' + os.path.basename(f), f))
    for f in sorted(glob.glob(os.path.join(vk, '*.spv')) + glob.glob(os.path.join(vk, '*.vk.txt'))): out.append(('vk/' + os.path.basename(f), f))
    for f in sorted(glob.glob(os.path.join(b, 'font', '*'))): out.append(('font/' + os.path.basename(f), f))
    romfs = glob.glob(os.path.join(b, 'extract', '*', 'RomFS'))
    if len(romfs) != 1: raise ValueError('expected one extract/*/RomFS folder')
    romfs = romfs[0]
    for root, _, fs in os.walk(romfs):
        for f in sorted(fs): p = os.path.join(root, f); out.append(('romfs/' + os.path.relpath(p, romfs), p))
    return out

def deploy(a):
    sdk = a.sdk or os.environ.get('ANDROID_SDK_ROOT') or os.environ.get('ANDROID_HOME')
    adb = os.path.join(sdk, 'platform-tools', 'adb') if sdk else 'adb'
    ext = f'/sdcard/Android/data/{package(a.id)}/files'
    if a.from_device:   # copied on the device: much faster than over USB
        run(adb, '-s', a.serial, 'shell', f'mkdir -p {ext} && for d in romfs dm vk font; do rm -rf {ext}/$d; cp -r {shlex.quote(a.from_device)}/$d {ext}/$d; done')
    else:   # adb cannot make folders inside Android/data (Android 13: secure_mkdirs is refused): pushed to /data/local/tmp, then copied on the device
        b = a.build; tmp = '/data/local/tmp/nx_deploy_' + a.id.lower()   # the folders themselves: adb push copies symbolic links as links, which the device refuses
        src = {'dm': os.path.join(b, 'patched_android'), 'vk': glob.glob(os.path.join(b, 'shader', 'vk'))[0], 'font': os.path.join(b, 'font'), 'romfs': glob.glob(os.path.join(b, 'extract', '*', 'RomFS'))[-1]}
        run(adb, '-s', a.serial, 'shell', f'rm -rf {tmp}; mkdir -p {tmp} {ext}')
        for d in ('dm', 'vk', 'font', 'romfs'):
            run(adb, '-s', a.serial, 'push', src[d], f'{tmp}/{d}', stdout=subprocess.DEVNULL)
            run(adb, '-s', a.serial, 'shell', f'rm -rf {ext}/{d} && cp -r {tmp}/{d} {ext}/{d} && rm -rf {tmp}/{d}')
        run(adb, '-s', a.serial, 'shell', f'rm -rf {tmp}')
    print(f'game files in {ext}')

def main(argv=None):
    ap = argparse.ArgumentParser(description='Package an ARM64 Android host and a prepared game folder.')
    ap.add_argument('id'); ap.add_argument('build'); ap.add_argument('out', nargs='?')
    ap.add_argument('--deploy', action='store_true'); ap.add_argument('--from-device'); ap.add_argument('--serial')
    ap.add_argument('--save'); ap.add_argument('--cheats'); ap.add_argument('--label')
    ap.add_argument('--bundle', action='store_true'); ap.add_argument('--test', action='store_true')
    ap.add_argument('--library'); ap.add_argument('--sdk'); ap.add_argument('--build-tools'); ap.add_argument('--android-jar')
    ap.add_argument('--keystore'); ap.add_argument('--version-code', type=int)
    a = ap.parse_args(argv)
    if not re.fullmatch(r'[0-9a-fA-F]{16}', a.id): ap.error('id must be a 16-digit title ID')
    if a.deploy and not a.serial: ap.error('--deploy requires an explicit --serial')
    if not a.deploy and not a.out: ap.error('an output APK path is required')
    try:
        if a.deploy: deploy(a)
        else:
            zipfile.ZIP64_LIMIT = 0xFFFFFFFE
            build(a)
    except (ValueError, OSError, subprocess.CalledProcessError) as e:
        ap.exit(1, '[RED] ' + str(e) + '\n')

if __name__ == '__main__':
    main()
