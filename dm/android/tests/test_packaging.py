"""Exercise the reused Android asset packer without SDK tools or game dumps."""
from pathlib import Path
import importlib.util
import struct
import io
import zipfile
import tempfile
import shutil
import subprocess
from types import SimpleNamespace
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('make_apk', Path(__file__).resolve().parents[1] / 'tools/make_apk.py')
assert spec is not None and spec.loader is not None
pack = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pack)


class Packaging(unittest.TestCase):
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
                        subprocess.run(['sh', '-c', 'umask 007; ' + command], check=True)
                    else:
                        self.assertEqual(args[3], 'push')
                        destination = args[-1].replace(device, str(ext))
                        for path in args[4:-1]: shutil.copy(path, destination)
                a = SimpleNamespace(sdk=None, id='0100633007D48000', test=False, serial='emulator-fixture',
                                    from_device=str(source) if remote else None, build='', patched=None, romfs=None, aoc=None)
                files = [(str(p.relative_to(source)), str(p)) for p in source.rglob('asset')]
                with patch.object(pack, 'run', side_effect=run), patch.object(pack, 'game_files', return_value=files):
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
                    with self.subTest(test=test, remote=remote), patch.object(pack, 'run') as run:
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
