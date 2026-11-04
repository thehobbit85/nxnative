"""Run the wrapper through GNU Make with game/NDK paths that contain spaces."""
from pathlib import Path
import importlib.util
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('android_build_wrapper', Path(__file__).resolve().parents[3] / 'scripts/build_android.py')
assert spec and spec.loader
wrapper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(wrapper)
TITLE = '0100633007D48000'


class NativePaths(unittest.TestCase):
    @unittest.skipUnless(shutil.which('make'), 'GNU Make is required')
    def test_requested_build_paths_survive_make_and_reuse(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); repo = root / 'repo'; android = repo / 'dm/android'; android.mkdir(parents=True)
            tools = android / 'tools'; tools.mkdir()
            (tools / 'make_apk.py').write_text('from pathlib import Path\nimport sys\nPath(sys.argv[3]).write_bytes(Path(sys.argv[sys.argv.index("--library") + 1]).read_bytes())\n')
            (android / 'source.txt').write_bytes(b'native build fixture')
            # Deliberately mirrors the native Makefile's path-list constraints;
            # execute the real build tool, not a mock of its argv.
            (android / 'Makefile').write_text('OUT := $(BUILD_ROOT)/$(GAME)\n$(OUT)/libdmhost.so: source.txt\n\t@mkdir -p $(OUT)\n\t$(NDK)/compiler $< $@\n')
            ndk = root / 'NDK with spaces'; ndk.mkdir()
            compiler = ndk / 'compiler'; compiler.write_text('#!/bin/sh\ncp "$1" "$2"\n'); compiler.chmod(0o755)
            for name in ('Build with spaces', 'Build $literal; characters'):
                build = root / name; build.mkdir(); apk = build / 'Game.apk'
                args = ['build_android.py', TITLE, str(build), str(apk), '--ndk', str(ndk)]
                with patch.object(wrapper, 'ROOT', repo), patch.object(wrapper, 'ALIASES', root / 'aliases'), patch.object(sys, 'argv', args):
                    wrapper.main()
                    library = build / 'android-host' / TITLE / 'libdmhost.so'
                    self.assertEqual(apk.read_bytes(), b'native build fixture')
                    first = library.stat().st_mtime_ns
                    wrapper.main()
                    self.assertEqual(library.stat().st_mtime_ns, first, 'unchanged native build was rebuilt')

    def test_invalid_title_or_profile_fails_before_make(self):
        for options in (['bad'], [TITLE, '--profile', 'bad']):
            with self.subTest(options=options), tempfile.TemporaryDirectory() as tmp:
                args = ['build_android.py', options[0], tmp, tmp + '/Game.apk', '--ndk', tmp, *options[1:]]
                with patch.object(sys, 'argv', args), patch.object(wrapper.subprocess, 'run') as run:
                    with self.assertRaises(SystemExit): wrapper.main()
                    run.assert_not_called()

    def test_missing_ndk_fails_before_make(self):
        with tempfile.TemporaryDirectory() as tmp:
            args = ['build_android.py', TITLE, tmp, tmp + '/Game.apk', '--ndk', tmp + '/missing']
            with patch.object(sys, 'argv', args), patch.object(wrapper.subprocess, 'run') as run:
                with self.assertRaises(SystemExit): wrapper.main()
                run.assert_not_called()


if __name__ == '__main__': unittest.main()
