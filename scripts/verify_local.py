#!/usr/bin/env python3
"""Local verification only; never installs dependencies or dispatches Actions."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', default=os.environ.get('CC', 'gcc'))
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--node', action='store_true')
    parser.add_argument('--chronicle', action='store_true')
    args = parser.parse_args()
    output = ROOT / 'build/local-verification'
    output.mkdir(parents=True, exist_ok=True)
    results = []

    def run(name, command, cwd=ROOT):
        try:
            completed = subprocess.run(command, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                       text=True, encoding='utf-8', errors='replace', timeout=300)
            code, text = completed.returncode, completed.stdout
        except (OSError, subprocess.TimeoutExpired) as error:
            code, text = 1, str(error)
        log = output / (name + '.log')
        log.write_text(text.replace(str(ROOT), '<WORKTREE>'), encoding='utf-8')
        results.append({'check': name, 'exit_code': code, 'log': log.name,
                        'sha256': hashlib.sha256(log.read_bytes()).hexdigest()})
        print(name + ': ' + ('PASS' if code == 0 else 'FAIL'), flush=True)
        return code == 0

    build = [sys.executable, 'scripts/build_native.py', '--compiler', args.compiler, '--smoke']
    if args.sanitize:
        build += ['--debug', '--sanitize']
    native_ok = run('native-build', build)
    suffix = '.exe' if os.name == 'nt' else ''
    if native_ok:
        run('native-bridge', [sys.executable, 'tests/test_local_bridge.py', 'build/casper-chronicle' + suffix])
        sampler = ROOT / 'build' / ('niyah_sampler_test' + suffix)
        command = [args.compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-ICore_CPP',
                   'Core_CPP/niyah_core.c', 'tests/test_niyah_sampler.c', '-lm', '-o', str(sampler)]
        if args.sanitize:
            command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        if run('sampler-build', command):
            run('sampler-test', [str(sampler)])
    run('http-contracts-fake-bridge', [sys.executable, 'tests/test_local_server.py', '-v'])
    run('training-contracts', [sys.executable, 'tests/test_training_contracts.py', '-v'])
    if args.chronicle:
        command = [sys.executable, 'scripts/test_chronicle.py', '--compiler', args.compiler]
        if os.name == 'nt':
            command.append('--native-windows')
        run('chronicle-acceptance', command)
    if args.node:
        npm = shutil.which('npm') or 'npm'
        run('node-tests', [npm, 'test'], ROOT / 'niyah_engine_local')
        run('node-audit', [npm, 'audit', '--omit=dev', '--audit-level=moderate'], ROOT / 'niyah_engine_local')
    ok = all(row['exit_code'] == 0 for row in results)
    report = {'format': 'CASPER-LOCAL-VERIFICATION-V1', 'passed': ok,
              'tested_at_utc': datetime.now(timezone.utc).isoformat(), 'checks': results,
              'limitations': ['HTTP contracts use an explicit fake bridge; native bridge is tested separately.',
                  'Training numeric test skips when PyTorch is absent; inspect training-contracts.log.',
                  'No CUDA, visual browser, WPF, or cross-platform certification is provided.'],
              'cloud_runner_dispatched': False}
    (output / 'summary.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    return 0 if ok else 1


if __name__ == '__main__':
    raise SystemExit(main())
