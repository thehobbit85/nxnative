#!/usr/bin/env python3
"""Build and package the Android host using an already extracted/rewritten game folder.

Usage: build_android.py <title ID> <build folder> <output.apk> [--bundle] [packaging options]
Set ANDROID_NDK_HOME, ANDROID_SDK_ROOT and JAVA_HOME. See dm/android/README.md.
"""
from pathlib import Path
import argparse
import hashlib
import os
import platform
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
ALIASES = Path('/tmp') / f'nxnative-android-paths-{os.getuid()}'


def make_path(path):
    """GNU Make splits path lists on spaces; stable aliases preserve the native cache.

    Only the symlink lives in /tmp. Objects/codecs stay in the requested build
    folder, and a later invocation uses the same compiler and output paths.
    """
    if re.fullmatch(r'[A-Za-z0-9_./+-]+', str(path)): return path
    aliases = ALIASES
    aliases.mkdir(mode=0o700, exist_ok=True)
    if aliases.is_symlink() or aliases.stat().st_uid != os.getuid() or aliases.stat().st_mode & 0o077:
        raise ValueError('Android path alias directory must be private: ' + str(aliases))
    alias = aliases / hashlib.sha256(os.fsencode(path)).hexdigest()[:24]
    try: alias.symlink_to(path, target_is_directory=True)
    except FileExistsError:
        if not alias.is_symlink() or alias.readlink() != path:
            raise ValueError('Android path alias has a different target: ' + str(alias)) from None
    return alias


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('id'); p.add_argument('build', type=Path); p.add_argument('out', type=Path)
    p.add_argument('--ndk', default=os.environ.get('ANDROID_NDK_HOME'))
    p.add_argument('--build-root', type=Path)
    p.add_argument('--profile', help='build profile ID when it differs from the application title ID')
    p.add_argument('--jobs', type=int, default=4)
    p.add_argument('--check-tools', action='store_true', help='validate native and packaging tools without building')
    a, packaging = p.parse_known_args()
    if not a.ndk: p.error('set ANDROID_NDK_HOME or pass --ndk')
    if a.jobs < 1: p.error('--jobs must be positive')
    if not re.fullmatch(r'[0-9a-fA-F]{16}', a.id): p.error('id must be a 16-digit title ID')
    if a.profile and not re.fullmatch(r'[0-9a-fA-F]{16}', a.profile): p.error('profile must be a 16-digit title ID')
    ndk = Path(a.ndk).expanduser().resolve()
    if not ndk.is_dir(): p.error('Android NDK folder does not exist: ' + str(ndk))
    build = a.build.resolve(); output = (a.build_root or build / 'android-host').resolve()
    profile = (a.profile or a.id).upper()
    try:
        if not re.fullmatch(r'[A-Za-z0-9_./+-]+', str(ROOT)):
            raise ValueError('Android native build needs a repository path without spaces or shell characters: ' + str(ROOT))
        if sys.platform == 'darwin': ndk_host = 'darwin-x86_64'
        elif sys.platform.startswith('linux') and platform.machine().lower() in ('x86_64', 'amd64'): ndk_host = 'linux-x86_64'
        else: raise ValueError('Android native builds support macOS or x86-64 Linux build hosts')
        if not shutil.which('make'): raise ValueError('Android native build needs GNU Make on PATH')
        for tool in ('aarch64-linux-android30-clang', 'llvm-ar'):
            path = ndk / 'toolchains/llvm/prebuilt' / ndk_host / 'bin' / tool
            if not path.is_file() or not os.access(path, os.X_OK):
                raise ValueError('Android NDK is missing executable ' + str(path))
        package_cmd = [sys.executable, str(ROOT / 'dm/android/tools/make_apk.py'), a.id, str(build), str(a.out.resolve()),
                       '--library', str(output / profile / 'libdmhost.so'), *packaging]
        subprocess.run([*package_cmd, '--check-tools'], check=True)
        if a.check_tools: return
        output.mkdir(parents=True, exist_ok=True)
        subprocess.run(['make', '-C', str(ROOT / 'dm/android'), 'GAME=' + profile,
                        'NDK=' + str(make_path(ndk)), 'BUILD_ROOT=' + str(make_path(output)), '-j' + str(a.jobs)], check=True)
        subprocess.run(package_cmd, check=True)
    except subprocess.CalledProcessError as e:
        p.exit(e.returncode or 1, '[RED] Android build or packaging failed\n')
    except (OSError, ValueError) as e:
        p.exit(1, '[RED] ' + str(e) + '\n')


if __name__ == '__main__':
    main()
