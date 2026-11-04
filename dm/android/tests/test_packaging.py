"""Exercise the reused Android asset packer without SDK tools or game dumps."""
from pathlib import Path
import importlib.util
import struct
import io
import hashlib
import json
import zipfile
import tempfile
import time
import shutil
import subprocess
import xml.etree.ElementTree as ET
from types import SimpleNamespace
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('make_apk', Path(__file__).resolve().parents[1] / 'tools/make_apk.py')
assert spec is not None and spec.loader is not None
pack = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pack)


def elf_library(machine=183, kind=3):
    return struct.pack('<16sHHIQQQIHHHHHH', b'\x7fELF\x02\x01\x01' + bytes(9), kind, machine, 1, 0, 0, 0, 0, 64, 0, 0, 0, 0, 0)


def time_tuple():
    return time.struct_time((2026, 1, 1, 12, 0, 0, 3, 1, -1))


class Packaging(unittest.TestCase):
    def package_with_fake_sdk(self, root, payload, fail=None, options=(), limit=1, trace=None, automatic_version=False, bundle=True):
        build = root / 'build'; build.mkdir(exist_ok=True)
        output = root / 'output with spaces'; output.mkdir(exist_ok=True)
        library = build / 'libdmhost.so'; library.write_bytes(elf_library())
        source = build / 'RomFS.img'; source.write_bytes(payload)
        sdk = root / 'sdk'; java = root / 'java'
        for path in [sdk / name for name in ('aapt2', 'd8', 'zipalign', 'apksigner', 'android.jar', 'lib/d8.jar', 'lib/apksigner.jar')] + \
                    [java / name for name in ('bin/java', 'bin/javac', 'bin/keytool', 'release', 'lib/modules')]:
            path.parent.mkdir(parents=True, exist_ok=True)
            if not path.exists(): path.write_bytes(b'fixture tool')
        def configure(_):
            pack.BT = str(sdk); pack.JAR = str(sdk / 'android.jar'); pack.JAVA = str(java)
        def run(*args, **kwargs):
            tool = Path(args[0]).name
            if trace is not None: trace.append((tool, *args[1:]))
            if tool == 'aapt2':
                path = args[args.index('-o') + 1]
                with zipfile.ZipFile(path, 'w') as archive:
                    if 'link' in args: archive.write(args[args.index('--manifest') + 1], 'AndroidManifest.xml')
            elif tool == 'd8':
                Path(args[args.index('--output') + 1], 'classes.dex').write_bytes(b'fixture dex')
            elif tool == 'javac' and fail == 'input-change':
                source.write_bytes(b'changed while compiling')
            elif tool == 'zipalign' and '-c' not in args:
                shutil.copyfile(args[-2], args[-1])
            elif tool == 'zipalign' and fail == 'align':
                raise subprocess.CalledProcessError(1, args)
            elif tool == 'keytool':
                Path(args[args.index('-keystore') + 1]).write_bytes(b'fixture key')
            elif tool == 'apksigner':
                if args[1] == fail:
                    if fail == 'sign': Path(args[args.index('--out') + 1]).write_bytes(b'incomplete signing output')
                    raise subprocess.CalledProcessError(1, args)
                if args[1] == 'sign': shutil.copyfile(args[-1], args[args.index('--out') + 1])
        write_data = pack.write_data_file
        def write(*args):
            digest = write_data(*args)
            if fail == 'data': raise OSError('fixture archive write failure')
            return digest
        with patch.object(pack, 'configure_tools', side_effect=configure), patch.object(pack, 'run', side_effect=run), patch.object(pack, 'write_data_file', side_effect=write), \
             patch.object(pack, 'game_files', return_value=[('romfs/romfs.img', str(source))]), patch.object(pack, 'APK_GAME_LIMIT', limit):
            version = [] if automatic_version else ['--version-code', '101']
            bundled = ['--bundle'] if bundle else []
            pack.main(['0100633007D48000', str(build), str(output / 'Game.apk'), *bundled, '--library', str(library), *version, *options])
        return output / 'Game.apk'

    def test_incomplete_sdk_or_jdk_fails_during_configuration(self):
        for missing in ('aapt2', 'd8', 'zipalign', 'apksigner', 'android.jar', 'java', 'javac', 'keytool'):
            with self.subTest(missing=missing), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp); bt = root / 'SDK with spaces/build-tools/35.0.0'; java = root / 'JDK with spaces'
                jar = root / 'SDK with spaces/platforms/android-35/android.jar'
                paths = [bt / n for n in ('aapt2', 'd8', 'zipalign', 'apksigner')] + [java / 'bin' / n for n in ('java', 'javac', 'keytool')] + [jar]
                for path in paths:
                    if path.name == missing: continue
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_text('#!/bin/sh\necho javac 17.0.1\n'); path.chmod(0o755)
                args = SimpleNamespace(sdk=str(root / 'SDK with spaces'), build_tools=str(bt), android_jar=str(jar))
                with patch.dict(pack.os.environ, {'JAVA_HOME': str(java)}):
                    with self.assertRaisesRegex(ValueError, missing): pack.configure_tools(args)

    def test_unchanged_apk_reuses_verified_products(self):
        for limit, bundle in ((1, True), (1000000, True), (1, False)):
            with self.subTest(limit=limit, bundle=bundle), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp); trace = []
                apk = self.package_with_fake_sdk(root, b'game', limit=limit, bundle=bundle)
                before = {p.name: (p.read_bytes(), p.stat().st_mtime_ns) for p in apk.parent.iterdir()}
                self.assertIn('Game.apk.build.json', list(before))
                self.package_with_fake_sdk(root, b'game', trace=trace, limit=limit, bundle=bundle)
                self.assertEqual(trace, [])
                self.assertEqual({p.name: (p.read_bytes(), p.stat().st_mtime_ns) for p in apk.parent.iterdir()}, before)

    def test_separate_apk_binds_expected_assets_without_bundling_them(self):
        with tempfile.TemporaryDirectory() as tmp:
            apk = self.package_with_fake_sdk(Path(tmp), b'external data', bundle=False)
            with zipfile.ZipFile(apk) as archive:
                self.assertNotIn('assets/game/files.txt', archive.namelist())
                self.assertFalse(any(n.startswith('assets/game/p/') for n in archive.namelist()))
                row = archive.read('assets/game/external-files.txt').decode().strip().split('\t')
                self.assertEqual(row, ['romfs/romfs.img', '13', '=d', hashlib.sha256(b'external data').hexdigest()])

    def test_changed_input_or_tool_invalidates_reuse(self):
        for change in ('same-size data', 'version', 'tool', 'runtime', 'save', 'new save'):
            with self.subTest(change=change), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp); trace = []; save = root / 'save'; save.mkdir(); (save / 'slot').write_bytes(b'old')
                options = ['--save', str(save)]
                apk = self.package_with_fake_sdk(root, b'old data', options=options)
                receipt = apk.with_name(apk.name + '.build.json')
                previous = json.loads(receipt.read_text())['inputs']
                if change == 'version': options += ['--version-name', '2.0']
                if change == 'tool': (root / 'sdk/lib/d8.jar').write_bytes(b'changed tool')
                if change == 'runtime': (root / 'java/lib/modules').write_bytes(b'changed runtime')
                if change == 'save': (save / 'slot').write_bytes(b'new')
                if change == 'new save': (save / 'other-slot').write_bytes(b'new')
                self.package_with_fake_sdk(root, b'new data' if change == 'same-size data' else b'old data', options=options, trace=trace)
                self.assertIn('javac', [c[0] for c in trace])
                self.assertNotEqual(json.loads(receipt.read_text())['inputs'], previous)

    def test_damaged_products_and_receipts_rebuild(self):
        for change in ('apk', 'missing data', 'corrupt data', 'receipt', 'receipt type', 'unsafe name'):
            with self.subTest(change=change), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp); trace = []
                # Fixed time makes rebuilding produce the same data name, testing
                # repair of a corrupt content-addressed archive in place.
                with patch('time.localtime', return_value=time_tuple()):
                    apk = self.package_with_fake_sdk(root, b'game')
                    receipt = apk.with_name(apk.name + '.build.json')
                    saved = json.loads(receipt.read_text())
                    with zipfile.ZipFile(apk) as archive: name = archive.read('assets/game/datos.txt').decode().split('\t')[0]
                    if change == 'apk': apk.write_bytes(b'broken')
                    if change == 'missing data': apk.with_name(name).unlink()
                    if change == 'corrupt data': apk.with_name(name).write_bytes(b'broken')
                    if change == 'receipt': receipt.write_text('{')
                    if change == 'receipt type': receipt.write_text('[]')
                    if change == 'unsafe name':
                        saved['data']['name'] = '../outside.zip'; receipt.write_text(json.dumps(saved))
                    original_digest = pack.file_digest
                    def digest(path):
                        self.assertNotEqual(Path(path).name, 'outside.zip')
                        return original_digest(path)
                    with patch.object(pack, 'file_digest', side_effect=digest):
                        self.package_with_fake_sdk(root, b'game', trace=trace)
                self.assertIn('javac', [c[0] for c in trace])
                with zipfile.ZipFile(apk.with_name(name)) as archive:
                    self.assertEqual(archive.read('romfs/romfs.img.nxpart/0000000000000000'), b'game')

    def test_failed_changed_build_preserves_receipt_and_playable_products(self):
        for failure in ('verify', 'input-change'):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp); apk = self.package_with_fake_sdk(root, b'old data')
                before = {p.name: p.read_bytes() for p in apk.parent.iterdir()}
                with self.assertRaises(SystemExit): self.package_with_fake_sdk(root, b'new data', fail=failure)
                self.assertEqual({p.name: p.read_bytes() for p in apk.parent.iterdir()}, before)

    def test_automatic_version_is_assigned_only_on_rebuild(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); trace = []
            with patch.object(pack.time, 'time', return_value=1700000000):
                apk = self.package_with_fake_sdk(root, b'old data', automatic_version=True)
            old = apk.read_bytes()
            with patch.object(pack.time, 'time', return_value=1700000010):
                self.package_with_fake_sdk(root, b'old data', automatic_version=True, trace=trace)
                self.assertEqual(trace, []); self.assertEqual(apk.read_bytes(), old)
                self.package_with_fake_sdk(root, b'new data', automatic_version=True)
            with zipfile.ZipFile(apk) as archive: manifest = ET.fromstring(archive.read('AndroidManifest.xml'))
            self.assertEqual(manifest.get('{http://schemas.android.com/apk/res/android}versionCode'), '1700000010')

    def test_final_rename_failure_cannot_make_old_apk_reusable_for_new_inputs(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); apk = self.package_with_fake_sdk(root, b'old data')
            old_apk = apk.read_bytes()
            with zipfile.ZipFile(apk) as archive: name = archive.read('assets/game/datos.txt').decode().split('\t')[0]
            old_data = apk.with_name(name).read_bytes()
            replace = pack.os.replace
            def fail(src, dst):
                if Path(src).name == 'signed.apk': raise OSError('fixture publication failed')
                replace(src, dst)
            with patch.object(pack.os, 'replace', side_effect=fail), self.assertRaises(SystemExit):
                self.package_with_fake_sdk(root, b'new data')
            self.assertEqual(apk.read_bytes(), old_apk); self.assertEqual(apk.with_name(name).read_bytes(), old_data)
            trace = []; self.package_with_fake_sdk(root, b'new data', trace=trace)
            self.assertIn('javac', [c[0] for c in trace])

    def test_asset_manifest_identifies_contents_even_for_same_size_updates(self):
        for limit in (1, 1000000):
            with self.subTest(limit=limit), tempfile.TemporaryDirectory() as tmp:
                for payload in (b'old data', b'new data', b''):
                    apk = self.package_with_fake_sdk(Path(tmp), payload, limit=limit)
                    with zipfile.ZipFile(apk) as archive: row = archive.read('assets/game/files.txt').decode().rstrip('\n').split('\t')
                    self.assertEqual(len(row), 4)
                    self.assertEqual(row[3], hashlib.sha256(payload).hexdigest())
                    self.assertEqual(row[1], str(len(payload)))
                    self.assertEqual(row[2] == '=d', limit == 1)

    def test_manifest_keeps_game_version_separate_from_install_version(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            version = '1.5.12620 & "test"'
            apk = self.package_with_fake_sdk(root, b'game', options=['--version-name', version])
            with zipfile.ZipFile(apk) as archive: manifest = ET.fromstring(archive.read('AndroidManifest.xml'))
            android = '{http://schemas.android.com/apk/res/android}'
            self.assertEqual(manifest.get(android + 'versionName'), version)
            self.assertEqual(manifest.get(android + 'versionCode'), '101')

    def test_explicit_invalid_version_codes_fail_before_sdk_work(self):
        for code in ('0', '-1', '2100000001'):
            with self.subTest(code=code), patch.object(pack, 'configure_tools') as configure:
                with self.assertRaises(SystemExit):
                    pack.main(['0100633007D48000', 'missing-build', 'out.apk', '--version-code', code])
                configure.assert_not_called()

    def test_wrong_native_library_fails_before_sdk_work(self):
        for data in (b'not an ELF library', elf_library(machine=62), elf_library(kind=2), elf_library()[:40]):
            with self.subTest(data=data[:20]), tempfile.TemporaryDirectory() as tmp:
                library = Path(tmp, 'lib.so'); library.write_bytes(data)
                with patch.object(pack, 'configure_tools') as configure:
                    with self.assertRaises(SystemExit):
                        pack.main(['0100633007D48000', tmp, tmp + '/out.apk', '--library', str(library)])
                    configure.assert_not_called()

    def test_output_cannot_replace_a_package_input(self):
        for source in ('library', 'romfs', 'keystore'):
            with self.subTest(source=source), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp); lib = root / 'lib.so'; lib.write_bytes(elf_library())
                romfs = root / 'RomFS.img'; romfs.write_bytes(b'game data')
                key = root / 'android-debug.keystore'; key.write_bytes(b'key file')
                output = {'library': lib, 'romfs': romfs, 'keystore': key}[source]
                before = output.read_bytes()
                with patch.object(pack, 'configure_tools') as configure, patch.object(pack, 'game_files', return_value=[('romfs/romfs.img', str(romfs))]):
                    with self.assertRaises(SystemExit):
                        pack.main(['0100633007D48000', tmp, str(output), '--library', str(lib), '--bundle'])
                    configure.assert_not_called()
                    self.assertEqual(output.read_bytes(), before)

    def test_output_cannot_be_collected_as_an_asset_on_next_build(self):
        for directory, option in (('extract/base/RomFS', None), ('romfs directory', '--romfs'),
                                  ('dlc directory', '--aoc'), ('save directory', '--save'),
                                  ('font', None), ('shader/vk', None), ('patched_android', None)):
            with self.subTest(directory=directory), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp); lib = root / 'lib.so'; lib.write_bytes(elf_library())
                asset = root / directory; asset.mkdir(parents=True)
                source = asset / 'data'; source.write_bytes(b'input')
                args = ['0100633007D48000', tmp, str(asset / 'Game.apk'), '--library', str(lib)]
                if option: args += [option, str(asset)]
                with patch.object(pack, 'configure_tools', side_effect=RuntimeError('too late: SDK invoked')), patch.object(pack, 'game_files', return_value=[('romfs/data', str(source))]):
                    with self.assertRaises(SystemExit): pack.main(args)
                self.assertEqual(source.read_bytes(), b'input')
                self.assertFalse((asset / 'Game.apk').exists())

    def test_explicit_missing_assets_fail_before_sdk_work(self):
        for option in ('--save', '--aoc', '--icon', '--env-file', '--cheats', '--romfs'):
            with self.subTest(option=option), tempfile.TemporaryDirectory() as tmp:
                library = Path(tmp, 'lib.so'); library.write_bytes(elf_library())
                with patch.object(pack, 'configure_tools') as configure:
                    with self.assertRaises(SystemExit):
                        pack.main(['0100633007D48000', tmp, tmp + '/out.apk', '--library', str(library), option, tmp + '/missing'])
                    configure.assert_not_called()

    def test_module_and_shader_files_must_be_complete_pairs(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); modules = root / 'patched_android'; modules.mkdir()
            shaders = root / 'shader/vk'; shaders.mkdir(parents=True)
            romfs = root / 'extract/base/RomFS'; romfs.mkdir(parents=True)
            (modules / 'sdk.patched.bin').write_bytes(b'code')
            (modules / 'sdk.fixups.tsv').write_text('# TP_SLOT\t2\n')
            with self.assertRaisesRegex(ValueError, 'main'):
                pack.game_files(tmp)
            (modules / 'main.patched.bin').write_bytes(b'code')
            (modules / 'main.fixups.tsv').write_text('# TP_SLOT\t2\n')
            shader = shaders / '0123456789abcdef.spv'; sidecar = shader.with_suffix('.vk.txt')
            shader.write_bytes(struct.pack('<5I', 0x07230203, 0x10000, 0, 1, 0))
            with self.assertRaisesRegex(ValueError, 'sidecar'):
                pack.game_files(tmp)
            sidecar.write_text('stage vs\n')
            self.assertIn('vk/' + shader.name, dict(pack.game_files(tmp)))
            shader.unlink()
            with self.assertRaisesRegex(ValueError, 'SPIR-V'):
                pack.game_files(tmp)
            sidecar.unlink()
            with self.assertRaisesRegex(ValueError, 'DLC'):
                pack.game_files(tmp, aoc=str(root / 'missing-dlc'))

    def test_failed_rebuild_preserves_existing_apk_and_data(self):
        for failure in ('data', 'sign', 'verify', 'align'):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp); output = root / 'output with spaces'; output.mkdir()
                originals = {'Game.apk': b'previous apk', 'Game-datos.zip': b'previous data archive'}
                for name, data in originals.items(): (output / name).write_bytes(data)
                with self.assertRaises(SystemExit): self.package_with_fake_sdk(root, b'new game content', failure)
                self.assertEqual({p.name: p.read_bytes() for p in output.iterdir()}, originals)

    def test_same_version_builds_preserve_each_companion_archive(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); names = []; archives = []
            for payload in (b'first game content', b'different game content'):
                apk = self.package_with_fake_sdk(root, payload)
                with zipfile.ZipFile(apk) as z:
                    name = z.read('assets/game/datos.txt').decode().split('\t')[0]
                names.append(name); archive = apk.with_name(name)
                with zipfile.ZipFile(archive) as z:
                    self.assertEqual(z.read('nxdatos.txt'), b'com.nxnative.t0100633007d48000\t101\n')
                    self.assertEqual(b''.join(z.read(n) for n in z.namelist() if n != 'nxdatos.txt'), payload)
                archives.append(archive.read_bytes())
            self.assertNotEqual(names[0], names[1])
            self.assertEqual(apk.with_name(names[0]).read_bytes(), archives[0])

    def test_deploy_assets_remain_readable_outside_adb_owner(self):
        # Run the deployment's shell steps on a real temporary tree. Android's
        # shell-owned 0770 directories must be traversable by the separate app UID.
        for remote in (False, True):
            with self.subTest(remote=remote), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp); ext = root / 'external'; ext.mkdir()
                source = root / 'source'; source.mkdir()
                for name in ('dm', 'romfs', 'vk'):
                    (source / name).mkdir(mode=0o700)
                    (source / name / 'asset').write_bytes(b'fixture')
                    (source / name / 'asset').chmod(0o600)
                sentinel = ext / 'env.txt'; sentinel.write_bytes(b'keep'); sentinel.chmod(0o600)
                device = '/sdcard/Android/data/com.nxnative.t0100633007d48000/files'
                def run(*args, **kwargs):
                    if args[3] == 'shell':
                        command = args[4].replace(device, str(ext))
                        if command.startswith('am force-stop '): return
                        if 'com.nxnative.AssetDeploy' in command: return # Java verifier/publication has its own native fixtures.
                        subprocess.run(['sh', '-c', 'umask 007; ' + command], check=True)
                    else:
                        self.assertEqual(args[3], 'push')
                        destination = args[-1].replace(device, str(ext))
                        for path in args[4:-1]: shutil.copy(path, destination)
                a = SimpleNamespace(sdk=None, id='0100633007D48000', test=False, serial='emulator-fixture',
                                    from_device=str(source) if remote else None, build='', patched=None, romfs=None, aoc=None)
                files = [(str(p.relative_to(source)), str(p)) for p in source.rglob('asset')]
                with patch.object(pack, 'run', side_effect=run), patch.object(pack, 'game_files', return_value=files), \
                     patch.object(pack, 'installed_apk', return_value='/data/app/fixture/base.apk'):
                    pack.deploy(a)
                for path in ext.rglob('*'):
                    if path == sentinel: continue
                    self.assertEqual(path.stat().st_mode & (0o5 if path.is_dir() else 0o4),
                                     0o5 if path.is_dir() else 0o4, str(path))
                self.assertEqual(sentinel.read_bytes(), b'keep')
                self.assertEqual(sentinel.stat().st_mode & 0o777, 0o600)

    def test_large_image_spills_to_bounded_data_chunks(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); src = root / 'RomFS.img'; src.write_bytes(b'abcde' * 1000000)
            archive = root / 'data.zip'
            with zipfile.ZipFile(io.BytesIO(), 'w') as apk:
                self.assertTrue(pack.needs_data_archive(apk, src, 1024))
            with zipfile.ZipFile(archive, 'w', allowZip64=True) as data:
                pack.write_data_file(data, 'romfs/romfs.img', src)
            with zipfile.ZipFile(archive) as data:
                self.assertGreater(len(data.infolist()), 1)
                self.assertTrue(all(i.file_size <= 2 << 20 for i in data.infolist()))
                self.assertEqual(b''.join(data.read(i) for i in data.infolist()), src.read_bytes())

    def test_apk_update_during_deploy_aborts_before_publication(self):
        a = SimpleNamespace(sdk=None, id='0100633007D48000', test=True, serial='emulator-fixture',
                            from_device=None, build='', patched=None, romfs=None, aoc=None)
        with patch.object(pack, 'game_files', return_value=[]), patch.object(pack, 'run') as run, \
             patch.object(pack, 'installed_apk', side_effect=['/data/app/old path/base.apk', '/data/app/new/base.apk']):
            with self.assertRaisesRegex(ValueError, 'APK changed'):
                pack.deploy(a)
        commands = [c.args[-1] for c in run.call_args_list]
        self.assertEqual(sum('com.nxnative.AssetDeploy' in c for c in commands), 1)
        self.assertIn('--check', commands[0])
        self.assertIn("CLASSPATH='/data/app/old path/base.apk'", commands[0])

    def test_selected_version_uses_android_modules_and_romfs_image(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            modules = root / 'patched_android_v42'; modules.mkdir()
            (modules / 'main.patched.bin').write_bytes(b'code')
            (modules / 'main.fixups.tsv').write_text('# TP_SLOT\t2\n')
            (root / 'shader/vk').mkdir(parents=True)
            image = root / 'extract/v42/RomFS.img'; image.parent.mkdir(parents=True)
            image.write_bytes(b'image')
            files = dict(pack.game_files(str(root), patched=str(modules), romfs=str(image)))
            self.assertEqual(Path(files['romfs/romfs.img']), image)
            self.assertEqual(Path(files['dm/main.patched.bin']), modules / 'main.patched.bin')
            self.assertNotIn('romfs/ExeFS/main', files)

    def test_deploy_keeps_test_assets_in_test_package(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'shader/vk').mkdir(parents=True)
            (root / 'extract/game/RomFS').mkdir(parents=True)
            (root / 'patched_android').mkdir()
            (root / 'patched_android/main.patched.bin').write_bytes(b'code')
            (root / 'patched_android/main.fixups.tsv').write_text('# TP_SLOT\t2\n')
            for test in (False, True):
                for remote in (False, True):
                    with self.subTest(test=test, remote=remote), patch.object(pack, 'run') as run, \
                         patch.object(pack, 'installed_apk', return_value='/data/app/fixture/base.apk'):
                        args = ['0100633007D48000', tmp, '--deploy', '--serial', 'emulator-5554']
                        if test:
                            args.append('--test')
                        if remote:
                            args.extend(['--from-device', '/sdcard/source'])
                        pack.main(args)
                        commands = '\n'.join(str(c.args) for c in run.call_args_list)
                        target = '/sdcard/Android/data/' + pack.package(args[0], test) + '/files'
                        self.assertIn(target, commands)
                        other = '/sdcard/Android/data/' + pack.package(args[0], not test) + '/files'
                        self.assertNotIn(other, commands)

    def test_image_assets_are_streamed_and_reconstruct_exactly(self):
        with tempfile.TemporaryDirectory() as tmp:
            image = Path(tmp) / 'RomFS.img'
            original = b'xy' * (3 << 20) + b'end'
            image.write_bytes(original)
            parts = list(pack.asset_pieces(image))
            self.assertGreater(len(parts), 1)
            self.assertTrue(all(len(p) <= 2 << 20 for p in parts))
            self.assertEqual(b''.join(parts), original)

    def test_piece_ranges_reconstruct_archive(self):
        # Two adjacent entries plus the archive table: every byte is covered once.
        data = struct.pack('<IIIQIIQII', 40, 8, 2, 1, 44, 48, 2, 48, 52) + b'AAAABBBB'
        pieces = pack.pieces(data)
        self.assertEqual(b''.join(data[o:o+n] for o, n in pieces), data)
        self.assertEqual(pieces, [(0, 44), (44, 4), (48, 4)])
        overlap = bytearray(data); struct.pack_into('<I', overlap, 36, 46)
        self.assertEqual(pack.pieces(overlap), [(0, len(overlap))])

    def test_prepared_layout_requires_android_rewrites(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for d in ('patched_android', 'shader/vk', 'extract/game/RomFS'):
                (root / d).mkdir(parents=True)
            (root / 'patched_android/main.patched.bin').write_bytes(b'synthetic')
            meta = root / 'patched_android/main.fixups.tsv'
            meta.write_text('# BUILD_ID\t0001020304050607\n')
            with self.assertRaisesRegex(ValueError, 'Android'):
                pack.game_files(str(root))
            meta.write_text(meta.read_text() + '# TP_SLOT\t2\n')
            (root / 'extract/game/RomFS/a').write_bytes(b'a')
            files = dict(pack.game_files(str(root)))
            self.assertIn('dm/main.patched.bin', files)
            self.assertIn('romfs/a', files)
            self.assertEqual(pack.package('0100633007D48000', True), 'com.nxnative.t0100633007d48000.test')


if __name__ == '__main__':
    unittest.main()
