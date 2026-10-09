# Chronicle store and receipt compatibility

New imports write `CASPER-CHRON-V2` in the existing 16-byte magic field. The
remaining binary layout keeps the same field widths and offsets. New receipts
declare `CASPER-CHRONICLE-INTEGRITY-RECEIPT-V2`. A version identifies the parser
and result contract; matching byte layouts alone do not imply matching meaning.

## Published V1 stores

The V1 profile from commit `07c4c2cc6363dd411908b7e835100f1093dd91b6` extracts
explicit `@chronicle` records only. Loading that profile replays the typed
parser across the original source and compares the complete event table.
Ordinary prose remains ignored even if the V2 parser would recognize it.
Changed, omitted, duplicated, or invented event records are rejected.

| Operation on a valid typed V1 store | Behavior |
| --- | --- |
| Verify an original INGEST receipt | Verify the original store/document hashes. |
| Verify an original FIND receipt | Replay retrieval against the preserved source bytes. |
| Import the same source again | Reuse the V1 store and INGEST receipt without changing their bytes. |
| Run a new query | Evaluate with the current query rules; write a V2 receipt under `.query-v2-<hash>.receipt`. |
| Verify an original V1 QUERY receipt | Print `UNSUPPORTED`, exit `3`, and leave all files unchanged. |

The separate V2 query suffix avoids colliding with an immutable V1 query
receipt for the same question. Queries on V2 stores retain the ordinary
`.query-<hash>.receipt` suffix. There is no automatic in-place migration.
Current source-size, event-count, field, and memory limits also apply when
opening a legacy store.

## Why an old query receipt can be unsupported

V1 query receipts contain a result digest but do not contain the result JSON
or an evaluator version. Result changes include the `assessment` field,
borrower/lender identities, explanatory limits, and evaluation rules.
For example, the published evaluator could report `SUPPORTED` for a
confirmed-status debt with uncertain modality, or a zero-valued debt without
a payment. The current evaluator returns `UNKNOWN` for those fixtures.
Removing new JSON fields cannot reproduce every historical evaluation.

Before returning exit `3`, verification validates the receipt structure and
hex encoding, the referenced store hash, the document hash, and the complete
V1 typed event table. Malformed digests, invalid UTF-8 questions, and corrupt
store/document evidence produce `INVALID`, exit `1`.

`UNSUPPORTED` means that the historical result is **not verified**. A changed
but syntactically valid historical result digest also remains unsupported;
there is no original result available for comparison. Exit `3` never grants
the `VALID` result reserved for exit `0`. Keep the old receipt for provenance
and run a new query to obtain a receipt under the current contract.

## Unpublished V1 literal stores

The local profile at `c30ace4b7ec36922cc67538bd588b1f57fb13808` also used V1
magic while extracting literal three-field sentences. Its event table can
therefore differ from the published V1 parser for identical source bytes.
These stores are rejected when their literal events do not match the typed
V1 profile. Reimport and query failures preserve the existing source and store;
the application does not relabel them as V2 or rewrite their event tables.

For these stores, retain the originals, copy the original source bytes to a
separate unused filename, and ingest that copy. This creates an independent
V2 store with new receipts. The compatibility test checks that a literal claim
can be recovered this way while the original files remain byte-identical.

## Independent compatibility oracle

```sh
python tests/test_chronicle_compatibility.py build/casper-chronicle
```

The test embeds actual serialized stores captured from the two commits above,
with SHA-256 checks on fixture bytes. Historical query and retrieval JSON are
captured outputs, not a second implementation of the old evaluator. Receipt
paths are constructed for the test directory; historical store, document,
and result bytes remain fixed. Cases cover receipt versions, immutable
reimport, mixed prose/typed V1 data, separate query namespaces, rejected local
literal profiles, recovery to a separate V2 file, event-table tampering,
digest tampering, and unsupported historical results.

This contract covers the declared parser profiles and limits. Receipt
verification establishes reproducibility and byte integrity; it does not
establish real-world truth or general understanding of a question.
