#!/usr/bin/env python3
"""Offline UI adapter oracle: real native subprocess, hashes and failure states."""

import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
BINARY = None


class LocalBridgeTests(unittest.TestCase):
    def setUp(self):
        from casper_local_bridge import ChronicleBridge
        self.workspace = tempfile.TemporaryDirectory(prefix="casper-local-")
        self.addCleanup(self.workspace.cleanup)
        self.folder = Path(self.workspace.name) / "سجل عربي 🧾"
        self.bridge = ChronicleBridge(binary=BINARY, store_root=self.folder)

    def test_original_arabic_bytes_and_hashes_are_preserved(self):
        document = "مقدمة عربية.\r\nأحمد يملك كتاب.\r\nخالد يملك قلم.\n"
        result = self.bridge.run_document(document, "أحمد", "find")
        self.assertEqual(result["result"]["status"], "MATCHES")
        self.assertEqual(result["receipt_integrity"], "VALID")
        raw = document.encode("utf-8")
        self.assertEqual(Path(result["source_path"]).read_bytes(), raw)
        self.assertEqual(result["document_sha256"], hashlib.sha256(raw).hexdigest())
        for match in result["result"]["matches"]:
            span = raw[match["byte_start"]:match["byte_end"]]
            self.assertEqual(match["text"].encode("utf-8"), span)
            self.assertEqual(match["exact_span_sha256"], hashlib.sha256(span).hexdigest())

    def test_typed_query_keeps_uncertain_payment_partial(self):
        document = (ROOT / "tests/fixtures/chronicle_ahmed_ar.txt").read_text(encoding="utf-8")
        result = self.bridge.run_document(document, "أحمد خالد", "query")
        self.assertEqual(result["result"]["status"], "PARTIAL")
        self.assertEqual(result["receipt_integrity"], "VALID")

    def test_bounded_arabic_story_keeps_pending_transfer_partial(self):
        document = (ROOT / "tests/fixtures/chronicle_ahmed_prose_ar.txt").read_text(encoding="utf-8")
        result = self.bridge.run_document(document, "أحمد خالد", "query")
        self.assertEqual(result["result"]["status"], "PARTIAL")
        self.assertEqual(result["receipt_integrity"], "VALID")
        for item in result["result"]["evidence"]:
            raw = document.encode("utf-8")[item["byte_start"]:item["byte_end"]]
            self.assertEqual(item["text"].encode("utf-8"), raw)
            self.assertEqual(item["exact_span_sha256"], hashlib.sha256(raw).hexdigest())

    def test_unstructured_prose_does_not_claim_settlement(self):
        result = self.bridge.run_document("ربما دفع أحمد دين خالد.", "أحمد خالد", "query")
        self.assertEqual(result["result"]["status"], "UNKNOWN")

    def test_missing_native_backend_fails_closed(self):
        from casper_local_bridge import BridgeError, ChronicleBridge
        bridge = ChronicleBridge(binary=self.folder / "missing.exe", store_root=self.folder)
        with self.assertRaisesRegex(BridgeError, "BACKEND_UNAVAILABLE"):
            bridge.run_document("أحمد يملك كتاب.", "أحمد", "find")

    def test_malformed_document_cannot_produce_success(self):
        from casper_local_bridge import BridgeError
        with self.assertRaisesRegex(BridgeError, "BACKEND_ERROR"):
            self.bridge.run_document("@chronicle\tbroken", "broken", "find")

    def test_changed_receipt_is_rejected(self):
        from casper_local_bridge import BridgeError
        result = self.bridge.run_document("أحمد يملك كتاب.", "أحمد", "find")
        receipt = Path(result["receipt_path"])
        receipt.write_bytes(receipt.read_bytes() + b"unknown:tampered\n")
        with self.assertRaisesRegex(BridgeError, "BACKEND_ERROR"):
            self.bridge.verify(receipt)

    def test_different_cwd_does_not_change_binary_or_source(self):
        original = Path.cwd()
        try:
            os.chdir(self.workspace.name)
            result = self.bridge.run_document("أحمد يملك كتاب.", "أحمد", "find")
            self.assertEqual(result["result"]["status"], "MATCHES")
        finally:
            os.chdir(original)

    def test_repeat_input_reuses_exact_store_bytes(self):
        first = self.bridge.run_document("أحمد يملك كتاب.", "أحمد", "find")
        stored = Path(first["store_path"]).read_bytes()
        second = self.bridge.run_document("أحمد يملك كتاب.", "أحمد", "find")
        self.assertEqual(first, second)
        self.assertEqual(Path(second["store_path"]).read_bytes(), stored)

    def test_previous_backend_store_is_preserved_during_upgrade(self):
        document = "أحمد يملك كتاب."
        raw = document.encode("utf-8")
        legacy = self.folder / hashlib.sha256(raw).hexdigest()
        legacy.mkdir(parents=True)
        (legacy / "source.txt").write_bytes(raw)
        previous_store = legacy / "source.txt.chronicle"
        previous_store.write_bytes(b"previous incompatible store retained as evidence")
        snapshot = previous_store.read_bytes()
        result = self.bridge.run_document(document, "أحمد", "find")
        self.assertEqual(result["result"]["status"], "MATCHES")
        self.assertEqual(previous_store.read_bytes(), snapshot)
        self.assertNotEqual(Path(result["store_path"]), previous_store)
        self.assertEqual(result["backend_sha256"], hashlib.sha256(BINARY.read_bytes()).hexdigest())

    def test_invalid_arguments_rejected_before_subprocess(self):
        from casper_local_bridge import BridgeError
        cases = [("", "x", "find"), ("x", "", "find"),
                 ("x", "x", "invent"), ("x\0y", "x", "find")]
        for document, question, mode in cases:
            with self.subTest(mode=mode, document=document):
                with self.assertRaises(BridgeError):
                    self.bridge.run_document(document, question, mode)

    def test_ui_error_state_does_not_fabricate_success(self):
        from casper_local_app import evaluate_document
        from casper_local_bridge import ChronicleBridge
        missing = ChronicleBridge(binary=self.folder / "missing.exe", store_root=self.folder)
        summary, payload, receipt = evaluate_document(missing, "نص", "نص", "find")
        self.assertEqual(payload["status"], "ERROR")
        self.assertEqual(payload["receipt_integrity"], "NOT_VERIFIED")
        self.assertIn("BACKEND_UNAVAILABLE", summary)
        self.assertIsNone(receipt)

    def test_ui_presents_amounts_and_exact_source_without_json_traversal(self):
        from casper_local_app import present_result
        document = (ROOT / "tests/fixtures/chronicle_ahmed_prose_ar.txt").read_text(encoding="utf-8")
        payload = self.bridge.run_document(document, "أحمد خالد", "query")
        account, evidence = present_result(payload)
        self.assertIn("الدين: 100 SAR", account)
        self.assertIn("السداد المحتسب: 40 SAR", account)
        self.assertIn("أحمد", account)
        for item in payload["result"]["evidence"]:
            self.assertIn(item["text"], evidence)
            self.assertIn(f"السطر {item['line_start']}", evidence)

    def test_ui_unknown_does_not_present_zero_as_established_debt(self):
        from casper_local_app import present_result
        payload = self.bridge.run_document("ربما دفع أحمد دين خالد.", "أحمد خالد", "query")
        account, evidence = present_result(payload)
        self.assertNotIn("الدين: 0", account)
        self.assertIn("لم يحدد", account)

    def test_app_import_has_no_launch_or_workspace_side_effect(self):
        code = ("import sys; sys.path.insert(0, sys.argv[1]); "
                "import casper_local_app; "
                "assert callable(casper_local_app.build_app); print('IMPORT_SAFE')")
        outcome = subprocess.run([sys.executable, "-c", code, str(ROOT / "tools")],
                                 cwd=self.workspace.name, capture_output=True,
                                 timeout=15, text=True)
        self.assertEqual(outcome.returncode, 0, outcome.stderr)
        self.assertEqual(outcome.stdout.strip(), "IMPORT_SAFE")
        self.assertEqual(list(Path(self.workspace.name).iterdir()), [])


def main():
    global BINARY
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    BINARY = parser.parse_args().binary.resolve()
    result = unittest.TextTestRunner(verbosity=2).run(
        unittest.defaultTestLoader.loadTestsFromTestCase(LocalBridgeTests))
    print(f"local_bridge_cases={result.testsRun} failures={len(result.failures)} errors={len(result.errors)}")
    return int(not result.wasSuccessful())


if __name__ == "__main__":
    raise SystemExit(main())
