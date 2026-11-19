"""Write the next vendor patch from the current working-tree edits.

usage: python tools/make_patch.py <NNNN-name> <vendor-relative path> [...]

The base of each file is the pinned PS2Recomp revision with every existing
patch replayed in order, so the new patch applies cleanly at the end of the
chain that tools/runtime_patches.py verifies.
"""
from pathlib import Path
import json
import subprocess
import sys
import tempfile

from runtime_patches import normalized, synchronize

PORT = Path(__file__).resolve().parents[1]
VENDOR = PORT / 'vendor/PS2Recomp'


def main():
    name, files = sys.argv[1], sys.argv[2:]
    target = PORT / 'patches' / f'{name}.patch'
    if target.exists():
        raise SystemExit(f'{target.name} already exists')
    revision = json.loads((PORT / 'config/toolchain.json').read_text())['ps2recomp_commit']
    patches = sorted((PORT / 'patches').glob('*.patch'))
    # Linha original desativada pelo mesmo motivo de tools/runtime_patches.py:
    # dentro do repositorio git, "git apply" ignorava a pasta temporaria e o
    # patch novo saia comparado com o PS2Recomp original, sem os patches
    # anteriores. A pasta temporaria do sistema fica fora do repositorio.
    # with tempfile.TemporaryDirectory(prefix='make-patch-', dir=PORT / 'local') as directory:
    with tempfile.TemporaryDirectory(prefix='orochi-make-patch-') as directory:
        stage = Path(directory)
        for side in ('a', 'b'):
            for name_ in files:
                (stage / side / name_).parent.mkdir(parents=True, exist_ok=True)
        for name_ in files:
            source = subprocess.run(['git', '-C', str(VENDOR), 'show', f'{revision}:{name_}'],
                                    capture_output=True)
            if source.returncode == 0:
                (stage / 'a' / name_).write_bytes(normalized(source.stdout))
            (stage / 'b' / name_).write_bytes(normalized((VENDOR / name_).read_bytes()))
        for patch in patches:
            touched = [f for f in files if f'+++ b/{f}' in patch.read_text(encoding='utf8')]
            if touched:
                subprocess.run(['git', '-C', str(stage / 'a'), '-c', 'core.autocrlf=false', 'apply',
                                '--whitespace=nowarn'] + [f'--include={f}' for f in touched] + [str(patch)],
                               check=True, capture_output=True)
        diff = b''
        for name_ in files:
            base = f'a/{name_}' if (stage / 'a' / name_).exists() else '/dev/null'
            result = subprocess.run(['git', '-c', 'core.autocrlf=false', 'diff', '--no-index', '--no-prefix',
                                     base, f'b/{name_}'], cwd=stage, capture_output=True)
            if result.returncode not in (0, 1):
                raise SystemExit(result.stderr.decode())
            diff += result.stdout
    if not diff:
        raise SystemExit('no changes')
    target.write_bytes(diff)
    report = synchronize(PORT, VENDOR, revision)
    print(target.name, 'written; chain verified' if report['passed'] else 'written; CHAIN MISMATCH')
    raise SystemExit(0 if report['passed'] else 1)


if __name__ == '__main__':
    main()
