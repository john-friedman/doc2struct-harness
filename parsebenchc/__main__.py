"""Optional convenience launcher; all scoring happens in the C executable."""
from pathlib import Path
import subprocess
import sys


def main():
    executable=Path(__file__).resolve().parent/'build/standalone'/('parsebenchc.exe' if sys.platform=='win32' else 'parsebenchc')
    if not executable.exists():
        raise SystemExit('Build first: python parsebenchc/build.py')
    return subprocess.call([str(executable),*sys.argv[1:]])


if __name__=='__main__':
    raise SystemExit(main())
