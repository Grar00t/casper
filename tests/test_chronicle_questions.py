#!/usr/bin/env python3
"""Full query-shape and exact role/name oracle for debt settlement."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile


def records(borrower, lender):
    return "".join(f"@chronicle\t{borrower}\t{predicate}\t{lender}\t100\tSAR\tT{time}\tASSERTED\tPOSITIVE\tCONFIRMED\n"
                   for predicate, time in (("BORROWED_FROM", 1), ("PAID_TO", 2)))


def invoke(binary, *arguments):
    result = subprocess.run([binary, *map(str, arguments)], capture_output=True,
                            text=True, encoding="utf-8", timeout=30)
    if result.returncode != 0:
        raise ValueError(f"exit={result.returncode}: {result.stderr}")
    if any(marker in result.stderr for marker in ("AddressSanitizer", "runtime error:")):
        raise ValueError(result.stderr)
    return result.stdout


def assess(binary, folder, name, text, question, pair, digest):
    source = folder / (name + ".txt")
    source.write_bytes(text.encode())
    ingest = invoke(binary, "ingest", source)
    store = next(line[6:] for line in ingest.splitlines() if line.startswith("store="))
    output = invoke(binary, "query", store, question)
    result = json.loads(output.splitlines()[0])
    expected = "SUPPORTED" if pair else "UNKNOWN"
    if result["status"] != expected:
        raise ValueError(f"expected {expected}, got {result['status']}")
    if pair:
        actual = (result["debt"]["borrower"], result["debt"]["lender"])
        if actual != pair or len(result["evidence"]) != 2:
            raise ValueError(f"wrong entity identity or evidence: {actual}")
        if result["settled"] != {"num": 100, "den": 1}:
            raise ValueError("wrong selected settlement")
    elif result["debt"] is not None or result["evidence"] or result["settled"] != {"num": 0, "den": 1}:
        raise ValueError("unsupported query selected debt evidence")
    if result["question"] != question:
        raise ValueError("query bytes changed")
    receipt = next(line[8:] for line in output.splitlines() if line.startswith("receipt="))
    if invoke(binary, "verify", receipt).strip() != "VALID":
        raise ValueError("query receipt did not verify")
    digest.update(output.splitlines()[0].encode())


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    binary = str(parser.parse_args().binary.resolve())
    arabic, english = records("أحمد", "خالد"), records("Alice", "Bob")
    a, e = ("أحمد", "خالد"), ("Alice", "Bob")
    multi = ("أحمد بن سالم", "خالد بن عمر")
    cases = [
        ("arabic_pair", arabic, "أحمد خالد", a),
        ("ascii_pair", english, "Alice Bob", e),
        ("arabic_pair_question", arabic, "أحمد خالد؟", a),
        ("legacy_arabic_punctuation", arabic, "،أحمد خالد؟", a),
        ("english_question", english, "Did Alice repay Bob?", e),
        ("arabic_debt_question", arabic, "هل سدد أحمد دين خالد؟", a),
        ("arabic_ascii_question_mark", arabic, "هل سدد أحمد دين خالد?", a),
        ("arabic_status_question", arabic, "ما حالة سداد دين أحمد إلى خالد؟", a),
        ("unknown_longer_borrower", arabic, "هل سدد أحمد سالم دين خالد؟", None),
        ("unknown_longer_lender", arabic, "هل سدد أحمد دين خالد سالم؟", None),
        ("unknown_longer_bare", arabic, "أحمد سالم خالد", None),
        ("known_longer_borrower", arabic + records("أحمد سالم", "خالد"), "هل سدد أحمد سالم دين خالد؟", ("أحمد سالم", "خالد")),
        ("known_shorter_borrower", arabic + records("أحمد سالم", "خالد"), "هل سدد أحمد دين خالد؟", a),
        ("multiword_question", records(*multi), "ما حالة سداد دين أحمد بن سالم إلى خالد بن عمر؟", multi),
        ("multiword_pair", records(*multi), "أحمد بن سالم خالد بن عمر", multi),
        ("delimiter_borrower_rejected", records("أحمد دين سالم", "خالد"), "هل سدد أحمد دين سالم دين خالد؟", None),
        ("delimiter_lender_rejected", records("أحمد", "خالد إلى عمر"), "ما حالة سداد دين أحمد إلى خالد إلى عمر؟", None),
        ("delimiter_borrower_escape", records("أحمد دين سالم", "خالد"), "@settlement\tأحمد دين سالم\tخالد", ("أحمد دين سالم", "خالد")),
        ("delimiter_lender_escape", records("أحمد", "خالد إلى عمر"), "@settlement\tأحمد\tخالد إلى عمر", ("أحمد", "خالد إلى عمر")),
        ("english_negation_name", records("Alice not", "Bob"), "Did Alice not repay Bob?", None),
        ("english_negation_name_escape", records("Alice not", "Bob"), "@settlement\tAlice not\tBob", ("Alice not", "Bob")),
        ("delimiter_prefix_name", records("دينار", "خالد"), "هل سدد دينار دين خالد؟", ("دينار", "خالد")),
        ("negated_english", english, "Did Alice not repay Bob?", None),
        ("negated_arabic", arabic, "هل لم يسدد أحمد دين خالد؟", None),
        ("unrelated_english", english, "Are Alice and Bob astronauts?", None),
        ("unrelated_arabic", arabic, "هل أحمد خالد رائدا فضاء؟", None),
        ("empty_query", arabic, "", None),
        ("prefix_rejected", arabic, "سؤال: هل سدد أحمد دين خالد؟", None),
        ("suffix_rejected", arabic, "هل سدد أحمد دين خالد؟ ربما", None),
        ("two_questions", arabic, "هل سدد أحمد دين خالد؟ هل سدد أحمد دين خالد؟", None),
        ("reversed_roles_question", arabic, "هل سدد خالد دين أحمد؟", None),
        ("reversed_roles_pair", arabic, "خالد أحمد", None),
        ("diacritics_distinct", arabic, "هل سدد أَحمد دين خالد؟", None),
        ("case_distinct", english, "Did alice repay Bob?", None),
        ("escape_exact", arabic, "@settlement\tأحمد\tخالد", a),
        ("escape_wrong_name", arabic, "@settlement\tأحمد سالم\tخالد", None),
        ("escape_wrong_roles", arabic, "@settlement\tخالد\tأحمد", None),
        ("escape_extra_field", arabic, "@settlement\tأحمد\tخالد\textra", None),
        ("escape_missing_field", arabic, "@settlement\tأحمد", None),
        ("escape_extra_newline", arabic, "@settlement\tأحمد\tخالد\n", None),
        ("spaces_not_normalized", english, "Alice  Bob", None),
        ("punctuation_name_escape", records('A"lice', "Bob"), '@settlement\tA"lice\tBob', ('A"lice', "Bob")),
    ]
    maximum = ("A" * 127, "B" * 127)
    cases.append(("maximum_fields_escape", records(*maximum), "@settlement\t" + "\t".join(maximum), maximum))
    cases.append(("maximum_fields_question", records(*maximum), f"Did {maximum[0]} repay {maximum[1]}?", maximum))
    failures, digest = 0, hashlib.sha256()
    with tempfile.TemporaryDirectory(prefix="chronicle-questions-") as directory:
        for name, text, question, pair in cases:
            try:
                assess(binary, Path(directory), name, text, question, pair, digest)
                print(f"PASS {name}")
            except (ValueError, OSError, KeyError, StopIteration, subprocess.TimeoutExpired) as error:
                failures += 1
                print(f"FAIL {name}: {error}")
    print(f"question_cases={len(cases)} failures={failures} result_sha256={digest.hexdigest()}")
    return int(failures != 0)


if __name__ == "__main__":
    raise SystemExit(main())
