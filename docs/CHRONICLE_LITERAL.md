# Chronicle bounded literal retrieval

The `claim` command retrieves exact three-token statements from an ingested
UTF-8 document. It runs offline and uses the same evidence store and integrity
receipt verifier as the typed debt/payment path.

## CLI

Create `statements.txt` with these exact lines:

```text
Alice owns Book.
أحمد يملك كتاب.
```

Build and run:

```sh
bash scripts/build.sh --arch generic
./build/casper-chronicle ingest statements.txt
./build/casper-chronicle claim statements.txt.chronicle Alice owns Book
./build/casper-chronicle claim statements.txt.chronicle أحمد يملك كتاب
./build/casper-chronicle claim statements.txt.chronicle Alice owns Pen
```

The first two claims return `EXACT_STATED`; the last returns `UNRESOLVED`.
Each successful invocation prints one JSON object followed by a
`receipt=PATH` line. Pass that exact path to:

```sh
./build/casper-chronicle verify PATH
```

The C API continues to use `casper_chronicle_query`. Its reserved literal
question encoding is `@claim\tSUBJECT\tPREDICATE\tOBJECT`, where `\t` denotes
one actual tab byte. All three fields must be present. Tabs and control bytes
inside a field are rejected. Free returned JSON and receipt-path buffers with
`casper_chronicle_free`, including results from earlier calls no longer needed.

## Accepted input grammar

A supported statement contains exactly three nonempty tokens separated by
ASCII spaces or tabs, followed by one terminal ASCII period (`.`). Leading and
trailing spaces/tabs are accepted. One statement occupies one physical line;
LF and CRLF input retain their original bytes. UTF-8 is validated, but text is
not normalized, case-folded, translated, or stemmed.

ASCII `. , ; : ! ? @`, Arabic comma/semicolon/question mark, and the supported
Unicode sentence separators are rejected inside tokens. Quotes and backslashes
are literal identity bytes and are escaped in JSON. Multiple sentences, missing
terminal periods, empty tokens, and statements with additional tokens remain
stored source text but create no literal event. A matching three-token statement
whose field exceeds the byte limit fails ingestion instead of being truncated.

Supported literals receive `modality=LITERAL`, `polarity=POSITIVE`,
`status=ASSERTED`, no amount, and absent currency/time (`-`). Explicit
`@chronicle` records remain on the typed event path. Setting a typed record's
modality to `LITERAL` does not make that record eligible for literal retrieval.

## Results and evidence

The JSON fields are:

| Field | Meaning |
| --- | --- |
| `status` | `EXACT_STATED` if at least one exact stored literal matches; otherwise `UNRESOLVED` |
| `assessment` | Always `LITERAL_RETRIEVAL` |
| `question` | The exact reserved question encoding |
| `subject`, `predicate`, `object` | The exact requested fields |
| `limits` | The bounded retrieval capability statement |
| `evidence` | Every matching source occurrence, in source order |

All three fields must match byte-for-byte. A subject prefix, a similar predicate,
or a different object does not match. Each evidence item contains event and span
IDs, document SHA-256, half-open byte offsets, one-based line offsets, exact span
text, and its SHA-256. Duplicate statements retain their separate evidence
occurrences. Whitespace is excluded from token identities but remains in the
evidence text and hashes.

The receipt binds the store, question, and canonical result bytes. Verification
reopens the store, validates the document and spans, reruns the same literal
query, and compares the result hash. It is an integrity checksum, not a signature
or authenticity guarantee.

`EXACT_STATED` establishes that the supported statement occurs in the source.
It does not establish factual truth, semantic entailment, contradiction freedom,
or the intended meaning of arbitrary prose. `UNRESOLVED` does not establish that
the statement is false. This path performs no deduction, transitive inference,
pronoun resolution, implicit relationship extraction, or multiword-entity
parsing. The separate typed reasoner assesses its documented debt/payment
contract and does not consume literals as financial evidence.

## Capacity and ownership

- Each subject, predicate, and object is limited to 127 UTF-8 bytes, excluding
  its terminating NUL; this is a byte limit, not a character limit.
- Source documents are limited to 8 MiB (8,388,608 bytes).
- Literal and typed events share a combined limit of 4,096 events.
- Chronicle allocations share one process-local 16 MiB (16,777,216-byte) static
  pool. Source bytes, event arrays, temporary buffers, and live returned handles
  consume that pool together. Pool exhaustion returns an error; the individual
  ceilings do not guarantee that every combination fits simultaneously.
- API calls must be serial. Concurrent calls are unsupported. Outputs remain
  live until released through `casper_chronicle_free`; the caller must not use
  the C library `free` on them.

## Reproducible acceptance check

```sh
python3 tests/test_chronicle_literal.py build/casper-chronicle
```

The oracle checks exact fields, similar-name rejection, duplicate evidence,
Arabic UTF-8, JSON escaping, source bytes and independent SHA-256 values,
receipt tampering, grammar exclusions, and field limits. It prints the case
count, failure count, and a hash of result JSON bytes. This hash excludes
temporary paths and import timestamps, so separate runs and compiler builds
can compare deterministic query output.

## Windows command line

The native Windows entry point converts UTF-16 command-line arguments to UTF-8;
the build script adds `-municode` for that target. Chronicle file operations and
receipt hashing convert these UTF-8 paths back to UTF-16 at the filesystem
boundary. Arabic and supplementary Unicode directory/file names are supported
within the existing CLI, receipt, and operating-system path limits. Source
bytes and the UTF-8 path bytes recorded in receipts are preserved without
normalization. Native checks cover import, claim/find/query, UTF-8 stdout,
repeat import, independent evidence hashes, and receipt tampering:

```powershell
python tests/test_chronicle_windows.py build/casper-chronicle.exe
```
