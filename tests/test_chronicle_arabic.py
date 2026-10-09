#!/usr/bin/env python3
"""Independent arithmetic and byte-span oracle for bounded Arabic prose."""

import argparse
from fractions import Fraction
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

DEBT = "أقر أحمد بأنه اقترض من خالد مبلغ 100 ريال سعودي في اليوم الأول."
LINK = "أكد خالد أن سعود وسيط عنه في اليوم الثاني."
PAID = "أكد سعود استلام مبلغ 40 ريال سعودي من أحمد في اليوم الثاني."
PENDING = "حوّل أحمد إلى سعود مبلغ 60 ريال سعودي في اليوم الثالث ولم يثبت وصول الحوالة."
QUESTION = "ما حالة سداد دين أحمد إلى خالد؟"


def invoke(binary, *arguments, success=True):
    result = subprocess.run([binary, *arguments], capture_output=True,
                            text=True, encoding="utf-8", timeout=30)
    if (result.returncode == 0) != success:
        raise ValueError(f"unexpected exit={result.returncode}: {result.stderr.strip()}")
    if "AddressSanitizer" in result.stderr or "runtime error:" in result.stderr:
        raise ValueError(result.stderr)
    return result.stdout


def assess(binary, root, name, data, expected, amount, evidence_count=None, question=QUESTION):
    source = root / (name + ".txt")
    source.write_bytes(data)
    ingested = invoke(binary, "ingest", str(source))
    store = next(row[6:] for row in ingested.splitlines() if row.startswith("store="))
    queried = invoke(binary, "query", store, question)
    result = json.loads(queried.splitlines()[0])
    if result["status"] != expected:
        raise ValueError(f"expected {expected}, got {result['status']}")
    settled = result["settled"]
    if Fraction(settled["num"], settled["den"]) != Fraction(amount):
        raise ValueError(f"expected settled={amount}, got {settled}")
    if evidence_count is not None and len(result["evidence"]) != evidence_count:
        raise ValueError(f"expected {evidence_count} spans, got {len(result['evidence'])}")
    for span in result["evidence"]:
        start, end = span["byte_start"], span["byte_end"]
        if not 0 <= start < end <= len(data):
            raise ValueError("span outside input")
        original = data[start:end]
        if original != span["text"].encode("utf-8"):
            raise ValueError("changed original evidence bytes")
        if span["exact_span_sha256"] != hashlib.sha256(original).hexdigest():
            raise ValueError("wrong span digest")
        if span["document_sha256"] != hashlib.sha256(data).hexdigest():
            raise ValueError("wrong document digest")
        line = data[:start].count(b"\n") + 1
        if span["line_start"] != line or span["line_end"] != line:
            raise ValueError("wrong line positions")
    receipt = next(row[8:] for row in queried.splitlines() if row.startswith("receipt="))
    invoke(binary, "verify", receipt)
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    binary = str(parser.parse_args().binary.resolve())
    story = Path(__file__).with_name("fixtures").joinpath("chronicle_ahmed_prose_ar.txt").read_bytes()
    base = "\n".join((DEBT, LINK, PAID, PENDING))
    cases = [
        ("ordinary_story", story, "PARTIAL", "40", 4),
        ("crlf", base.replace("\n", "\r\n").encode(), "PARTIAL", "40", 4),
        ("far_apart", (DEBT + "\n" + "سرد طويل لا يثبت سداد الدين.\n" * 5000 + LINK + "\n" + PAID + "\n" + PENDING).encode(), "PARTIAL", "40", 4),
        ("long_unrelated_paragraph", ("وصف طويل للأحداث الأخرى " * 1000 + ".\n" + base).encode(), "PARTIAL", "40", 4),
        ("long_quoted_paragraph", ('"' + PAID + " كلام آخر " * 1000 + '".\n' + base).encode(), "PARTIAL", "40", 4),
        ("narrative_debt", base.replace(DEBT, "اقترض أحمد من خالد مبلغ 100 ريال سعودي في اليوم الأول.").encode(), "PARTIAL", "40", 4),
        ("missing_link", "\n".join((DEBT, PAID, PENDING)).encode(), "UNKNOWN", "0", None),
        ("negative_receipt", "\n".join((DEBT, LINK, PAID, PAID.replace("أكد", "نفى", 1))).encode(), "CONFLICT", "0", 4),
        ("negative_intermediary", "\n".join((DEBT, LINK, LINK.replace("أكد", "نفى", 1), PAID.replace("40", "100"))).encode(), "CONFLICT", "0", 4),
        ("different_currency", base.replace("40 ريال سعودي", "40 دولار أمريكي").encode(), "UNKNOWN", "0", None),
        ("ambiguous_currency", base.replace("40 ريال سعودي", "40 ريال").encode(), "UNKNOWN", "0", None),
        ("ascii_currency", base.replace("ريال سعودي", "SAR").encode(), "PARTIAL", "40", 4),
        ("explicit_usd", base.replace("ريال سعودي", "دولار أمريكي").encode(), "PARTIAL", "40", 4),
        ("full_receipt", "\n".join((DEBT, LINK, PAID.replace("40", "100"))).encode(), "SUPPORTED", "100", 3),
        ("arabic_digits", base.translate(str.maketrans("0123456789", "٠١٢٣٤٥٦٧٨٩")).encode(), "PARTIAL", "40", 4),
        ("fractions", base.replace("100", "1").replace("40", "١/٣").replace("60", "٢/٣").encode(), "PARTIAL", "1/3", 4),
        ("multiword_names", base.replace("أحمد", "أحمد بن سالم").replace("خالد", "خالد بن عمر").encode(), "PARTIAL", "40", 4, "ما حالة سداد دين أحمد بن سالم إلى خالد بن عمر؟"),
        ("wrong_payer", base.replace("من أحمد في اليوم الثاني", "من أحمدان في اليوم الثاني").encode(), "UNKNOWN", "0", None),
        ("name_diacritics_distinct", base.replace("من أحمد في اليوم الثاني", "من أَحمد في اليوم الثاني").encode(), "UNKNOWN", "0", None),
        ("wrong_agent", base.replace("سعود وسيط", "سعودان وسيط").encode(), "UNKNOWN", "0", None),
        ("duplicate_receipt", (base + "\n" + PAID).encode(), "PARTIAL", "40", 5),
        ("second_debt", (base + "\n" + DEBT.replace("100", "200")).encode(), "UNKNOWN", "0", None),
    ]
    for label, altered in [
        ("negated", "لم " + PAID), ("uncertain", "ربما " + PAID),
        ("conditional", "إذا " + PAID), ("quoted", '"' + PAID + '"'),
        ("reported", "قال شاهد: " + PAID),
        ("extra_clause", PAID[:-1] + " ولكنه لم يتأكد."),
        ("future", "سوف " + PAID), ("embedded_maybe", PAID.replace("أكد سعود", "أكد ربما سعود")),
        ("conditional_mid_name", PAID.replace("أكد سعود", "أكد سعود إذا")),
        ("unsupported_time", PAID.replace("اليوم الثاني", "الأسبوع الثاني")),
        ("extra_sentence", PAID + " لكن ذلك غير مؤكد."),
        ("unconfirmed_transfer", PENDING),
    ]:
        cases.append((label, "\n".join((DEBT, LINK, altered)).encode(), "UNKNOWN", "0", None))
    failures = 0
    digest = hashlib.sha256()
    with tempfile.TemporaryDirectory(prefix="chronicle-arabic-") as directory:
        root = Path(directory)
        for case in cases:
            try:
                result = assess(binary, root, *case)
                digest.update(json.dumps(result, sort_keys=True, ensure_ascii=False).encode("utf-8"))
                print("PASS " + case[0])
            except (ValueError, KeyError, StopIteration, subprocess.TimeoutExpired) as error:
                failures += 1
                print(f"FAIL {case[0]}: {error}")
        invalid = [
            ("oversize_name", DEBT.replace("أحمد", "أ" * 64)),
            ("bad_digit", DEBT.replace("100", "١x٠")),
            ("zero_denominator", DEBT.replace("100", "١/٠")),
            ("amount_overflow", DEBT.replace("100", "9223372036854775808")),
        ]
        for name, data in invalid:
            source = root / (name + ".txt")
            source.write_text(data, encoding="utf-8")
            try:
                invoke(binary, "ingest", str(source), success=False)
                print("PASS " + name)
            except ValueError as error:
                failures += 1
                print(f"FAIL {name}: {error}")
    print(f"arabic_cases={len(cases) + len(invalid)} failures={failures} result_sha256={digest.hexdigest()}")
    return int(failures != 0)


if __name__ == "__main__":
    raise SystemExit(main())
