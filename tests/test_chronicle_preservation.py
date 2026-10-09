#!/usr/bin/env python3
"""Existing store/receipt files must survive collisions byte-for-byte."""
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    binary = str(Path(sys.argv[1]).resolve())
    checks = 0
    failures = 0
    story = b"@chronicle\tAlice\tBORROWED_FROM\tBob\t100\tSAR\tT1\tASSERTED\tPOSITIVE\tCONFIRMED\n"
    sentinel = b"user-owned file: preserve exactly\x00\xff\n"
    with tempfile.TemporaryDirectory(prefix="chronicle-preservation-") as directory:
        root = Path(directory)

        def run(*args, cwd=root):
            result = subprocess.run([binary, *map(str, args)], cwd=cwd, capture_output=True, text=True, encoding="utf-8")
            diagnostics = ("AddressSanitizer", "UndefinedBehaviorSanitizer", "LeakSanitizer", "runtime error:")
            if result.returncode not in (0, 1, 2) or any(x in result.stderr for x in diagnostics):
                raise RuntimeError(f"abnormal CLI exit {result.returncode}: {args!r}\n{result.stderr[:4000]}")
            return result

        def check(label, condition):
            nonlocal checks, failures
            checks += 1
            if not condition:
                failures += 1
                print("FAIL " + label)

        source = root / "receipt-collision.txt"
        source.write_bytes(story)
        receipt = Path(str(source) + ".chronicle.receipt")
        receipt.write_bytes(sentinel)
        check("receipt collision rejected", run("ingest", source).returncode != 0)
        check("receipt sentinel preserved", receipt.read_bytes() == sentinel)
        check("source preserved on receipt collision", source.read_bytes() == story)

        source = root / "store-collision.txt"
        source.write_bytes(story)
        store = Path(str(source) + ".chronicle")
        receipt = Path(str(store) + ".receipt")
        store.write_bytes(sentinel)
        receipt.write_bytes(sentinel)
        check("store collision rejected", run("ingest", source).returncode != 0)
        check("store sentinel preserved", store.read_bytes() == sentinel)
        check("sibling receipt preserved", receipt.read_bytes() == sentinel)

        source = root / "repeat.txt"
        source.write_bytes(story)
        store = Path(str(source) + ".chronicle")
        receipt = Path(str(store) + ".receipt")
        check("initial ingestion", run("ingest", source).returncode == 0)
        original_store, original_receipt = store.read_bytes(), receipt.read_bytes()
        check("identical repeated ingestion", run("ingest", source).returncode == 0)
        check("identical store unchanged", store.read_bytes() == original_store)
        check("identical receipt unchanged", receipt.read_bytes() == original_receipt)
        check("identical receipt valid", run("verify", receipt).returncode == 0)
        question = "Alice Bob"
        query_receipt = Path(str(store) + ".query-" + hashlib.sha256(question.encode()).hexdigest()[:16] + ".receipt")
        check("initial query", run("query", store, question).returncode == 0)
        original_query_receipt = query_receipt.read_bytes()
        check("identical repeated query", run("query", store, question).returncode == 0)
        check("identical query receipt unchanged", query_receipt.read_bytes() == original_query_receipt)
        query_receipt.write_bytes(sentinel)
        check("query receipt collision rejected", run("query", store, question).returncode != 0)
        check("query receipt sentinel preserved", query_receipt.read_bytes() == sentinel)
        query_receipt.write_bytes(original_query_receipt)
        source.write_bytes(story + b"new content\n")
        check("changed source rejected", run("ingest", source).returncode != 0)
        check("changed source preserves existing store", store.read_bytes() == original_store)
        check("changed source preserves existing receipt", receipt.read_bytes() == original_receipt)

        # Simulate an external replacement with another valid store, so receipt
        # preservation is checked independently of store creation policy.
        other = root / "other.txt"
        other.write_bytes(story + b"new content\n")
        check("second independent ingestion", run("ingest", other).returncode == 0)
        store.write_bytes(Path(str(other) + ".chronicle").read_bytes())
        check("different valid query receipt refused", run("query", store, question).returncode != 0)
        check("prior valid query receipt preserved", query_receipt.read_bytes() == original_query_receipt)
        check("different valid ingest receipt refused", run("ingest", source).returncode != 0)
        check("prior valid ingest receipt preserved", receipt.read_bytes() == original_receipt)
        query_suffix = ".query-" + hashlib.sha256("هل سدد أحمد دين خالد؟".encode()).hexdigest()[:16] + ".receipt"
        for command, relative in [
            ("--self-check", "chronicle-self-test.txt"),
            ("--self-check", "chronicle-conflict-test.txt"),
            ("--self-check", "chronicle-self-test.txt.chronicle" + query_suffix),
            ("--benchmark", "chronicle-benchmark.txt.chronicle"),
        ]:
            with tempfile.TemporaryDirectory(prefix="scratch-", dir=root) as scratch:
                protected = Path(scratch) / relative
                protected.write_bytes(sentinel)
                result = run(command, cwd=scratch)
                check(command + " rejects occupied " + relative, result.returncode != 0)
                check(command + " preserves " + relative, protected.exists() and protected.read_bytes() == sentinel)
    print(f"chronicle_preservation_checks={checks} failures={failures}")
    return bool(failures)


if __name__ == "__main__":
    raise SystemExit(main())
