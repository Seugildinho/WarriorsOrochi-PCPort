"""Run bounded, windowless tests against this prototype's actual runtime."""
from pathlib import Path
import hashlib
import json
import subprocess

PORT = Path(__file__).resolve().parents[1]

def main():
    runner = PORT/'build-tools/ps2xRuntime/Release/WarriorsOrochi.exe'
    results = []
    for test in ('--test-dma', '--test-vsync', '--test-gif', '--test-display', '--test-pad', '--test-overlay', '--test-switch', '--test-sif', '--test-iop-memory', '--test-iop-loader', '--test-syncv', '--test-mpeg-audio', '--test-mpeg-transport', '--test-gpu', '--test-gs-thread'):
        try:
            result = subprocess.run([str(runner), test], cwd=PORT/'local',
                capture_output=True, text=True, encoding='utf8', errors='replace',
                # The GPU test compares ~750 full VRAM snapshots (randomized batches).
                timeout=30 if test == '--test-gpu' else 10, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
            results.append(dict(test=test, exit_code=result.returncode,
                stdout=result.stdout, stderr=result.stderr))
        except subprocess.TimeoutExpired:
            results.append(dict(test=test, exit_code=None, error='test_timeout'))
    # 2026-10-23: in-process ipu2 decoder must stay byte-identical to the former
    # ffmpeg.exe output. The sample is game-derived and lives only in local/.
    checker = PORT/'build-tools/ps2xRuntime/Release/orochi_ipu_check.exe'
    sample = PORT/'local/ipu-check/sample.ipu'
    reference = PORT/'local/ipu-check/sample.ffmpeg-cli.rgba'
    if checker.exists() and sample.exists() and reference.exists():
        try:
            result = subprocess.run([str(checker), str(sample), str(reference), '50'], cwd=PORT/'local',
                capture_output=True, text=True, encoding='utf8', errors='replace',
                timeout=30, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
            results.append(dict(test='orochi_ipu_check', exit_code=result.returncode,
                stdout=result.stdout, stderr=result.stderr))
        except subprocess.TimeoutExpired:
            results.append(dict(test='orochi_ipu_check', exit_code=None, error='test_timeout'))
    else:
        results.append(dict(test='orochi_ipu_check', exit_code=None, skipped=True,
            error='missing checker or local/ipu-check sample'))
    with runner.open('rb') as source:
        digest = hashlib.file_digest(source, 'sha256').hexdigest()
    report = dict(executable_sha256=digest, tests=results,
                  passed=all(item['exit_code'] == 0 for item in results if not item.get('skipped')))
    (PORT/'relatorios/regressoes.json').write_text(json.dumps(report, indent=2), encoding='utf8')
    print(json.dumps(report, indent=2))
    return 0 if report['passed'] else 1

if __name__ == '__main__':
    raise SystemExit(main())
