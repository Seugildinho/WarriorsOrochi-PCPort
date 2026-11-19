"""Replay the complete patch chain; never overwrite an unrecognised local edit."""
from pathlib import Path, PurePosixPath
import hashlib
import json
import re
import subprocess
import tempfile


def normalized(data):
    return data.replace(b'\r\n', b'\n') if data is not None else None


def contents(path):
    """A file's bytes; None for one a later patch has yet to create."""
    return path.read_bytes() if path.exists() else None


def synchronize(port, vendor, revision, apply=False):
    patches = sorted((port/'patches').glob('*.patch'))
    paths = sorted({name for patch in patches for name in
                    re.findall(r'^\+\+\+ b/(.+)$', patch.read_text(), re.M)})
    for name in paths:
        path = PurePosixPath(name)
        if path.is_absolute() or '..' in path.parts:
            raise RuntimeError(f'Unsafe patch path: {name}')
    known = {name: set() for name in paths}
    (port/'local').mkdir(exist_ok=True)
    # Linha original desativada: a pasta temporaria ficava em port_pc/local.
    # Desde que port_pc virou um repositorio git, "git apply" rodando la dentro
    # usa caminhos relativos a RAIZ do repositorio (nao a pasta temporaria):
    # nada era aplicado, arquivos novos de patch (ex.: iop_spu2.cpp do 0041)
    # nao eram criados e a verificacao falhava com FileNotFoundError.
    # A pasta temporaria do sistema (ex.: C:\Users\<voce>\AppData\Local\Temp)
    # fica fora do repositorio, entao "git apply" volta a usar a pasta atual.
    # with tempfile.TemporaryDirectory(prefix='patch-replay-', dir=port/'local') as directory:
    with tempfile.TemporaryDirectory(prefix='orochi-patch-replay-') as directory:
        stage = Path(directory)
        for name in paths:
            source = subprocess.run(['git', '-C', str(vendor), 'show', f'{revision}:{name}'],
                                    capture_output=True)
            target = stage/name
            target.parent.mkdir(parents=True, exist_ok=True)
            if source.returncode == 0:
                target.write_bytes(normalized(source.stdout))
        def remember():
            for name in paths:
                known[name].add(normalized(contents(stage/name)))
        remember()
        for patch in patches:
            subprocess.run(['git', '-C', str(stage), '-c', 'core.autocrlf=false',
                            'apply', '--whitespace=nowarn', str(patch)], check=True,
                           capture_output=True)
            remember()
        expected = {name: (stage/name).read_bytes() for name in paths}
        actual = {name: normalized(contents(vendor/name)) for name in paths}
        unknown = [name for name in paths if actual[name] not in known[name]]
        if unknown:
            raise RuntimeError('Preserving unrecognised local edits: '+', '.join(unknown))
        if apply:
            for name in paths:
                if actual[name] != expected[name]:
                    (vendor/name).write_bytes(expected[name])
                    actual[name] = expected[name]
        matches = {name: actual[name] == expected[name] for name in paths}
    report = dict(upstream_commit=revision,
                  patches={p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in patches},
                  files_match_working_tree=matches, passed=all(matches.values()))
    (port/'relatorios/patches.json').write_text(json.dumps(report, indent=2), encoding='utf8')
    return report


if __name__ == '__main__':
    root = Path(__file__).resolve().parents[1]
    revision = json.loads((root/'config/toolchain.json').read_text())['ps2recomp_commit']
    report = synchronize(root, root/'vendor/PS2Recomp', revision)
    print(json.dumps(report, indent=2))
    raise SystemExit(0 if report['passed'] else 1)
