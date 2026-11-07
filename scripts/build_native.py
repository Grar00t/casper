#!/usr/bin/env python3
"""Local C11 build using GCC/Clang; no Bash, MSVC or cloud runner."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
TARGETS = {
    'niyah': ['niyah_core.c', 'niyah_train_full.c', 'niyah_main.c'],
    'trainer': ['niyah_train.c', 'niyah_train_full.c', 'niyah_core.c', '../tokenizer.c'],
    'niyah_hybrid': ['niyah_hybrid_main.c', 'niyah_core.c', 'hybrid_reasoner.c', 'constraint_solver.c', 'rule_parser.c', 'rule_source_guard.c', 'proof_generator.c', 'khz_q_svd.c', 'casper_rag.c', '../src/niyah_gguf_map.c', '../tokenizer.c'],
    'casper': ['casper_cli.c', 'casper_rag.c', 'rule_parser.c', 'rule_source_guard.c', 'proof_generator.c', 'khz_q_svd.c'],
    'casper-chronicle': ['casper_chronicle_main.c', 'casper_chronicle.c', 'chronicle_pool.c', 'proof_generator.c'],
    'bench_niyah': ['bench_niyah.c', 'niyah_core.c'],
    'tokenizer_test': ['../tokenizer.c'],
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', default=os.environ.get('CC'))
    parser.add_argument('--debug', action='store_true')
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--smoke', action='store_true')
    args = parser.parse_args()
    compiler = args.compiler or next((x for x in ('gcc', 'clang') if shutil.which(x)), None)
    if not compiler or not shutil.which(compiler):
        parser.error('Install GCC or Clang; Windows requires a MinGW-compatible toolchain.')
    if Path(compiler).name.lower() in ('cl', 'cl.exe', 'clang-cl', 'clang-cl.exe'):
        parser.error('MSVC-compatible drivers are not supported; use gcc or clang.')
    macros = subprocess.check_output([compiler, '-dM', '-E', '-x', 'c', '-'], input='', text=True)
    windows = any(line.startswith('#define _WIN32 ') for line in macros.splitlines())
    if windows != (os.name == 'nt'):
        parser.error('Cross-compilation is not supported by this build-and-test entry point.')
    suffix = '.exe' if windows else ''
    output = ROOT / 'build'
    output.mkdir(parents=True, exist_ok=True)
    flags = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-Wstrict-prototypes', '-Wmissing-prototypes', '-Wcast-align', '-Wwrite-strings', '-Wshadow', '-pedantic', '-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'Core_CPP')]
    flags += ['-O0', '-g3', '-DDEBUG'] if args.debug else ['-O3', '-DNDEBUG']
    if args.sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    if windows:
        flags += ['-finput-charset=UTF-8', '-fexec-charset=UTF-8']
    for name, sources in TARGETS.items():
        extra = ['-DTOKENIZER_TEST'] if name == 'tokenizer_test' else []
        if windows and name == 'casper-chronicle':
            extra += ['-municode']
        links = ['-lm']
        if windows and 'casper_rag.c' in sources:
            links += ['-lwinhttp']
        binary = output / (name + suffix)
        paths = [str((ROOT / 'Core_CPP' / source).resolve()) for source in sources]
        subprocess.run([compiler, *flags, *extra, *paths, '-o', str(binary), *links], check=True, cwd=ROOT)
        print(hashlib.sha256(binary.read_bytes()).hexdigest(), binary.name, flush=True)
    subprocess.run([str(output / ('tokenizer_test' + suffix))], check=True, cwd=ROOT)
    if args.smoke:
        for name, arguments in [('niyah', []), ('niyah_hybrid', ['--smoke']), ('casper', ['--self-check']), ('casper-chronicle', ['--self-check'])]:
            subprocess.run([str(output / (name + suffix)), *arguments], check=True, cwd=ROOT)
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as error:
        raise SystemExit(error.returncode)
