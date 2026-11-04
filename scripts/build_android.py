#!/usr/bin/env python3
"""Build and package the Android host using an already extracted/rewritten game folder.

Usage: build_android.py <title ID> <build folder> <output.apk> [--bundle] [packaging options]
Set ANDROID_NDK_HOME, ANDROID_SDK_ROOT and JAVA_HOME. See dm/android/README.md.
"""
from pathlib import Path
import argparse
import os
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('id'); p.add_argument('build', type=Path); p.add_argument('out', type=Path)
    p.add_argument('--ndk', default=os.environ.get('ANDROID_NDK_HOME'))
    p.add_argument('--build-root', type=Path)
    p.add_argument('--jobs', type=int, default=4)
    a, packaging = p.parse_known_args()
    if not a.ndk: p.error('set ANDROID_NDK_HOME or pass --ndk')
    if a.jobs < 1: p.error('--jobs must be positive')
    build = a.build.resolve(); output = (a.build_root or build / 'android-host').resolve()
    try:
        subprocess.run(['make', '-C', str(ROOT / 'dm/android'), 'GAME=' + a.id,
                        'NDK=' + str(Path(a.ndk).resolve()), 'BUILD_ROOT=' + str(output), '-j' + str(a.jobs)], check=True)
        subprocess.run([sys.executable, str(ROOT / 'dm/android/tools/make_apk.py'), a.id, str(build), str(a.out.resolve()),
                        '--library', str(output / a.id / 'libdmhost.so'), *packaging], check=True)
    except subprocess.CalledProcessError as e:
        p.exit(e.returncode or 1, '[RED] Android build or packaging failed\n')


if __name__ == '__main__':
    main()
