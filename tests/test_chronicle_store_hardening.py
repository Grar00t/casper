#!/usr/bin/env python3
"""Independent binary-layout mutation oracle for strict Chronicle stores."""
import hashlib
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


def main():
    binary = str(Path(sys.argv[1]).resolve())
    checks, failures = 0, 0
    story = b"@chronicle\tAlice\tBORROWED_FROM\tBob\t100\tSAR\tT1\tASSERTED\tPOSITIVE\tCONFIRMED\n"
    with tempfile.TemporaryDirectory(prefix="chronicle-store-hardening-") as folder:
        root = Path(folder)

        def run(*arguments):
            result = subprocess.run([binary, *map(str, arguments)], cwd=root,
                                    capture_output=True, encoding="utf-8", timeout=30)
            diagnostics = ("AddressSanitizer", "UndefinedBehaviorSanitizer", "LeakSanitizer", "runtime error:")
            if result.returncode not in (0, 1, 2) or any(x in result.stderr for x in diagnostics):
                raise RuntimeError(f"abnormal CLI exit {result.returncode}: {arguments!r}\n{result.stderr}")
            return result

        def check(name, condition):
            nonlocal checks, failures
            checks += 1
            if not condition:
                failures += 1
                print("FAIL " + name)

        source = root / "source.txt"
        source.write_bytes(story)
        result = run("ingest", source)
        check("baseline ingestion", result.returncode == 0)
        if result.returncode:
            return 1
        store = Path(str(source) + ".chronicle")
        receipt = Path(str(store) + ".receipt")
        original = store.read_bytes()
        original_receipt = receipt.read_bytes()
        store_hash = hashlib.sha256(original).hexdigest().encode()
        event_at = 68 + len(story)
        query_receipt = Path(str(store) + ".query-" + hashlib.sha256(b"Alice Bob").hexdigest()[:16] + ".receipt")
        mutations = {}
        field_at = event_at + 145
        for field in ("subject", "predicate", "object", "currency", "time", "modality", "polarity", "status"):
            length = struct.unpack_from("<I", original, field_at)[0]
            for added in (1, 2):
                changed = bytearray(original)
                struct.pack_into("<I", changed, field_at, length + added)
                changed[field_at + 4 + length:field_at + 4 + length] = b"\x00" * added
                mutations[f"{field} with {added} trailing NUL bytes"] = changed
            for length_alias in (128, (1 << 32) - 1):
                changed = bytearray(original)
                struct.pack_into("<I", changed, field_at, length_alias)
                mutations[f"{field} length {length_alias}"] = changed
            field_at += 4 + length
        for name, offset, fmt, values in (
            ("source size", 48, "<Q", (0, len(story) - 1, len(story) + 1, 8 * 1024 * 1024 + 1, (1 << 64) - 1)),
            ("event count", 64, "<I", (0, 2, 4097, (1 << 32) - 1)),
            ("byte start", event_at + 64, "<Q", (1, (1 << 64) - 1)),
            ("byte end", event_at + 72, "<Q", (0, (1 << 64) - 1)),
            ("line start", event_at + 80, "<Q", (0, (1 << 64) - 1)),
            ("line end", event_at + 88, "<Q", (0, (1 << 64) - 1)),
        ):
            for value in values:
                changed = bytearray(original)
                struct.pack_into(fmt, changed, offset, value)
                mutations[f"{name} {value}"] = changed
        mutations["trailing byte"] = original + b"x"
        for name, changed in mutations.items():
            store.write_bytes(changed)
            check("query rejects " + name, run("query", store, "Alice Bob").returncode != 0)
            # The oracle requires canonical event encoding even if an attacker
            # recomputes every receipt digest over the malformed serialization.
            digest = hashlib.sha256(changed).hexdigest().encode()
            receipt.write_bytes(original_receipt.replace(store_hash, digest))
            check("receipt rejects " + name, run("verify", receipt).returncode != 0)
            if query_receipt.exists():
                query_receipt.unlink()

        for cut in range(len(original)):
            store.write_bytes(original[:cut])
            check(f"rejects truncation at {cut}", run("query", store, "Alice Bob").returncode != 0)

        store.write_bytes(original)
        receipt.write_bytes(original_receipt)
        check("restored receipt", run("verify", receipt).returncode == 0)
        results = [run("query", store, "Alice Bob") for _ in range(3)]
        check("three successful replays", all(row.returncode == 0 for row in results))
        check("three byte-identical replays", len({row.stdout for row in results}) == 1)
    print(f"chronicle_store_hardening_checks={checks} failures={failures}")
    return int(failures != 0)


if __name__ == "__main__":
    raise SystemExit(main())
