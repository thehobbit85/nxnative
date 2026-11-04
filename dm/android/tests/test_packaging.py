"""Exercise the reused Android asset packer without SDK tools or game dumps."""
from pathlib import Path
import importlib.util
import struct
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('make_apk', Path(__file__).resolve().parents[1] / 'tools/make_apk.py')
assert spec is not None and spec.loader is not None
pack = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pack)


class Packaging(unittest.TestCase):
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
