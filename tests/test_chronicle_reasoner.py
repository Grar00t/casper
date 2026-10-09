#!/usr/bin/env python3
"""External CLI oracle for the bounded Chronicle debt-settlement contract."""

import argparse
import hashlib
import json
import pathlib
import subprocess
import tempfile


def event(subject="Alice", predicate="BORROWED_FROM", target="Bob", amount="100",
          currency="SAR", time="T1", modality="ASSERTED", polarity="POSITIVE",
          status="CONFIRMED"):
    return "\t".join(("@chronicle", subject, predicate, target, amount, currency,
                      time, modality, polarity, status)) + "\n"


def payment(**kwargs):
    return event(predicate="PAID_TO", time="T2", **kwargs)


def link(**kwargs):
    return event(subject="Carl", predicate="INTERMEDIARY_FOR", amount="-", **kwargs)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=pathlib.Path)
    args = parser.parse_args()
    binary = str(args.binary.resolve())
    cases = []

    def case(name, text, status, settled=0, evidence=None, question="Did Alice repay Bob?",
             assessment=False):
        cases.append((name, text, status, settled, evidence, question, assessment))

    debt = event()
    full = payment()
    case("direct", debt + full, "SUPPORTED", 100, 2)
    case("currency_mismatch", debt + payment(currency="USD"), "UNKNOWN", 0)
    case("currency_absent", debt + payment(currency="-"), "UNKNOWN", 0)
    case("debt_currency_absent", event(currency="-") + full, "UNKNOWN", 0)
    case("pending_debt", event(status="PENDING") + full, "UNKNOWN", 0)
    case("uncertain_debt", event(modality="UNCERTAIN") + full, "UNKNOWN", 0)
    case("pending_payment", debt + payment(status="PENDING"), "UNKNOWN", 0)
    case("uncertain_payment", debt + payment(modality="UNCERTAIN"), "UNKNOWN", 0)
    case("pending_link", debt + link(status="PENDING") + payment(target="Carl"), "UNKNOWN", 0)
    case("uncertain_link", debt + link(modality="UNCERTAIN") + payment(target="Carl"), "UNKNOWN", 0)
    case("confirmed_link", debt + link() + payment(target="Carl"), "SUPPORTED", 100, 3)
    case("ambiguous_debt_amount", debt + event(amount="200") + full, "UNKNOWN", 0, 2)
    case("ambiguous_debt_time", debt + event(time="T0") + full, "UNKNOWN", 0, 2)
    case("duplicate_debt", debt + debt + full, "SUPPORTED", 100, 3)
    case("duplicate_payment", debt + payment(amount="50") * 2, "PARTIAL", 50, 3)
    case("payment_transfer_duplicate", debt + payment(amount="50") +
         event(predicate="TRANSFER_TO", amount="50", time="T2"), "PARTIAL", 50, 3)
    case("distinct_payment_time", debt + payment(amount="50") +
         event(predicate="PAID_TO", amount="50", time="T3"), "SUPPORTED", 100, 3)
    case("debt_conflict_evidence", debt + event(polarity="NEGATIVE"), "CONFLICT", 0, 2)
    case("payment_conflict_evidence", debt + full + payment(polarity="NEGATIVE"), "CONFLICT", 0, 3)
    case("link_conflict_evidence", debt + link() + link(polarity="NEGATIVE") +
         payment(target="Carl"), "CONFLICT", 0, 4)
    case("irrelevant_payment_conflict", debt + full + payment(target="Dan") +
         payment(target="Dan", polarity="NEGATIVE"), "SUPPORTED", 100)
    case("zero_debt_no_payment", event(amount="0"), "UNKNOWN", 0)
    arabic = event(subject="أحمد", target="خالد") + payment(subject="أحمد", target="خالد")
    case("utf8_left_boundary", arabic, "SUPPORTED", 100, 2, "،أحمد خالد؟")
    case("utf8_similar_name", arabic, "UNKNOWN", 0, 0, "أحمدان خالد؟")
    case("utf8_combining_suffix", arabic, "UNKNOWN", 0, 0, "أحمدُ خالد؟")
    case("negative_question_scope", debt + full, "SUPPORTED", 100, 2,
         "Did Alice not repay Bob?", True)
    case("nondebt_question_scope", debt + full, "SUPPORTED", 100, 2,
         "Are Alice and Bob astronauts?", True)

    failed = 0
    result_hash = hashlib.sha256()
    with tempfile.TemporaryDirectory(prefix="chronicle-reasoner-") as directory:
        root = pathlib.Path(directory)
        for name, text, status, settled, evidence, question, assessment in cases:
            try:
                source = root / (name + ".txt")
                source.write_bytes(text.encode("utf-8"))
                ingest = subprocess.run([binary, "ingest", str(source)], text=True, encoding="utf-8",
                                        capture_output=True, check=True)
                store = next(line[6:] for line in ingest.stdout.splitlines() if line.startswith("store="))
                query = subprocess.run([binary, "query", store, question], text=True, encoding="utf-8",
                                       capture_output=True, check=True)
                result = json.loads(query.stdout.splitlines()[0])
                errors = []
                if result["status"] != status:
                    errors.append(f"status={result['status']} expected={status}")
                amount = result["settled"]
                if amount["num"] != settled * amount["den"]:
                    errors.append(f"settled={amount} expected={settled}")
                if evidence is not None and len(result["evidence"]) != evidence:
                    errors.append(f"evidence={len(result['evidence'])} expected={evidence}")
                if assessment and result.get("assessment") != "DEBT_SETTLEMENT":
                    errors.append("missing explicit DEBT_SETTLEMENT assessment scope")
                for item in result["evidence"]:
                    span = source.read_bytes()[item["byte_start"]:item["byte_end"]]
                    if hashlib.sha256(span).hexdigest() != item["exact_span_sha256"]:
                        errors.append("evidence span hash mismatch")
                if errors:
                    raise ValueError("; ".join(errors))
                result_hash.update(query.stdout.splitlines()[0].encode("utf-8"))
                print(f"PASS {name}")
            except (ValueError, KeyError, StopIteration, subprocess.CalledProcessError) as error:
                failed += 1
                print(f"FAIL {name}: {error}")
                if isinstance(error, subprocess.CalledProcessError):
                    print(error.stderr)
    print(f"reasoner_cases={len(cases)} failures={failed} result_sha256={result_hash.hexdigest()}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
