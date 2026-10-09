#!/usr/bin/env python3
"""Independent CLI resource ceilings and exact literal membership oracle."""

import argparse
import hashlib
import json
import pathlib
import random
import struct
import subprocess
import tempfile


SOURCE_LIMIT = 8 * 1024 * 1024
EVENT_LIMIT = 4096


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=pathlib.Path)
    binary = str(parser.parse_args().binary.resolve())
    failures, checks = 0, 0
    digest = hashlib.sha256()

    with tempfile.TemporaryDirectory(prefix="chronicle-resources-") as directory:
        root = pathlib.Path(directory)

        def run(*arguments, reject=False):
            result = subprocess.run([binary, *map(str, arguments)], cwd=root,
                                    capture_output=True, encoding="utf-8", timeout=60)
            if "AddressSanitizer" in result.stderr or "runtime error:" in result.stderr:
                raise ValueError(f"sanitizer diagnostic: {result.stderr.strip()}")
            expected = (1, 2) if reject else (0,)
            if result.returncode not in expected:
                raise ValueError(f"exit={result.returncode}, expected={expected}: {result.stderr.strip()}")
            return result

        def check(name, operation):
            nonlocal checks, failures
            checks += 1
            try:
                operation()
                print(f"PASS {name}")
            except (ValueError, KeyError, StopIteration, OSError, subprocess.TimeoutExpired) as error:
                failures += 1
                print(f"FAIL {name}: {error}")

        def equal(actual, expected, description):
            if actual != expected:
                raise ValueError(f"{description}: actual={actual!r}, expected={expected!r}")

        def ingest(name, data):
            source = root / (name + ".txt")
            source.write_bytes(data)
            result = run("ingest", source)
            store = pathlib.Path(next(row[6:] for row in result.stdout.splitlines() if row.startswith("store=")))
            receipt = pathlib.Path(next(row[8:] for row in result.stdout.splitlines() if row.startswith("receipt=")))
            run("verify", receipt)
            return source, store, receipt

        def event_count(store):
            with store.open("rb") as stream:
                header = stream.read(68)
            if len(header) != 68:
                raise ValueError("short store header")
            return struct.unpack_from("<I", header, 64)[0]

        def claim(store, fields, expected):
            result = run("claim", store, *fields)
            encoded = result.stdout.splitlines()[0]
            value = json.loads(encoded)
            equal(value["status"], "EXACT_STATED" if expected else "UNRESOLVED", "literal status")
            equal(len(value["evidence"]), expected, "exact occurrence count")
            equal(tuple(value[key] for key in ("subject", "predicate", "object")), fields, "query fields")
            receipt = next(row[8:] for row in result.stdout.splitlines() if row.startswith("receipt="))
            run("verify", receipt)
            digest.update(encoded.encode())
            return value

        def maximum_source():
            source, store, receipt = ingest("maximum", b"x" * SOURCE_LIMIT)
            equal(event_count(store), 0, "unsupported maximum source event count")
            before = hashlib.sha256(store.read_bytes()).digest()
            receipt_before = receipt.read_bytes()
            run("ingest", source)
            equal(hashlib.sha256(store.read_bytes()).digest(), before, "repeat ingest store bytes")
            equal(receipt.read_bytes(), receipt_before, "repeat ingest receipt bytes")
            run("verify", receipt)

        def rejected_source(name, data):
            source = root / (name + ".txt")
            source.write_bytes(data)
            run("ingest", source, reject=True)
            store = pathlib.Path(str(source) + ".chronicle")
            if store.exists() or pathlib.Path(str(store) + ".receipt").exists():
                raise ValueError("rejected ingest left a store or receipt")

        check("8MiB_source_and_byte_identical_reingest", maximum_source)
        check("8MiB_plus_one_rejected", lambda: rejected_source("oversize", b"x" * (SOURCE_LIMIT + 1)))

        def maximum_events():
            rows = [f"Person{i:04d} owns Item{i:04d}.\n" for i in range(EVENT_LIMIT)]
            _, store, _ = ingest("events_maximum", "".join(rows).encode())
            equal(event_count(store), EVENT_LIMIT, "maximum event count")
            first = claim(store, ("Person0000", "owns", "Item0000"), 1)
            last = claim(store, ("Person4095", "owns", "Item4095"), 1)
            equal(first["evidence"][0]["line_start"], 1, "first event line")
            equal(last["evidence"][0]["line_start"], EVENT_LIMIT, "last event line")

        check("4096_literal_events_with_exact_claims", maximum_events)
        check("4097_events_rejected", lambda: rejected_source("events_oversize", b"Alice owns Book.\n" * (EVENT_LIMIT + 1)))

        def empty_source():
            _, store, _ = ingest("empty", b"")
            equal(event_count(store), 0, "empty event count")
            claim(store, ("Alice", "owns", "Book"), 0)

        check("empty_corpus", empty_source)
        for name, data in [
            ("invalid_utf8", b"Alice owns \xff.\n"),
            ("overlong_utf8", b"Alice owns \xc0\xaf.\n"),
            ("utf8_surrogate", b"Alice owns \xed\xa0\x80.\n"),
            ("truncated_utf8", b"Alice owns \xe2\x82.\n"),
            ("embedded_NUL", b"Alice owns Book.\x00\n"),
        ]:
            check(name, lambda n=name, d=data: rejected_source(n, d))

        def unsupported_prose():
            _, store, _ = ingest("unsupported", b"Alice really owns Book.\nAlice owns Book\n")
            equal(event_count(store), 0, "unsupported prose event count")
            claim(store, ("Alice", "owns", "Book"), 0)
            value = json.loads(run("query", store, "Did Alice repay Bob?").stdout.splitlines()[0])
            equal(value["status"], "UNKNOWN", "unsupported prose debt result")
            equal(value["evidence"], [], "unsupported prose debt evidence")

        check("unsupported_prose_creates_no_events", unsupported_prose)

        rng = random.Random(20261008)
        records = [(rng.choice(("Alice", "Alison", "أحمد", "أحمدان", "Bob")),
                    rng.choice(("owns", "likes", "reads", "يملك")),
                    rng.choice(("Book", "Pen", "كتاب", "Cup", "Paper"))) for _ in range(50)]
        corpus = "".join(" ".join(fields) + ".\n" for fields in records).encode()
        random_store = None

        def random_corpus():
            nonlocal random_store
            _, random_store, _ = ingest("random_literals", corpus)
            equal(event_count(random_store), len(records), "random record count")

        check("50_random_triples_ingested", random_corpus)
        if random_store is not None:
            for index, fields in enumerate(records):
                check(f"random_exact_{index:02d}",
                      lambda f=fields: claim(random_store, f, sum(row == f for row in records)))
                changed = (fields[0], fields[1], fields[2] + "Absent")
                check(f"random_object_mismatch_{index:02d}",
                      lambda f=changed: claim(random_store, f, sum(row == f for row in records)))
            subject, predicate, target = records[0]
            check("random_subject_prefix_rejected", lambda: claim(random_store, (subject[:-1], predicate, target), 0))
            check("random_predicate_prefix_rejected", lambda: claim(random_store, (subject, predicate[:-1], target), 0))
    print(f"resource_cases={checks} failures={failures} result_sha256={digest.hexdigest()}")
    return int(failures != 0)


if __name__ == "__main__":
    raise SystemExit(main())
