"""Offline, fail-closed subprocess adapter for the native Chronicle executable."""

import hashlib
import json
import os
from pathlib import Path
import subprocess

PROJECT_ROOT = Path(__file__).resolve().parents[1]
MAX_DOCUMENT_BYTES = 8 * 1024 * 1024
MAX_QUESTION_BYTES = 1024
PROCESS_TIMEOUT_SECONDS = 30


class BridgeError(RuntimeError):
    """An input, native process, or integrity check failed."""


def _rooted_path(value):
    path = Path(value).expanduser()
    return (path if path.is_absolute() else PROJECT_ROOT / path).resolve()


def _text_bytes(text, label, ceiling):
    if not isinstance(text, str) or not text.strip() or "\0" in text:
        raise BridgeError(f"INVALID_INPUT: {label} must contain text without NUL")
    try:
        data = text.encode("utf-8", errors="strict")
    except UnicodeError as error:
        raise BridgeError(f"INVALID_INPUT: {label} is not valid UTF-8") from error
    if len(data) > ceiling:
        raise BridgeError(f"INVALID_INPUT: {label} exceeds {ceiling} UTF-8 bytes")
    return data


def _field(output, name):
    values = [line[len(name) + 1:] for line in output.splitlines()
              if line.startswith(name + "=")]
    if len(values) != 1 or not values[0]:
        raise BridgeError(f"BACKEND_PROTOCOL_ERROR: missing or duplicate {name}")
    return values[0]


class ChronicleBridge:
    """Each request executes the real C engine; no model or network is used."""

    def __init__(self, binary=None, store_root=None):
        name = "casper-chronicle.exe" if os.name == "nt" else "casper-chronicle"
        self.binary = _rooted_path(binary or os.environ.get("CASPER_CHRONICLE_BIN", "build/" + name))
        self.store_root = _rooted_path(store_root or os.environ.get("CASPER_CHRONICLE_STORE", "build/local-chronicle"))

    def _require_backend(self):
        if not self.binary.is_file():
            raise BridgeError(f"BACKEND_UNAVAILABLE: native executable missing: {self.binary}")

    def _execute(self, *arguments):
        self._require_backend()
        try:
            outcome = subprocess.run([str(self.binary), *map(str, arguments)],
                                     cwd=self.binary.parent, capture_output=True,
                                     timeout=PROCESS_TIMEOUT_SECONDS, check=False)
            output = outcome.stdout.decode("utf-8", errors="strict")
            error_text = outcome.stderr.decode("utf-8", errors="strict")
        except subprocess.TimeoutExpired as error:
            raise BridgeError("BACKEND_TIMEOUT: native command exceeded 30 seconds") from error
        except (OSError, UnicodeError) as error:
            raise BridgeError(f"BACKEND_UNAVAILABLE: {error}") from error
        if outcome.returncode != 0:
            detail = (error_text or output).strip()[:2048]
            raise BridgeError(f"BACKEND_ERROR: {arguments[0]} exit={outcome.returncode}: {detail}")
        return output

    def verify(self, receipt):
        output = self._execute("verify", _rooted_path(receipt)).strip()
        if output != "VALID":
            raise BridgeError("BACKEND_PROTOCOL_ERROR: receipt was not reported VALID")
        return output

    def _save_document(self, data):
        digest = hashlib.sha256(data).hexdigest()
        backend_digest = hashlib.sha256(self.binary.read_bytes()).hexdigest()
        folder = self.store_root / backend_digest / digest
        folder.mkdir(parents=True, exist_ok=True)
        source = folder / "source.txt"
        try:
            with source.open("xb") as stream:
                stream.write(data)
        except FileExistsError:
            if source.read_bytes() != data:
                raise BridgeError("SOURCE_INTEGRITY_ERROR: existing source bytes differ")
        return source, digest, backend_digest

    def ingest_text(self, document):
        data = _text_bytes(document, "document", MAX_DOCUMENT_BYTES)
        self._require_backend()
        source, digest, backend_digest = self._save_document(data)
        output = self._execute("ingest", source)
        store = _field(output, "store")
        receipt = _field(output, "receipt")
        self.verify(receipt)
        return {"source_path": str(source), "store_path": store,
                "ingest_receipt_path": receipt, "document_sha256": digest,
                "backend_sha256": backend_digest}

    def run_document(self, document, question, mode="find"):
        if mode not in ("find", "query"):
            raise BridgeError("INVALID_INPUT: mode must be find or query")
        _text_bytes(question, "question", MAX_QUESTION_BYTES)
        imported = self.ingest_text(document)
        output = self._execute(mode, imported["store_path"], question)
        try:
            result = json.loads(output.splitlines()[0])
        except (ValueError, IndexError) as error:
            raise BridgeError("BACKEND_PROTOCOL_ERROR: invalid result JSON") from error
        if not isinstance(result, dict) or not isinstance(result.get("status"), str):
            raise BridgeError("BACKEND_PROTOCOL_ERROR: result status is missing")
        receipt = _field(output, "receipt")
        return {**imported, "operation": mode, "result": result,
                "receipt_path": receipt, "receipt_integrity": self.verify(receipt)}
