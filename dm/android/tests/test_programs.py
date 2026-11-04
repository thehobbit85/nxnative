"""All declared prepared programs reach one APK's asset and reuse inventories."""
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest

from dm.android.tests.test_packaging import pack


class Programs(unittest.TestCase):
    def test_selected_assets_keep_program_code_and_resources_separate(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name, payload in [('base', b'base'), ('child', b'child')]:
                build = root / name
                (build / 'patched_android').mkdir(parents=True)
                (build / 'shader/vk').mkdir(parents=True)
                (build / 'patched_android/main.patched.bin').write_bytes(payload)
                (build / 'patched_android/main.fixups.tsv').write_text('# TP_SLOT\t2\n')
                (build / 'RomFS.img').write_bytes(payload + b' resource')
            catalog = root / 'programs.json'
            catalog.write_text(json.dumps({'schema': 1, 'programs': [{'index': 1, 'profile': '0100633007D48001', 'build': 'child', 'romfs': 'child/RomFS.img', 'gfx': 'nvn'}]}))
            a = SimpleNamespace(id='0100633007D48000', profile=None, programs=str(catalog), build=str(root / 'base'),
                                patched=None, romfs=str(root / 'base/RomFS.img'), aoc=None, save=None, icon=None, env_file=None, cheats=None)
            files, _, _, _, _, sources = pack.selected_inputs(a, 'lib.so', 'key')
            inventory = {name: Path(path).read_bytes() for name, path in files}
            self.assertEqual(inventory.get('dm/main.patched.bin'), b'base')
            self.assertEqual(inventory.get('programs/1/dm/main.patched.bin'), b'child')
            self.assertEqual(inventory.get('programs/1/romfs/romfs.img'), b'child resource')
            self.assertIn(str(catalog), [str(path) for _, path in sources])


if __name__ == '__main__': unittest.main()
