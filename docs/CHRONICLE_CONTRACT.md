# Casper Chronicle v2 Contract

## Scope

Chronicle v2 is an offline evidence store and a narrow debt/payment reasoner.
It preserves the exact UTF-8 input bytes and consumes explicit event records
and complete Arabic sentences under [CHRONICLE_ARABIC.md](CHRONICLE_ARABIC.md).
The separate bounded literal retrieval path accepts `Subject Predicate Object.`
on one line; see [CHRONICLE_LITERAL.md](CHRONICLE_LITERAL.md).
It does not claim to understand unrestricted prose, establish factual truth,
detect fraud, eliminate hallucinations, or achieve a preset accuracy.

No network, SaaS service, model download, KHZ_Q score, .nrule rule, or neural
model participates in the baseline path. A future neural extractor may propose
records only through a separate adapter; proposed records remain unverified
until they have explicit source spans and pass this contract.

## Input record

An event is one UTF-8 line with nine tab-separated fields:

    @chronicle SUBJECT PREDICATE OBJECT AMOUNT CURRENCY TIME MODALITY POLARITY STATUS

The spaces above represent tab characters.

- PREDICATE: BORROWED_FROM, PAID_TO, INTERMEDIARY_FOR, or TRANSFER_TO.
- AMOUNT: a non-negative integer or rational numerator/denominator; a hyphen
  means absent. Floating-point values are rejected.
- POLARITY: POSITIVE or NEGATIVE.
- STATUS: ASSERTED, CONFIRMED, PENDING, or DENIED.
- Entity identity is exact UTF-8 text. Similar names are not merged.

Other free text may surround records and is preserved without creating an event.
Malformed @chronicle records fail ingestion instead of being silently ignored.

## Stored data and identity

The binary store contains the original document bytes, SHA-256, byte size,
import time, UTF-8 encoding contract, and typed events. Import time is metadata
and is excluded from document, event, and span identities.

Each event has a deterministic event_id, subject, predicate, object, optional
rational amount, optional currency, time expression, modality, polarity,
status, and one evidence_span_id. Each span binds the document SHA-256,
half-open byte offsets, one-based line offsets, exact bytes, and their SHA-256.

Re-ingesting unchanged bytes reuses the existing valid store byte-for-byte.

## Reasoner inputs

The reasoner receives:

1. one validated Chronicle store;
2. one UTF-8 question;
3. an exact borrower/lender pair selected by a complete supported query form;
4. only stored events whose evidence spans re-hash correctly.

The current adapter does not invoke the repository's generic
hybrid_reasoner.c, constraint_solver.c, KHZ_Q, or .nrule subsystem.
Amounts use checked integer rational arithmetic inside Chronicle. Integrating a
generic solver later requires a new versioned mapping from these typed records
to solver variables and must preserve the outcomes below.

## Outputs

- SUPPORTED: one explicitly CONFIRMED debt is selected, and distinct CONFIRMED
  eligible payments in the same currency equal the stated debt without unresolved
  duplicate-payment ambiguity.
- PARTIAL: confirmed/eligible payments are greater than zero and less than
  the stated debt.
- CONFLICT: relevant, established opposite-polarity records exist, rational aggregation
  overflows, or eligible payments exceed the debt.
- UNKNOWN: no single explicitly CONFIRMED debt is selected, no eligible payment
  is established, an intermediary/receipt link is missing, or an otherwise
  fully settled total contains unresolved related or duplicate payments.

PAID and FRAUD are not output states. A transfer counts only when CONFIRMED.
A payment to a third party counts only when an explicit positive
INTERMEDIARY_FOR event links that exact party to the lender. If that link has
an established opposite-polarity claim, the attempted payment and both link
records are retained as conflict evidence; the payment is not counted as settled.
Debt, payment and intermediary records require `ASSERTED` modality and
`CONFIRMED` status to establish a settlement (this is document-supplied status,
not an independent check of payment). Pending, DENIED and merely ASSERTED
statuses cannot establish a settlement. Currency must be explicit and identical.
Multiple matching confirmed debts with different tuples yield `UNKNOWN`.
Identical confirmed debt tuples are treated as repeated evidence of the same
claim (with each source event preserved), not as proof of separate obligations;
no transaction identifier is available to establish real-world uniqueness.
Payment records with identical
subject, object, rational amount, currency and time count at most once, even when
the same event is described as both PAID_TO and TRANSFER_TO. If such a duplicate
coexists with a fully covered total, the result is UNKNOWN rather than
certifying complete settlement. Distinct but otherwise
identical payments need different time fields; transaction IDs and chronology
reasoning are not implemented.

The entire question must match [CHRONICLE_QUESTIONS.md](CHRONICLE_QUESTIONS.md),
including full names and their borrower/lender roles. An unsupported, negated,
or unrelated question returns `UNKNOWN` without selecting debt evidence.
The question selects an exact entity pair. The JSON `assessment` value is
`DEBT_SETTLEMENT`: it does not answer arbitrary or negated propositions. A zero
debt without an established payment returns `UNKNOWN`.

Every used reasoning step is returned with its event ID and exact evidence span.

## Lexical source retrieval

The `find STORE "TERMS"` command searches *ordinary original UTF-8 text*,
whether or not the document contains `@chronicle` records. It is a deterministic
lexical search, **not** event extraction, entity linking, causal reasoning, or
a language model. It matches byte-exact query terms split at ASCII whitespace
and basic ASCII punctuation; it does not stem Arabic or strip Arabic punctuation.

- Query length: 1–1024 UTF-8 bytes, maximum 16 query terms.
- Returns up to eight source lines, sorted by matched-term count (then source order).
- Each line includes exact text, half-open byte offsets, one-based line number,
  exact-span SHA-256, matched-term count, and the source document SHA-256.
- The `FIND` receipt replays the search and checks byte/result integrity.
- The current query path loads the whole Chronicle store into memory. This
  is not yet a low-memory streaming query engine.

## Integrity receipt

CASPER-CHRONICLE-INTEGRITY-RECEIPT-V2 binds the store bytes, document hash,
question bytes for queries, and canonical result bytes. verify reopens the
store, reparses all original source records, compares the complete canonical
event set and coordinates, validates document and span hashes, reruns the query, and
compares the result hash.

New stores use `CASPER-CHRON-V2`. Published V1 stores retain their original
typed-record parser; they are never silently reinterpreted with the V2 grammar.
Old INGEST and FIND receipts remain verifiable. Historical V1 QUERY results
cannot be replayed under the changed evaluator and return `UNSUPPORTED`, exit
3, after store/source integrity checks. Only exit 0 means `VALID`; malformed
or corrupt input returns `INVALID`, exit 1. New queries on a V1 store use a
separate V2 receipt filename. See
[CHRONICLE_COMPATIBILITY.md](CHRONICLE_COMPATIBILITY.md) for exact profiles,
the unpublished local V1 literal format, and recovery without overwriting.

The receipt is an integrity checksum, not a signature, provenance guarantee,
truth certificate, legal conclusion, or authenticity proof.
Receipt fields are unique and bounded; unknown, duplicate, oversized, missing
or unterminated fields are rejected. Paths and questions are limited to 2047
UTF-8 bytes. Existing stores or receipts are never overwritten: identical valid
content is reused; differing bytes cause failure. Use a new input filename to
import a revised document. An I/O or receipt collision can leave a newly created
store for inspection; ingestion is not a multi-file transaction.

## Resource limits

Fields are limited to 127 UTF-8 bytes, source to 8 MiB, and event count to 4096.
File payloads are read in chunks of at most 64 KiB. The whole source is still
retained in the arena for extraction and queries: chunked I/O does not remove
the source ceiling or make memory use independent of document size.
All Chronicle-managed dynamic storage uses one aligned static 16 MiB `_pool`:
source bytes, typed events, working data and returned output. The ceilings apply
together; allocation can fail before an individual ceiling if live outputs fill
the arena. Calls are serial, and successful outputs must be released with
`casper_chronicle_free`. Stack space, static constants and C runtime stdio buffers
are outside the arena. There are no direct application heap calls on the tested
ingest/query/verify paths; this is not a claim about libc's internal allocations.
SHA-256 uses the existing streaming implementation, including full test vectors.

`python3 scripts/test_chronicle.py --compiler gcc` (or `clang`) runs strict C11
O2 and O0 ASan/UBSan gates, adversarial store/receipt tests, exact literal and
debt tests, bounded Arabic sentences and questions, legacy compatibility,
preservation and resource boundaries, allocator tests, an API test intercepting
direct heap calls, and a read-size oracle. It compares deterministic result hashes
across three runs and both optimization levels. ASan cannot distinguish the
internal allocation boundaries of the static arena; dedicated allocator tests
exercise alignment, overflow, exhaustion, reuse and coalescing.

Native Windows gates use `--native-windows` with MinGW GCC/Clang and native
Python. They run O2/O0 acceptance and Unicode path tests. This runner does not
claim native sanitizer or heap-wrap coverage; the Linux jobs execute those gates.

The benchmark generates exactly 1,000,000 whitespace-delimited words containing
four debt/payment records separated by filler, and checks a 40/100 PARTIAL result.
It reports wall time (including file I/O), end-to-end query/receipt verification
time, actual arena high-water bytes and source/receipt hashes. The runner records
whole-process peak RSS separately. It is a synthetic long-distance evidence
test, not a general prose extraction or dense-event throughput benchmark, and
sets no performance threshold. Self-check and benchmark refuse existing scratch
filenames before creating any test artifacts.
