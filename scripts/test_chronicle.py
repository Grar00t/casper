#!/usr/bin/env python3
"""Reproducible C11, sanitizer, allocation, acceptance and benchmark gates."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CORE = ROOT / "Core_CPP"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", choices=("gcc", "clang"), default="gcc")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--native-windows", action="store_true",
                        help="Run native MinGW O2/O0 gates; sanitizers and heap wrapping run on Linux")
    args = parser.parse_args()
    if args.native_windows and os.name != "nt":
        parser.error("--native-windows requires native Windows Python")
    if os.name == "nt" and not args.native_windows:
        parser.error("use --native-windows with a MinGW GCC/Clang compiler on Windows")
    output = (args.output or ROOT / "build" / f"chronicle-{args.compiler}").resolve()
    output.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, PYTHONIOENCODING="utf-8", ASAN_OPTIONS="detect_leaks=1:halt_on_error=1",
               UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    rows = []

    def run(label, command, cwd=ROOT):
        result = subprocess.run([str(x) for x in command], cwd=cwd, env=env,
                                text=True, encoding="utf-8", stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        normalized = result.stdout.replace(str(ROOT), "<WORKTREE>")
        log = output / f"{label}.txt"
        log.write_text(normalized, encoding="utf-8")
        rows.append({"check": label, "exit_code": result.returncode,
                     "log": log.name, "sha256": hashlib.sha256(log.read_bytes()).hexdigest()})
        print(f"{label}: exit={result.returncode}", flush=True)
        if result.returncode or re.search(r"AddressSanitizer|runtime error:|LeakSanitizer", result.stdout):
            print(normalized, flush=True)
            raise RuntimeError(f"gate failed: {label}")
        return result.stdout

    cc_version = run("compiler-version", [args.compiler, "--version"]).splitlines()[0]
    flags = ["-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Wshadow", "-Wconversion", "-Werror",
             "-Wstrict-prototypes", "-Wmissing-prototypes", f"-I{CORE}"]
    common = [CORE / "casper_chronicle.c", CORE / "chronicle_pool.c", CORE / "proof_generator.c"]
    hashes = {}
    modes = (("o2", ["-O2"]), ("o0", ["-O0", "-g"])) if args.native_windows else (
        ("o2", ["-O2"]),
        ("san", ["-O0", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]))
    suffix = ".exe" if args.native_windows else ""
    link = ["-municode"] if args.native_windows else []
    try:
        for mode, extra in modes:
            binary = output / f"chronicle-{mode}{suffix}"
            run(f"build-{mode}", [args.compiler, *flags, *extra,
                                    CORE / "casper_chronicle_main.c", *common, *link, "-o", binary])
            for name in (("pool",) if args.native_windows else ("pool", "api", "stream")):
                test = output / f"test-{name}-{mode}{suffix}"
                sources = [CORE / "chronicle_pool.c"] if name == "pool" else common
                wrap = [] if name == "pool" else ["-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free"]
                if name == "stream":
                    wrap = ["-Wl,--wrap=fread"]
                run(f"build-{name}-{mode}", [args.compiler, *flags, *extra,
                    ROOT / "tests" / f"test_chronicle_{name}.c", *sources, *wrap, "-o", test])
                with tempfile.TemporaryDirectory(prefix="chronicle-test-") as scratch:
                    run(f"{name}-{mode}", [test], Path(scratch))
            with tempfile.TemporaryDirectory(prefix="chronicle-self-") as scratch:
                run(f"self-check-{mode}", [binary, "--self-check"], Path(scratch))
            suites = ("integrity", "reasoner", "literal", "preservation", "resources", "arabic", "compatibility", "questions")
            if args.native_windows:
                suites += ("windows",)
            for suite in suites:
                repetitions = 3 if suite in ("reasoner", "literal", "arabic", "questions") else 1
                for repeat in range(repetitions):
                    text = run(f"{suite}-{mode}-{repeat + 1}",
                        [sys.executable, ROOT / "tests" / f"test_chronicle_{suite}.py", binary])
                    found = re.search(r"result_sha256=([0-9a-f]{64})", text)
                    if found:
                        previous = hashes.setdefault(suite, found.group(1))
                        if previous != found.group(1):
                            raise RuntimeError(f"nondeterministic {suite}: {mode}")
            run(f"local-bridge-{mode}",
                [sys.executable, ROOT / "tests" / "test_local_bridge.py", binary])
            if mode == "o2":
                with tempfile.TemporaryDirectory(prefix="chronicle-bench-") as scratch:
                    command = [binary, "--benchmark"]
                    rss = output / "benchmark-rss.txt"
                    if Path("/usr/bin/time").exists():
                        command = ["/usr/bin/time", "-f", "max_rss_kib=%M", "-o", rss, *command]
                    run("benchmark", command, Path(scratch))
        summary = {"status": "PASS", "compiler": cc_version,
                   "platform": sys.platform, "native_windows": args.native_windows,
                   "sanitizers_executed": not args.native_windows,
                   "heap_wrapping_executed": not args.native_windows,
                   "deterministic_result_hashes": hashes,
                   "checks": rows}
    except (RuntimeError, OSError) as error:
        summary = {"status": "FAIL", "compiler": cc_version, "error": str(error), "checks": rows}
    summary_path = output / "summary.json"
    summary_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(f"chronicle_gate={summary['status']} completed_commands={len(rows)} summary={summary_path}")
    return 0 if summary["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
