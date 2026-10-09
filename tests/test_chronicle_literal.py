#!/usr/bin/env python3
"""External exact-match and byte-span oracle for bounded literal retrieval."""

import argparse
import hashlib
import json
import pathlib
import subprocess
import tempfile


def invoke(binary, *arguments, success=True):
    result = subprocess.run([binary, *arguments], capture_output=True,
                            text=True, encoding="utf-8")
    if success and result.returncode != 0:
        raise ValueError(f"exit={result.returncode}: {result.stderr.strip()}")
    if not success and (result.returncode not in (1, 2) or
                        "AddressSanitizer" in result.stderr or "runtime error:" in result.stderr):
        raise ValueError(f"expected clean rejection, exit={result.returncode}: {result.stderr.strip()}")
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=pathlib.Path)
    binary = str(parser.parse_args().binary.resolve())
    cases = [
        ("exact", b"Alice owns Book.\n", ("Alice", "owns", "Book"), 1),
        ("wrong_object", b"Alice owns Book.\n", ("Alice", "owns", "Pen"), 0),
        ("similar_subject", b"Alice owns Book.\n", ("Ali", "owns", "Book"), 0),
        ("similar_predicate", b"Alice owns Book.\n", ("Alice", "own", "Book"), 0),
        ("similar_object", b"Alice owns Book.\n", ("Alice", "owns", "Boo"), 0),
        ("all_matches", b"Alice owns Book.\nAlice owns Book.\n", ("Alice", "owns", "Book"), 2),
        ("escaped_identity", b'A"li owns B\\ook.\n', ('A"li', "owns", "B\\ook"), 1),
        ("arabic", "أحمد يملك كتاب.\n".encode(), ("أحمد", "يملك", "كتاب"), 1),
        ("arabic_similar", "أحمد يملك كتاب.\n".encode(), ("أحمدان", "يملك", "كتاب"), 0),
        ("exact_bytes", b"prefix\r\n \tAlice\t owns  Book. \t\r\n", ("Alice", "owns", "Book"), 1),
        ("missing_period", b"Alice owns Book\n", ("Alice", "owns", "Book"), 0),
        ("four_tokens", b"Alice really owns Book.\n", ("Alice", "owns", "Book"), 0),
        ("multiple_sentences", b"Alice owns Book. Bob owns Pen.\n", ("Alice", "owns", "Book"), 0),
        ("ambiguous_punctuation", b"Alice owns Book;\n", ("Alice", "owns", "Book"), 0),
        ("embedded_period", b"Alice owns B.ook.\n", ("Alice", "owns", "B.ook"), 0),
        ("arabic_question", "أحمد يملك كتاب؟.\n".encode(), ("أحمد", "يملك", "كتاب؟"), 0),
        ("max_field", b"A" * 127 + b" owns Book.\n", ("A" * 127, "owns", "Book"), 1),
        ("utf8_byte_limit", "أ".encode() * 63 + b" owns Book.\n", ("أ" * 63, "owns", "Book"), 1),
        ("typed_events_not_literals", b"@chronicle\tAlice\tPAID_TO\tBob\t1\tSAR\tT1\tASSERTED\tPOSITIVE\tCONFIRMED\n", ("Alice", "PAID_TO", "Bob"), 0),
        ("typed_modality_spoof", b"@chronicle\tAlice\tPAID_TO\tBob\t-\t-\t-\tLITERAL\tPOSITIVE\tASSERTED\n", ("Alice", "PAID_TO", "Bob"), 0),
    ]
    failures, count = 0, 0
    digest = hashlib.sha256()
    with tempfile.TemporaryDirectory(prefix="chronicle-literal-") as directory:
        root = pathlib.Path(directory)
        for name, data, claim, matches in cases:
            count += 1
            try:
                source = root / (name + ".txt")
                source.write_bytes(data)
                ingest = invoke(binary, "ingest", str(source))
                store = next(row[6:] for row in ingest.stdout.splitlines() if row.startswith("store="))
                query = invoke(binary, "claim", store, *claim)
                result = json.loads(query.stdout.splitlines()[0])
                expected = "EXACT_STATED" if matches else "UNRESOLVED"
                if result["status"] != expected or len(result["evidence"]) != matches:
                    raise ValueError(f"expected {expected}, {matches} matches: {result}")
                if result.get("assessment") != "LITERAL_RETRIEVAL":
                    raise ValueError("missing literal retrieval scope")
                if tuple(result[key] for key in ("subject", "predicate", "object")) != claim:
                    raise ValueError("claim changed during JSON encoding")
                for evidence in result["evidence"]:
                    start, end = evidence["byte_start"], evidence["byte_end"]
                    span = data[start:end]
                    if not (0 <= start < end <= len(data)):
                        raise ValueError("span outside source")
                    if evidence["text"].encode() != span:
                        raise ValueError("source bytes changed")
                    if evidence["exact_span_sha256"] != hashlib.sha256(span).hexdigest():
                        raise ValueError("span hash differs from independent oracle")
                    if evidence["document_sha256"] != hashlib.sha256(data).hexdigest():
                        raise ValueError("document hash differs from independent oracle")
                    line = data[:start].count(b"\n") + 1
                    if evidence["line_start"] != line or evidence["line_end"] != line:
                        raise ValueError("source line identity changed")
                receipt = next(row[8:] for row in query.stdout.splitlines() if row.startswith("receipt="))
                invoke(binary, "verify", receipt)
                changed = pathlib.Path(receipt).read_bytes().replace(b"result_sha256:", b"result_sha256:0", 1)
                tampered = root / (name + ".tampered.receipt")
                tampered.write_bytes(changed)
                invoke(binary, "verify", str(tampered), success=False)
                digest.update(query.stdout.splitlines()[0].encode())
                print(f"PASS {name}")
            except (ValueError, KeyError, StopIteration) as error:
                failures += 1
                print(f"FAIL {name}: {error}")

        source = root / "limits.txt"
        source.write_bytes(b"Alice owns Book.\n")
        ingest = invoke(binary, "ingest", str(source))
        store = next(row[6:] for row in ingest.stdout.splitlines() if row.startswith("store="))
        for name, claim in [("empty_field", ("", "owns", "Book")),
                            ("oversize_claim", ("A" * 128, "owns", "Book")),
                            ("extra_tab", ("Alice\tBob", "owns", "Book")),
                            ("claim_newline", ("Alice", "owns", "Book\n"))]:
            count += 1
            try:
                invoke(binary, "claim", store, *claim, success=False)
                print(f"PASS {name}")
            except ValueError as error:
                failures += 1
                print(f"FAIL {name}: {error}")
        count += 1
        try:
            source = root / "oversize_source.txt"
            source.write_bytes(b"A" * 128 + b" owns Book.\n")
            invoke(binary, "ingest", str(source), success=False)
            print("PASS oversize_source")
        except ValueError as error:
            failures += 1
            print(f"FAIL oversize_source: {error}")
    print(f"literal_cases={count} failures={failures} result_sha256={digest.hexdigest()}")
    return int(failures != 0)


if __name__ == "__main__":
    raise SystemExit(main())
