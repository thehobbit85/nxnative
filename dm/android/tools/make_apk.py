#!/usr/bin/env python3
# The Android app of one game: the menu (app/java), the host library (make -C dm/android GAME=<id>), the game's icon, and optionally a save to start from and a cheat file,
# signed with a development key. The game files are not in the APK (several GB): the deploy step copies them into the app's external folder over adb.
#   make_apk.py <id> <NSP build folder> <out.apk> [--save <folder>] [--cheats <file>] [--label <name>] [--bundle]
#     --bundle: the game files go inside the APK too (assets/game/, deflated, listed in assets/game/files.txt) and the menu unpacks them on the first start: one file, no
#     computer needed. Every file is stored as pieces, each piece once (assets/game/p/<hash>): archives of (hash, start, end) entries, such as Metroid Dread's .pkg files, are cut at
#     their entries, which repeat across archives (Dread's 4.7 GB deflated becomes 3.7 GB), and whole files that repeat are stored once. A game that still does not fit
#     reserves up to 3.8 GB in the APK and puts oversized files into <out>-datos.zip as bounded chunks (marked =d in
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
        icon = a.icon or (glob.glob(os.path.join(a.build, 'extract', '*', 'icon.jpg')) or [None])[0]
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
            if a.language: z.writestr('assets/default-language.txt', a.language)
            if a.env_file: z.write(a.env_file, 'assets/default.env')
            if a.cheats:
                fixups = (Path(a.patched or Path(a.build, 'patched_android')) / 'main.fixups.tsv').read_text()
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
                for rel, src in game_files(a.build, a.patched, a.romfs, a.aoc):
                    if dz is None and needs_data_archive(z, src, APK_GAME_LIMIT):
                        # Data archives are streamed, not installed as APKs. Their bounded
                        # entries need no ZIP64 sizes; the central directory may exceed 4 GiB.
                        dz = zipfile.ZipFile(os.path.join(os.path.dirname(os.path.abspath(a.out)), dname), 'w', zipfile.ZIP_DEFLATED, allowZip64=True)
                        dz.writestr('nxdatos.txt', f'{package(a.id, a.test)}\t{vc}\n')
                    if dz is not None:
                        write_data_file(dz, rel, src); listing.append(f'{rel}\t{os.path.getsize(src)}\t=d'); dn += 1; dbytes += os.path.getsize(src); continue
                    names = []
                    for piece in asset_pieces(src):
                        k = hashlib.sha1(piece).hexdigest()[:24]; names.append(k)
                        if k not in stored: stored.add(k); z.writestr(zipfile.ZipInfo('assets/game/p/' + k, (2020, 1, 1, 0, 0, 0)), piece, zipfile.ZIP_DEFLATED); raw += len(piece)
                    listing.append(f'{rel}\t{os.path.getsize(src)}\t' + ' '.join(names))
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

def needs_data_archive(apk, src, limit):
    # Bound the uncompressed file, deflate expansion and ZIP entry headers before
    # writing it. One large RomFS image must not overrun the APK's install limit.
    size = os.path.getsize(src)
    return apk.fp.tell() + size + size // 1000 + ((size >> 21) + 1) * 256 > limit


def write_data_file(archive, rel, src):
    with open(src, 'rb') as stream:
        offset = 0
        while chunk := stream.read(2 << 20):
            archive.writestr(f'{rel}.nxpart/{offset:016x}', chunk)
            offset += len(chunk)
        if not offset: archive.writestr(rel, b'')


def asset_pieces(src):
    with open(src, 'rb') as f:
        if str(src).endswith('.img'):
            while chunk := f.read(2 << 20):
                yield chunk
        else:
            data = f.read()
            for offset, size in pieces(data):
                yield data[offset:offset + size]


def game_files(b, patched=None, romfs=None, aoc=None):   # (path in the app's external folder, source) for every game file
    vk = os.path.join(b, 'shader', 'vk'); out = []
    patched = str(patched or os.path.join(b, 'patched_android'))
    if not os.path.isdir(vk): raise ValueError('missing shader/vk: export Android shaders first')
    if not glob.glob(os.path.join(patched, '*.patched.bin')): raise ValueError('missing patched_android modules: rewrite with --thread-slots android first')
    for module in Path(patched).glob('*.patched.bin'):
        metadata = module.with_name(module.name.replace('.patched.bin', '.fixups.tsv'))
        if not metadata.is_file() or not re.search(r'^# TP_SLOT\t2$', metadata.read_text(), re.M):
            raise ValueError(str(module) + ': not an Android rewrite; run patch_nso.py --thread-slots android')
    for f in sorted(glob.glob(os.path.join(patched, '*.patched.bin')) + glob.glob(os.path.join(patched, '*.tsv'))): out.append(('dm/' + os.path.basename(f), f))
    for f in sorted(glob.glob(os.path.join(vk, '*.spv')) + glob.glob(os.path.join(vk, '*.vk.txt'))): out.append(('vk/' + os.path.basename(f), f))
    for f in sorted(glob.glob(os.path.join(b, 'font', '*'))): out.append(('font/' + os.path.basename(f), f))
    choices = [str(romfs)] if romfs else glob.glob(os.path.join(b, 'extract', '*', 'RomFS')) + glob.glob(os.path.join(b, 'extract', '*', 'RomFS.img'))
    if len(choices) != 1: raise ValueError('select one extracted RomFS with --romfs')
    romfs = choices[0]
    if not os.path.exists(romfs): raise ValueError('missing RomFS: ' + romfs)
    if os.path.isfile(romfs): out.append(('romfs/romfs.img', romfs))
    else:
        for root, _, fs in os.walk(romfs):
            for f in sorted(fs): p = os.path.join(root, f); out.append(('romfs/' + os.path.relpath(p, romfs), p))
    if aoc:
        for root, _, fs in os.walk(aoc):
            for f in sorted(fs): p = os.path.join(root, f); out.append(('aoc/' + os.path.relpath(p, aoc), p))
    return out

def deploy(a):
    sdk = a.sdk or os.environ.get('ANDROID_SDK_ROOT') or os.environ.get('ANDROID_HOME')
    adb = os.path.join(sdk, 'platform-tools', 'adb') if sdk else 'adb'
    ext = f'/sdcard/Android/data/{package(a.id, a.test)}/files'
    stage = ext + '/.nx-deploy'
    roots = ('dm', 'vk', 'font', 'romfs', 'aoc')
    # Validate local inputs before touching device assets. Stage all files, then
    # replace only game roots; internal saves and settings are never deployed.
    files = [] if a.from_device else game_files(a.build, a.patched, a.romfs, a.aoc)
    run(adb, '-s', a.serial, 'shell', 'am force-stop ' + package(a.id, a.test))
    run(adb, '-s', a.serial, 'shell', f'rm -rf {stage} && mkdir -p ' + ' '.join(stage + '/' + d for d in roots))
    if a.from_device:
        source = shlex.quote(a.from_device.rstrip('/'))
        run(adb, '-s', a.serial, 'shell', f'set -e; test -d {source}/dm; test -d {source}/romfs; test -d {source}/vk; '
            + f'for d in dm vk font romfs aoc; do if test -d {source}/$d; then cp -R {source}/$d/. {stage}/$d/; fi; done')
    else:
        parents = sorted({os.path.dirname(stage + '/' + rel) for rel, _ in files})
        if parents: run(adb, '-s', a.serial, 'shell', 'mkdir -p ' + ' '.join(map(shlex.quote, parents)))
        groups = {}
        for rel, src in files:
            dst = stage + '/' + rel
            if os.path.basename(src) != os.path.basename(dst):
                run(adb, '-s', a.serial, 'push', src, dst, stdout=subprocess.DEVNULL)
            else: groups.setdefault(os.path.dirname(dst), []).append(src)
        for parent, sources in groups.items():
            for offset in range(0, len(sources), 64):
                run(adb, '-s', a.serial, 'push', *sources[offset:offset + 64], parent + '/', stdout=subprocess.DEVNULL)
    # ADB-created directories may remain shell-owned 0770 on emulators. The app
    # has a different UID; it needs read/traverse access inside its scoped folder.
    # Touch only staged game assets, never internal saves or external settings.
    run(adb, '-s', a.serial, 'shell', f'set -e; chmod -R a+rX {stage}; for d in dm vk font romfs aoc; do rm -rf {ext}/$d; mv {stage}/$d {ext}/$d; done; rmdir {stage}')
    print(f'game files in {ext}')

def main(argv=None):
    ap = argparse.ArgumentParser(description='Package an ARM64 Android host and a prepared game folder.')
    ap.add_argument('id'); ap.add_argument('build'); ap.add_argument('out', nargs='?')
    ap.add_argument('--deploy', action='store_true'); ap.add_argument('--from-device'); ap.add_argument('--serial')
    ap.add_argument('--patched'); ap.add_argument('--romfs'); ap.add_argument('--aoc')
    ap.add_argument('--icon'); ap.add_argument('--language'); ap.add_argument('--env-file')
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
