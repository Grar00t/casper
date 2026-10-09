#!/usr/bin/env python3
"""Native Windows Unicode path, UTF-8 output, and evidence-byte oracle."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile


def invoke(binary, root, *arguments, success=True):
    result = subprocess.run([binary, *map(str, arguments)], cwd=root,
                            capture_output=True, timeout=30)
    output = result.stdout.decode("utf-8", errors="strict")
    error = result.stderr.decode("utf-8", errors="strict")
    expected = (0,) if success else (1, 2)
    if result.returncode not in expected:
        raise ValueError(f"exit={result.returncode}: {error.strip()}")
    if any(marker in error for marker in ("AddressSanitizer", "runtime error:")):
        raise ValueError(error.strip())
    return output


def value(output, key):
    return next(line[len(key) + 1:] for line in output.splitlines()
                if line.startswith(key + "="))


def receipt_check(binary, root, receipt, store, data, output=None):
    text = Path(receipt).read_bytes().decode("ascii")
    fields = dict(line.split(":", 1) for line in text.splitlines()[1:])
    if bytes.fromhex(fields["store_path_hex"]).decode("utf-8") != str(store):
        raise ValueError("receipt path changed")
    for key, original in (("store_sha256", store.read_bytes()),
                          ("document_sha256", data)):
        if fields[key] != hashlib.sha256(original).hexdigest():
            raise ValueError(f"independent {key} differs")
    if output is not None and fields["result_sha256"] != hashlib.sha256(output).hexdigest():
        raise ValueError("independent result hash differs")
    if invoke(binary, root, "verify", receipt).strip() != "VALID":
        raise ValueError("receipt rejected")


def path_case(binary, root, directory, filename, digest):
    folder = root / directory
    folder.mkdir()
    source = folder / filename
    data = ("أحمد يملك كتاب.\r\n"
            "@chronicle\tأحمد\tBORROWED_FROM\tخالد\t100\tSAR\tT1\tASSERTED\tPOSITIVE\tCONFIRMED\r\n"
            "@chronicle\tأحمد\tPAID_TO\tخالد\t100\tSAR\tT2\tASSERTED\tPOSITIVE\tCONFIRMED\r\n").encode("utf-8")
    source.write_bytes(data)
    ingest = invoke(binary, folder, "ingest", source)
    store = Path(value(ingest, "store"))
    if store != Path(str(source) + ".chronicle"):
        raise ValueError("stdout path changed")
    receipt_check(binary, folder, value(ingest, "receipt"), store, data)
    snapshot = store.read_bytes()
    if invoke(binary, folder, "ingest", source) != ingest or store.read_bytes() != snapshot:
        raise ValueError("repeat import changed store or receipt")
    requests = [("claim", ("أحمد", "يملك", "كتاب"), "EXACT_STATED"),
                ("find", ("أحمد",), "MATCHES"),
                ("query", ("أحمد خالد",), "SUPPORTED")]
    for command, arguments, status in requests:
        output = invoke(binary, folder, command, store, *arguments)
        first = output.splitlines()[0].encode("utf-8")
        result = json.loads(first)
        if result["status"] != status:
            raise ValueError(f"{command}: expected {status}, got {result['status']}")
        evidence = result.get("evidence", result.get("matches", []))
        if not evidence:
            raise ValueError(f"{command}: missing evidence")
        for item in evidence:
            span = data[item["byte_start"]:item["byte_end"]]
            if item["text"].encode("utf-8") != span:
                raise ValueError("source bytes changed")
            if item["exact_span_sha256"] != hashlib.sha256(span).hexdigest():
                raise ValueError("independent span hash differs")
        receipt = value(output, "receipt")
        receipt_check(binary, folder, receipt, store, data, first)
        damaged = folder / (command + "-تالف.receipt")
        damaged.write_bytes(Path(receipt).read_bytes() + b"unknown:tampered\n")
        invoke(binary, folder, "verify", damaged, success=False)
        digest.update(first)
    if source.read_bytes() != data:
        raise ValueError("source file changed")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    binary = str(parser.parse_args().binary.resolve())
    if os.name != "nt":
        print("windows_path_cases=0 skipped=requires_native_windows")
        return 0
    cases = [("ascii", "source.txt"), ("مجلد عربي", "source.txt"),
             ("arabic_file", "سجل الأحداث.txt"),
             ("دليل مختلط 🧪", "قصة أحمد 🧾.txt")]
    failures = 0
    digest = hashlib.sha256()
    with tempfile.TemporaryDirectory(prefix="chronicle-winpaths-") as directory:
        for folder, filename in cases:
            try:
                path_case(binary, Path(directory), folder, filename, digest)
                print(f"PASS path_case_{cases.index((folder, filename)) + 1}")
            except (ValueError, OSError, KeyError, StopIteration, subprocess.TimeoutExpired) as error:
                failures += 1
                print(f"FAIL path_case_{cases.index((folder, filename)) + 1}: {error}")
    print(f"windows_path_cases={len(cases)} failures={failures} result_sha256={digest.hexdigest()}")
    return int(failures != 0)


if __name__ == "__main__":
    raise SystemExit(main())
