#!/usr/bin/env python3
"""Independent UTF-8 scalar, punctuation, exact-byte and path-boundary checks."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile


def invoke(binary, *arguments, reject=False):
    result = subprocess.run([binary, *arguments], capture_output=True, timeout=30)
    if result.returncode not in ((1, 2) if reject else (0,)):
        raise ValueError(f"unexpected exit={result.returncode}: {result.stderr!r}")
    if any(marker in result.stderr for marker in
           (b"AddressSanitizer", b"runtime error:", b"LeakSanitizer")):
        raise ValueError(f"sanitizer finding: {result.stderr!r}")
    return result.stdout


def output_path(output, prefix):
    return next(line[len(prefix):] for line in output.splitlines()
                if line.startswith(prefix))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    binary = str(parser.parse_args().binary.resolve())
    failures = count = 0
    digest = hashlib.sha256()

    def check(name, action):
        nonlocal count, failures
        count += 1
        try:
            action()
            print(f"PASS {name}")
        except (ValueError, KeyError, StopIteration, OSError, subprocess.TimeoutExpired) as error:
            failures += 1
            print(f"FAIL {name}: {error}")

    with tempfile.TemporaryDirectory(prefix="chronicle-utf8-") as directory:
        root = Path(directory)

        def invalid_source(name, data):
            source = root / (name + ".txt")
            source.write_bytes(data)
            invoke(binary, "ingest", str(source), reject=True)
            if Path(str(source) + ".chronicle").exists():
                raise ValueError("invalid input created a store")

        # UTF-8 rejects non-scalars, overlong forms, isolated continuations and
        # every incomplete prefix, including failures at the end of the file.
        invalid = {
            "embedded_nul": b"\x00", "continuation_80": b"\x80",
            "continuation_bf": b"\xbf", "overlong_nul": b"\xc0\x80",
            "overlong_ascii": b"\xc1\xbf", "overlong_three": b"\xe0\x9f\xbf",
            "overlong_four": b"\xf0\x8f\xbf\xbf", "surrogate_first": b"\xed\xa0\x80",
            "surrogate_last": b"\xed\xbf\xbf", "above_unicode": b"\xf4\x90\x80\x80",
            "invalid_f5": b"\xf5\x80\x80\x80", "invalid_ff": b"\xff",
            "truncated_two": b"\xc2", "truncated_three_one": b"\xe0",
            "truncated_three_two": b"\xe0\xa0", "truncated_four_one": b"\xf0",
            "truncated_four_two": b"\xf0\x90", "truncated_four_three": b"\xf0\x90\x80",
            "bad_continuation": b"\xe2A\x80", "split_by_lf": b"\xe2\n\x82\xac",
        }
        for name, encoded in invalid.items():
            for location, payload in (("end", b"Alice owns " + encoded),
                                      ("middle", b"Alice owns " + encoded + b".\n")):
                label = f"{name}_{location}"
                check(label, lambda n=label, p=payload: invalid_source(n, p))

        def claim_case(name, token, matches):
            source = root / (name + ".txt")
            data = (f"Alice owns {token}.\r\n").encode("utf-8")
            source.write_bytes(data)
            first = invoke(binary, "ingest", str(source))
            store = os.fsdecode(output_path(first, b"store="))
            before = Path(store).read_bytes()
            second = invoke(binary, "ingest", str(source))
            if first != second or Path(store).read_bytes() != before:
                raise ValueError("repeated ingestion changed stored bytes or paths")
            query = invoke(binary, "claim", store, "Alice", "owns", token)
            if invoke(binary, "claim", store, "Alice", "owns", token) != query:
                raise ValueError("repeated exact claim changed output bytes")
            result_bytes = query.splitlines()[0]
            result = json.loads(result_bytes)
            if result["status"] != ("EXACT_STATED" if matches else "UNRESOLVED"):
                raise ValueError(f"unexpected status: {result['status']}")
            if len(result["evidence"]) != matches or result["object"] != token:
                raise ValueError("field identity or evidence count changed")
            for evidence in result["evidence"]:
                span = data[evidence["byte_start"]:evidence["byte_end"]]
                if (evidence["text"].encode("utf-8") != span or
                        evidence["exact_span_sha256"] != hashlib.sha256(span).hexdigest() or
                        evidence["document_sha256"] != hashlib.sha256(data).hexdigest()):
                    raise ValueError("exact source bytes or independent SHA-256 differs")
            receipt = os.fsdecode(output_path(query, b"receipt="))
            if invoke(binary, "verify", receipt).strip() != b"VALID":
                raise ValueError("replay verification failed")
            digest.update(result_bytes)

        # Boundary scalars and supplementary-plane identities must survive JSON,
        # source spans, repeated ingest and receipt replay without normalization.
        for scalar in (0x80, 0x7FF, 0x800, 0xD7FF, 0xE000, 0xFFFF, 0x10000, 0x10FFFF):
            name = f"scalar_{scalar:06x}"
            check(name, lambda n=name, cp=scalar: claim_case(n, "B" + chr(cp) + "k", 1))
        for name, token in (("combining_identity", "cafe\u0301"),
                            ("precomposed_identity", "caf\u00e9"),
                            ("supplementary_identity", "Book\U0001f4da")):
            check(name, lambda n=name, t=token: claim_case(n, t, 1))

        separators = ".,;:!?@\u060c\u061b\u061f\u2024\u2025\u2026\u2027\u2028\u2029\u3002\uff0e\uff01\uff1f"
        for separator in separators:
            name = f"punctuation_{ord(separator):04x}"
            check(name, lambda n=name, p=separator: claim_case(n, "B" + p + "k", 0))

        if os.name != "nt":
            # POSIX can represent paths that violate Chronicle's UTF-8 contract.
            # Keep byte argv intact so Python cannot replace the malformed bytes.
            invalid_name = os.fsencode(root) + b"/invalid-\xff.txt"

            def invalid_ingest_path():
                with open(invalid_name, "wb") as handle:
                    handle.write(b"Alice owns Book.\n")
                invoke(binary, b"ingest", invalid_name, reject=True)
                if os.path.exists(invalid_name + b".chronicle"):
                    raise ValueError("invalid UTF-8 path created a store")

            check("invalid_utf8_ingest_path", invalid_ingest_path)
            source = root / "valid_path.txt"
            source.write_bytes(b"Alice owns Book.\n")
            output = invoke(binary, "ingest", str(source))
            valid_store = Path(os.fsdecode(output_path(output, b"store=")))
            invalid_store = os.fsencode(root) + b"/copied-\xff.chronicle"
            with open(invalid_store, "wb") as handle:
                handle.write(valid_store.read_bytes())
            check("invalid_utf8_query_path", lambda: invoke(
                binary, b"claim", invalid_store, b"Alice", b"owns", b"Book", reject=True))
            check("invalid_utf8_find_path", lambda: invoke(
                binary, b"find", invalid_store, b"Book", reject=True))

            def invalid_receipt_filename():
                receipt = os.fsencode(root) + b"/receipt-\xff.receipt"
                with open(receipt, "wb") as handle:
                    handle.write(Path(os.fsdecode(output_path(output, b"receipt="))).read_bytes())
                invoke(binary, b"verify", receipt, reject=True)

            check("invalid_utf8_receipt_filename", invalid_receipt_filename)

            def invalid_receipt_store_path():
                original = Path(os.fsdecode(output_path(output, b"receipt="))).read_bytes()
                changed = b"\n".join(
                    b"store_path_hex:" + invalid_store.hex().encode("ascii")
                    if row.startswith(b"store_path_hex:") else row
                    for row in original.split(b"\n"))
                receipt = root / "invalid-store-path.receipt"
                receipt.write_bytes(changed)
                invoke(binary, "verify", str(receipt), reject=True)

            check("invalid_utf8_receipt_store_path", invalid_receipt_store_path)

            for name in ("overlong_ascii", "surrogate_first", "above_unicode", "truncated_four_three"):
                for command in ("query", "find"):
                    check(f"invalid_{command}_{name}", lambda c=command, n=name: invoke(
                        binary, c.encode(), os.fsencode(valid_store), invalid[n], reject=True))

            def source_path_with_length(length):
                parent = root / "long_paths"
                while length - len(os.fsencode(parent)) - 1 > 240:
                    parent /= "d" * 100
                parent.mkdir(parents=True, exist_ok=True)
                return parent / ("a" * (length - len(os.fsencode(parent)) - 5) + ".txt")

            def maximum_store_path():
                source = source_path_with_length(2047 - len(".chronicle"))
                source.write_bytes(b"Alice owns Book.\n")
                ingested = invoke(binary, "ingest", str(source))
                store = os.fsdecode(output_path(ingested, b"store="))
                if len(os.fsencode(store)) != 2047:
                    raise ValueError("test did not reach serialized path boundary")
                queried = invoke(binary, "claim", store, "Alice", "owns", "Book")
                receipt = os.fsdecode(output_path(queried, b"receipt="))
                invoke(binary, "verify", receipt)

            def oversized_store_path():
                source = source_path_with_length(2048 - len(".chronicle"))
                source.write_bytes(b"Alice owns Book.\n")
                invoke(binary, "ingest", str(source), reject=True)
                if Path(str(source) + ".chronicle").exists():
                    raise ValueError("unrepresentable receipt path created a store")

            check("maximum_serialized_store_path", maximum_store_path)
            check("oversized_serialized_store_path", oversized_store_path)

    print(f"utf8_hardening_cases={count} failures={failures} result_sha256={digest.hexdigest()}")
    return int(failures != 0)


if __name__ == "__main__":
    raise SystemExit(main())
