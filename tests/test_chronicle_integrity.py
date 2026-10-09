#!/usr/bin/env python3
"""Fail-closed Chronicle store/receipt regression checks; stdlib only."""
import hashlib
import json
from fractions import Fraction
from pathlib import Path
import random
import struct
import subprocess
import sys
import tempfile


def record(amount="100", predicate="BORROWED_FROM"):
    return f"@chronicle\tAlice\t{predicate}\tBob\t{amount}\tSAR\tT1\tASSERTED\tPOSITIVE\tCONFIRMED\n"


def main():
    binary = str(Path(sys.argv[1]).resolve())
    failures = []
    checks = 0
    with tempfile.TemporaryDirectory(prefix="chronicle-integrity-") as folder:
        root = Path(folder)

        def run(*args):
            result = subprocess.run([binary, *map(str, args)], cwd=root, text=True, encoding="utf-8", capture_output=True)
            diagnostics = ("AddressSanitizer", "UndefinedBehaviorSanitizer", "LeakSanitizer", "runtime error:")
            if result.returncode not in (0, 1, 2) or any(x in result.stderr for x in diagnostics):
                raise RuntimeError(f"abnormal CLI exit {result.returncode}: {args!r}\n{result.stderr[:4000]}")
            return result

        def check(name, valid):
            nonlocal checks
            checks += 1
            if not valid:
                failures.append(name)
                print(f"FAIL {name}")

        source = root / "source.txt"
        source.write_text("prose\r\n" + record() + "tail\n", encoding="utf-8")
        check("valid ingestion", run("ingest", source).returncode == 0)
        store = Path(str(source) + ".chronicle")
        receipt = Path(str(store) + ".receipt")
        original = store.read_bytes()
        original_receipt = receipt.read_bytes()
        check("valid receipt", run("verify", receipt).returncode == 0)
        query = run("query", store, "Alice Bob")
        check("valid query", query.returncode == 0)
        query_receipt = Path(query.stdout.split("receipt=", 1)[1].strip())
        original_query_receipt = query_receipt.read_bytes()
        check("valid query receipt", run("verify", query_receipt).returncode == 0)
        source_size = struct.unpack_from("<Q", original, 48)[0]
        event_at = 68 + source_size
        mutations = {}
        for name, offset in [("event id", 0), ("span id", 32), ("line start", 80),
                             ("line end", 88), ("span hash", 96), ("amount present", 128),
                             ("amount num", 129), ("amount den", 137)]:
            changed = bytearray(original)
            changed[event_at + offset] ^= 1
            mutations[name] = changed
        for name, offset in [("negative numerator", 129), ("negative denominator", 137)]:
            changed = bytearray(original)
            struct.pack_into("<Q", changed, event_at + offset, (1 << 64) - 1)
            mutations[name] = changed
        offset = event_at + 145
        for name in ["subject", "predicate", "object", "currency", "time", "modality", "polarity", "status"]:
            n = struct.unpack_from("<I", original, offset)[0]
            changed = bytearray(original)
            changed[offset + 4] ^= 1
            mutations[name] = changed
            offset += 4 + n
        changed = bytearray(original[:event_at])
        struct.pack_into("<I", changed, 64, 0)
        mutations["omitted event"] = changed
        changed = bytearray(original)
        struct.pack_into("<I", changed, 64, 2)
        changed.extend(original[event_at:])
        mutations["duplicated event"] = changed
        mutations["trailing store data"] = original + b"x"
        changed = bytearray(original)
        subject_at = event_at + 145
        struct.pack_into("<I", changed, subject_at, 7)
        changed[subject_at + 9:subject_at + 9] = b"\x00Z"
        mutations["embedded NUL field alias"] = changed
        for name, changed in mutations.items():
            store.write_bytes(changed)
            check("store rejects " + name, run("query", store, "Alice Bob").returncode != 0)
            # Recomputing the receipt's outer hash must not bless fabricated event fields.
            digest = hashlib.sha256(changed).hexdigest().encode()
            forged = original_receipt.replace(hashlib.sha256(original).hexdigest().encode(), digest)
            receipt.write_bytes(forged)
            check("receipt rejects " + name, run("verify", receipt).returncode != 0)
        store.write_bytes(original)
        receipt.write_bytes(original_receipt)
        for name, changed in {
            "unknown field": original_receipt + b"unknown:ignored\n",
            "duplicate kind": original_receipt + b"kind:INGEST\n",
            "duplicate hash": original_receipt + next(x for x in original_receipt.splitlines(True) if x.startswith(b"result_sha256:")),
            "truncated last line": original_receipt.rstrip(b"\n"),
            "overlong hash": original_receipt.replace(b"result_sha256:", b"result_sha256:" + b"0" * 8192),
            "hash trailing junk": original_receipt.replace(b"\nresult_sha256:", b"JUNK\nresult_sha256:"),
            "embedded NUL": original_receipt + b"\x00unknown:ignored\n",
            "nonempty ingest question": original_receipt.replace(b"question_hex:\n", b"question_hex:41\n"),
            "missing question field": original_receipt.replace(b"question_hex:\n", b""),
        }.items():
            receipt.write_bytes(changed)
            check("receipt rejects " + name, run("verify", receipt).returncode != 0)
        receipt.write_bytes(original_receipt)
        for index, amount in enumerate(["/1", "-0", "+1", " 1", "1/", "1/0", "1/-1", "9223372036854775808", "1/9223372036854775808"]):
            source = root / f"invalid-{index}.txt"
            source.write_text(record(amount), encoding="utf-8")
            check("rational rejects " + repr(amount), run("ingest", source).returncode != 0)
        for index, (amount, expected) in enumerate([("0/7", (0, 1)), ("6/8", (3, 4)), ("9223372036854775807/9223372036854775806", (9223372036854775807, 9223372036854775806))]):
            source = root / f"valid-{index}.txt"
            store = Path(str(source) + ".chronicle")
            source.write_text(record(amount), encoding="utf-8")
            ingested = run("ingest", source)
            result = run("query", store, "Alice Bob") if ingested.returncode == 0 else ingested
            parsed = json.loads(result.stdout.splitlines()[0]) if result.returncode == 0 else {}
            debt = parsed.get("debt") or {}
            check("rational normalization " + amount, (debt.get("num"), debt.get("den")) == expected and debt.get("currency") == "SAR")
        # Adjacent large fractions overflow 64-bit cross products; the portable
        # comparison must distinguish them, exactly as Python's integers do.
        source = root / "ordering.txt"
        store = Path(str(source) + ".chronicle")
        source.write_text(record("9223372036854775807/9223372036854775806") + record("1", "PAID_TO"), encoding="utf-8")
        ingested = run("ingest", source)
        result = run("query", store, "Alice Bob") if ingested.returncode == 0 else ingested
        check("rational exact ordering", result.returncode == 0 and json.loads(result.stdout.splitlines()[0])["status"] == "PARTIAL")
        randomizer = random.Random(19001)
        vectors = [(1, 2, 2, 4), (1, 3, 1, 2), (2, 3, 1, 2), (5, 3, 8, 5)]
        vectors += [tuple(randomizer.randrange(1, 2**63) for _ in range(4)) for _ in range(24)]
        for index, (dn, dd, pn, pd) in enumerate(vectors):
            source = root / f"fraction-order-{index}.txt"
            store = Path(str(source) + ".chronicle")
            source.write_text(record(f"{dn}/{dd}") + record(f"{pn}/{pd}", "PAID_TO"), encoding="utf-8")
            ingested = run("ingest", source)
            result = run("query", store, "Alice Bob") if ingested.returncode == 0 else ingested
            debt, payment = Fraction(dn, dd), Fraction(pn, pd)
            expected = "SUPPORTED" if payment == debt else "CONFLICT" if payment > debt else "PARTIAL"
            check(f"integer fraction oracle {index}", result.returncode == 0 and json.loads(result.stdout.splitlines()[0])["status"] == expected)
        source = root / "capacity.txt"
        store = Path(str(source) + ".chronicle")
        source.write_text(record(), encoding="utf-8")
        check("restore ingestion", run("ingest", source).returncode == 0)
        for size in [2047, 2048, 4096]:
            result = run("query", store, "x" * size)
            if result.returncode == 0:
                path = Path(result.stdout.split("receipt=", 1)[1].strip())
                valid = run("verify", path).returncode == 0
            else:
                valid = True  # Explicit capacity rejection is allowed.
            check(f"receipt generation matches parser capacity {size}", valid)
        query_receipt.write_bytes(original_query_receipt)
    print(f"chronicle_integrity_checks={checks} failures={len(failures)}")
    return bool(failures)


if __name__ == "__main__":
    raise SystemExit(main())
