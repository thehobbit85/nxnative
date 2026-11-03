"""An incompatible rewrite must be diagnosed before the loader opens the module image."""
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    abi, module_dir, *runner = sys.argv[1:]
    original = (Path(module_dir) / 'main.fixups.tsv').read_text()
    # A historical Apple header has no TP_SLOT. Android writes slot 2.
    with tempfile.TemporaryDirectory() as tmp:
        header = Path(tmp) / 'main.fixups.tsv'
        foreign = (''.join(line for line in original.splitlines(keepends=True) if not line.startswith('# TP_SLOT'))
                   if abi == 'android' else original + '# TP_SLOT\t2\n')
        header.write_text(foreign)
        run = subprocess.run([*runner, tmp], capture_output=True, text=True)
        expected = 'missing TP_SLOT' if abi == 'android' else 'incompatible thread-slot layout'
        if run.returncode == 0 or expected not in run.stderr:
            raise RuntimeError(f'expected {expected!r} before module mapping; got {run.returncode}: {run.stderr}')
    print(f'{abi} loader rejected the foreign rewrite before opening its image')


if __name__ == '__main__':
    main()
