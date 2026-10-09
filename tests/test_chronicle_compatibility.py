#!/usr/bin/env python3
"""Real V1 fixture compatibility and immutable receipt-version oracle."""

import argparse
import base64
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile

# Captured from compiled 07c4c2c (typed/mixed) and c30ace4 (local literal).
# These are complete serialized stores, not a second event-extraction engine.
FIXTURES = {
    'typed': {
        "base64": (
            'Q0FTUEVSLUNIUk9OLVYxAJFJuZvrHXva9LGR8d2CbvAZQEJeSUMVsgNX/H9qi3oIjgAAAAAAAABCeshqAAAAAAIA'
            'AABAY2hyb25pY2xlCUFsaWNlCUJPUlJPV0VEX0ZST00JQm9iCTEwMAlTQVIJVDEJQVNTRVJURUQJUE9TSVRJVkUJ'
            'Q09ORklSTUVECkBjaHJvbmljbGUJQWxpY2UJUEFJRF9UTwlCb2IJMTAwCVNBUglUMQlBU1NFUlRFRAlQT1NJVElW'
            'RQlDT05GSVJNRUQK89Wm3m7HUY/RS8ocVDo1UwAzmT+yac9jrwHaU3UZO3ikZ8lPSmNKWaTMzZzKwhLP67S7xYy/'
            '/HcAC24VPGAZIQAAAAAAAAAASQAAAAAAAAABAAAAAAAAAAEAAAAAAAAAxhuz6zcwfmcMyKKCcgRZ5qtYTnqBHtIZ'
            'k9rwhHGccj4BZAAAAAAAAAABAAAAAAAAAAUAAABBbGljZQ0AAABCT1JST1dFRF9GUk9NAwAAAEJvYgMAAABTQVIC'
            'AAAAVDEIAAAAQVNTRVJURUQIAAAAUE9TSVRJVkUJAAAAQ09ORklSTUVETd6H7fCwU8NR00cXZQJtq+h01N+7iGDL'
            '6OmROH1tKuJ/tZc7FGwFn6BzI3HydMTx1GH/D+nWRCAL04nRQsLr90oAAAAAAAAAjQAAAAAAAAACAAAAAAAAAAIA'
            'AAAAAAAApI6C1l2HlP7XreyNshCNPVOJQV/8ncgYbO9qcO66cS0BZAAAAAAAAAABAAAAAAAAAAUAAABBbGljZQcA'
            'AABQQUlEX1RPAwAAAEJvYgMAAABTQVICAAAAVDEIAAAAQVNTRVJURUQIAAAAUE9TSVRJVkUJAAAAQ09ORklSTUVE'
        ),
        "sha256": '860c3c709842851c6585c166d59a2e72bea6c44f2e04c5a348a92c8273077920',
    },
    'mixed': {
        "base64": (
            'Q0FTUEVSLUNIUk9OLVYxAOA3Q0Q+a1A3RxPR5OIPr0erqmpfr15wXNxoYnnsr57fnwAAAAAAAABCeshqAAAAAAIA'
            'AABBbGljZSBvd25zIEJvb2suCkBjaHJvbmljbGUJQWxpY2UJQk9SUk9XRURfRlJPTQlCb2IJMTAwCVNBUglUMQlB'
            'U1NFUlRFRAlQT1NJVElWRQlDT05GSVJNRUQKQGNocm9uaWNsZQlBbGljZQlQQUlEX1RPCUJvYgkxMDAJU0FSCVQx'
            'CUFTU0VSVEVECVBPU0lUSVZFCUNPTkZJUk1FRApeUc5q7B0Ai5iHLMnpAWMEvTUTSX56JH3QOhikCfELX7MfTVM/'
            '9yUIFmwam1H7Kqj23IZALUSZx2hLrZacANQ0EQAAAAAAAABaAAAAAAAAAAIAAAAAAAAAAgAAAAAAAADGG7PrNzB+'
            'ZwzIooJyBFnmq1hOeoEe0hmT2vCEcZxyPgFkAAAAAAAAAAEAAAAAAAAABQAAAEFsaWNlDQAAAEJPUlJPV0VEX0ZS'
            'T00DAAAAQm9iAwAAAFNBUgIAAABUMQgAAABBU1NFUlRFRAgAAABQT1NJVElWRQkAAABDT05GSVJNRUTOnnL4lC6V'
            'wxq8isZOVzAaD0Iu+0FyP6Le3fcFnL6vtj19UdR2s7EeGzJ1GDiSpv//jQf1A5UlsQsFiH/XYxCyWwAAAAAAAACe'
            'AAAAAAAAAAMAAAAAAAAAAwAAAAAAAACkjoLWXYeU/tet7I2yEI09U4lBX/ydyBhs72pw7rpxLQFkAAAAAAAAAAEA'
            'AAAAAAAABQAAAEFsaWNlBwAAAFBBSURfVE8DAAAAQm9iAwAAAFNBUgIAAABUMQgAAABBU1NFUlRFRAgAAABQT1NJ'
            'VElWRQkAAABDT05GSVJNRUQ='
        ),
        "sha256": 'da348bcb364a52bbc428d9a834da7f266b42fbdf9def70cb47508f7e384a294f',
    },
    'local_literal': {
        "base64": (
            'Q0FTUEVSLUNIUk9OLVYxADbFq1tYa6IhPlIp58TSeHgkhJG4PyVYhFS+k6Ecfj3TEQAAAAAAAABueshqAAAAAAEA'
            'AABBbGljZSBvd25zIEJvb2suCppyvv76ZqsPgisHZtdQ8y6+HyvKcc1Nka0Sa+3dR6kmK3wvcilP826ZMKAZtw9S'
            'QtlmHbCCgLBTxZ05d0I5zc0AAAAAAAAAABAAAAAAAAAAAQAAAAAAAAABAAAAAAAAAMXB3a9rvD+uZmzJtTl4kGvy'
            '9Ase45VwlCY4EgYprHafAAAAAAAAAAAAAQAAAAAAAAAFAAAAQWxpY2UEAAAAb3ducwQAAABCb29rAQAAAC0BAAAA'
            'LQcAAABMSVRFUkFMCAAAAFBPU0lUSVZFCAAAAEFTU0VSVEVE'
        ),
        "sha256": 'e3b8559b12b08c76b9c3f4d4e4f1c495c2b61c292413285e15a070b0cf9724c0',
    },
}
LEGACY_QUERY = (
    '{"status":"SUPPORTED","question":"Alice Bob","debt":{"num":100,"den":1,"currency":"SAR"},"settled":{'
    '"num":100,"den":1},"limits":"confirmed exact-currency @chronicle records only; integrity is not trut'
    'h","evidence":[{"event_id":"f3d5a6de6ec7518fd14bca1c543a35530033993fb269cf63af01da5375193b78","evide'
    'nce_span_id":"a467c94f4a634a59a4cccd9ccac212cfebb4bbc58cbffc77000b6e153c601921","document_sha256":"9'
    '149b99beb1d7bdaf4b191f1dd826ef01940425e494315b20357fc7f6a8b7a08","byte_start":0,"byte_end":73,"line_'
    'start":1,"line_end":1,"exact_span_sha256":"c61bb3eb37307e670cc8a282720459e6ab584e7a811ed21993daf0847'
    '19c723e","text":"@chronicle\\tAlice\\tBORROWED_FROM\\tBob\\t100\\tSAR\\tT1\\tASSERTED\\tPOSITIVE\\tCONFIRMED"'
    '},{"event_id":"4dde87edf0b053c351d3471765026dabe874d4dfbb8860cbe8e991387d6d2ae2","evidence_span_id":'
    '"7fb5973b146c059fa0732371f274c4f1d461ff0fe9d644200bd389d142c2ebf7","document_sha256":"9149b99beb1d7b'
    'daf4b191f1dd826ef01940425e494315b20357fc7f6a8b7a08","byte_start":74,"byte_end":141,"line_start":2,"l'
    'ine_end":2,"exact_span_sha256":"a48e82d65d8794fed7adec8db2108d3d5389415ffc9dc8186cef6a70eeba712d","t'
    'ext":"@chronicle\\tAlice\\tPAID_TO\\tBob\\t100\\tSAR\\tT1\\tASSERTED\\tPOSITIVE\\tCONFIRMED"}]}'
)
LEGACY_FIND = (
    '{"status":"MATCHES","query":"Alice","document_sha256":"9149b99beb1d7bdaf4b191f1dd826ef01940425e49431'
    '5b20357fc7f6a8b7a08","total_matching_lines":2,"lexical_only":true,"matches":[{"line":1,"byte_start":'
    '0,"byte_end":73,"matched_terms":1,"exact_span_sha256":"c61bb3eb37307e670cc8a282720459e6ab584e7a811ed'
    '21993daf084719c723e","text":"@chronicle\\tAlice\\tBORROWED_FROM\\tBob\\t100\\tSAR\\tT1\\tASSERTED\\tPOSITIVE'
    '\\tCONFIRMED"},{"line":2,"byte_start":74,"byte_end":141,"matched_terms":1,"exact_span_sha256":"a48e82'
    'd65d8794fed7adec8db2108d3d5389415ffc9dc8186cef6a70eeba712d","text":"@chronicle\\tAlice\\tPAID_TO\\tBob\\'
    't100\\tSAR\\tT1\\tASSERTED\\tPOSITIVE\\tCONFIRMED"}]}'
)
V1_RECEIPT = "CASPER-CHRONICLE-INTEGRITY-RECEIPT-V1"
V2_RECEIPT = "CASPER-CHRONICLE-INTEGRITY-RECEIPT-V2"


def sha(data):
    return hashlib.sha256(data).hexdigest()


def invoke(binary, *args, expected=0):
    result = subprocess.run([binary, *map(str, args)], capture_output=True,
                            text=True, encoding="utf-8", timeout=30)
    if result.returncode != expected:
        raise ValueError(f"expected exit={expected}, got {result.returncode}: {result.stdout!r} {result.stderr!r}")
    if any(marker in result.stderr for marker in ("AddressSanitizer", "runtime error:")):
        raise ValueError(result.stderr)
    return result.stdout


def make_store(folder, fixture="typed"):
    data = base64.b64decode(FIXTURES[fixture]["base64"], validate=True)
    if sha(data) != FIXTURES[fixture]["sha256"]:
        raise ValueError("embedded fixture hash differs")
    source = folder / "source.txt"
    size = struct.unpack_from("<Q", data, 48)[0]
    source.write_bytes(data[68:68 + size])
    store = Path(str(source) + ".chronicle")
    store.write_bytes(data)
    return source, store


def legacy_receipt(store, kind):
    data = store.read_bytes()
    question = {"INGEST": "", "QUERY": "Alice Bob", "FIND": "Alice"}[kind]
    suffix = {"INGEST": ".receipt", "QUERY": f".query-{sha(question.encode())[:16]}.receipt",
              "FIND": f".find-{sha(question.encode())[:16]}.receipt"}[kind]
    result = {"INGEST": data, "QUERY": LEGACY_QUERY.encode(), "FIND": LEGACY_FIND.encode()}[kind]
    receipt = Path(str(store) + suffix)
    receipt.write_text(f"{V1_RECEIPT}\nkind:{kind}\nstore_path_hex:{str(store).encode().hex()}\n"
                       f"question_hex:{question.encode().hex()}\nstore_sha256:{sha(data)}\n"
                       f"document_sha256:{data[16:48].hex()}\nresult_sha256:{sha(result)}\n",
                       encoding="ascii", newline="\n")
    return receipt


def verify_legacy(binary, folder, kind, fixture="typed"):
    _, store = make_store(folder, fixture)
    receipt = legacy_receipt(store, kind)
    original = (store.read_bytes(), receipt.read_bytes())
    expected = 3 if kind == "QUERY" else 0
    output = invoke(binary, "verify", receipt, expected=expected)
    if output.strip() != ("UNSUPPORTED" if kind == "QUERY" else "VALID"):
        raise ValueError("verification status is ambiguous")
    if original != (store.read_bytes(), receipt.read_bytes()):
        raise ValueError("legacy verification changed files")


def reingest_legacy(binary, folder, fixture):
    source, store = make_store(folder, fixture)
    receipt = legacy_receipt(store, "INGEST")
    before = (source.read_bytes(), store.read_bytes(), receipt.read_bytes())
    invoke(binary, "ingest", source)
    if before != (source.read_bytes(), store.read_bytes(), receipt.read_bytes()):
        raise ValueError("legacy reimport changed bytes")


def query_legacy(binary, folder):
    _, store = make_store(folder)
    receipt = legacy_receipt(store, "QUERY")
    before = (store.read_bytes(), receipt.read_bytes())
    output = invoke(binary, "query", store, "Alice Bob")
    result = json.loads(output.splitlines()[0])
    created = Path(next(line[8:] for line in output.splitlines() if line.startswith("receipt=")))
    expected = Path(str(store) + f".query-v2-{sha(b'Alice Bob')[:16]}.receipt")
    if created != expected or result["status"] != "SUPPORTED":
        raise ValueError("new query did not use separate V2 namespace")
    if created.read_text(encoding="ascii").splitlines()[0] != V2_RECEIPT:
        raise ValueError("new query receipt did not declare V2")
    invoke(binary, "verify", created)
    if before != (store.read_bytes(), receipt.read_bytes()):
        raise ValueError("new query changed legacy bytes")


def reject_local_literal(binary, folder):
    source, store = make_store(folder, "local_literal")
    before = (source.read_bytes(), store.read_bytes())
    invoke(binary, "ingest", source, expected=1)
    invoke(binary, "claim", store, "Alice", "owns", "Book", expected=1)
    if before != (source.read_bytes(), store.read_bytes()):
        raise ValueError("unsupported local V1 literal store changed")


def recover_local_literal(binary, folder):
    source, store = make_store(folder, "local_literal")
    before = (source.read_bytes(), store.read_bytes())
    fresh = folder / "separate-v2.txt"
    fresh.write_bytes(source.read_bytes())
    invoke(binary, "ingest", fresh)
    created = Path(str(fresh) + ".chronicle")
    if created.read_bytes()[:16] != b"CASPER-CHRON-V2\0":
        raise ValueError("separate reimport did not create V2")
    output = invoke(binary, "claim", created, "Alice", "owns", "Book")
    if json.loads(output.splitlines()[0])["status"] != "EXACT_STATED":
        raise ValueError("separate reimport lost literal evidence")
    if before != (source.read_bytes(), store.read_bytes()):
        raise ValueError("separate reimport changed legacy files")


def new_store(binary, folder):
    source = folder / "new.txt"
    data = base64.b64decode(FIXTURES["typed"]["base64"])
    source.write_bytes(data[68:68 + struct.unpack_from("<Q", data, 48)[0]])
    output = invoke(binary, "ingest", source)
    store = Path(str(source) + ".chronicle")
    if store.read_bytes()[:16] != b"CASPER-CHRON-V2\0":
        raise ValueError("new store did not declare V2")
    receipt = Path(next(line[8:] for line in output.splitlines() if line.startswith("receipt=")))
    if receipt.read_text(encoding="ascii").splitlines()[0] != V2_RECEIPT:
        raise ValueError("new ingest receipt did not declare V2")
    invoke(binary, "verify", receipt)
    query = invoke(binary, "query", store, "Alice Bob")
    query_receipt = Path(next(line[8:] for line in query.splitlines() if line.startswith("receipt=")))
    if ".query-v2-" in query_receipt.name or ".query-" not in query_receipt.name:
        raise ValueError("unexpected V2-store query namespace")
    invoke(binary, "verify", query_receipt)


def tampered_query(binary, folder, field, replacement, expected=1):
    _, store = make_store(folder)
    receipt = legacy_receipt(store, "QUERY")
    lines = receipt.read_text(encoding="ascii").splitlines()
    receipt.write_text("\n".join(field + replacement if line.startswith(field) else line
                                 for line in lines) + "\n", encoding="ascii", newline="\n")
    output = invoke(binary, "verify", receipt, expected=expected)
    if output.strip() != ("UNSUPPORTED" if expected == 3 else "INVALID"):
        raise ValueError("tampered legacy receipt returned wrong status")


def tampered_legacy_events(binary, folder, omit=False):
    _, store = make_store(folder)
    data = bytearray(store.read_bytes())
    first_event = 68 + struct.unpack_from("<Q", data, 48)[0]
    if omit:
        struct.pack_into("<I", data, 64, 0)
        del data[first_event:]
    else:
        data[first_event] ^= 1
    store.write_bytes(data)
    receipt = legacy_receipt(store, "QUERY")
    if invoke(binary, "verify", receipt, expected=1).strip() != "INVALID":
        raise ValueError("tampered event table bypassed V1 replay validation")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    binary = str(parser.parse_args().binary.resolve())
    cases = [("legacy_ingest", lambda p: verify_legacy(binary, p, "INGEST")),
             ("legacy_find", lambda p: verify_legacy(binary, p, "FIND")),
             ("legacy_query_unsupported", lambda p: verify_legacy(binary, p, "QUERY")),
             ("legacy_mixed_ingest", lambda p: verify_legacy(binary, p, "INGEST", "mixed")),
             ("legacy_reimport", lambda p: reingest_legacy(binary, p, "typed")),
             ("legacy_mixed_reimport", lambda p: reingest_legacy(binary, p, "mixed")),
             ("legacy_query_v2_namespace", lambda p: query_legacy(binary, p)),
             ("local_literal_preserved", lambda p: reject_local_literal(binary, p)),
             ("local_literal_separate_reimport", lambda p: recover_local_literal(binary, p)),
             ("new_v2_store", lambda p: new_store(binary, p)),
             ("forged_legacy_event", lambda p: tampered_legacy_events(binary, p)),
             ("omitted_legacy_events", lambda p: tampered_legacy_events(binary, p, True)),
             ("bad_store_digest", lambda p: tampered_query(binary, p, "store_sha256:", "0" * 64)),
             ("bad_document_digest", lambda p: tampered_query(binary, p, "document_sha256:", "0" * 64)),
             ("malformed_result_digest", lambda p: tampered_query(binary, p, "result_sha256:", "z" * 64)),
             ("legacy_result_unverified", lambda p: tampered_query(binary, p, "result_sha256:", "0" * 64, 3)),
             ("invalid_query_utf8", lambda p: tampered_query(binary, p, "question_hex:", "ff"))]
    failed = 0
    with tempfile.TemporaryDirectory(prefix="chronicle-compat-") as directory:
        for name, case in cases:
            folder = Path(directory) / name
            folder.mkdir()
            try:
                case(folder)
                print(f"PASS {name}")
            except (ValueError, OSError, KeyError, StopIteration, subprocess.TimeoutExpired) as error:
                failed += 1
                print(f"FAIL {name}: {error}")
    print(f"compatibility_cases={len(cases)} failures={failed}")
    return int(failed != 0)


if __name__ == "__main__":
    raise SystemExit(main())
