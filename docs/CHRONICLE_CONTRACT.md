# Casper Chronicle v1 Contract

## Scope

Chronicle v1 is an offline evidence store and a narrow debt/payment reasoner.
It preserves the exact UTF-8 input bytes and consumes explicit event records.
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

Free text may surround records and is preserved, but it creates no event.
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
3. an exact borrower/lender pair named in the question;
4. only stored events whose evidence spans re-hash correctly.

The current adapter does not invoke the repository's generic
hybrid_reasoner.c, constraint_solver.c, KHZ_Q, or .nrule subsystem.
Amounts use checked integer rational arithmetic inside Chronicle. Integrating a
generic solver later requires a new versioned mapping from these typed records
to solver variables and must preserve the outcomes below.

## Outputs

- SUPPORTED: confirmed/eligible payments equal the stated debt exactly.
- PARTIAL: confirmed/eligible payments are greater than zero and less than
  the stated debt.
- CONFLICT: opposite-polarity duplicate records exist, rational aggregation
  overflows, or eligible payments exceed the debt.
- UNKNOWN: the debt cannot be selected exactly, no eligible payment is
  established, or an intermediary/receipt link is missing.

PAID and FRAUD are not output states. A transfer counts only when CONFIRMED.
A payment to a third party counts only when an explicit positive
INTERMEDIARY_FOR event links that exact party to the lender.

Every used reasoning step is returned with its event ID and exact evidence span.

## Integrity receipt

CASPER-CHRONICLE-INTEGRITY-RECEIPT-V1 binds the store bytes, document hash,
question bytes for queries, and canonical result bytes. verify reopens the
store, validates document and span hashes, reruns the deterministic query, and
compares the result hash.

The receipt is an integrity checksum, not a signature, provenance guarantee,
truth certificate, legal conclusion, or authenticity proof.

## Resource limits

Fields are limited to 127 bytes and event count to 100,000. Initial ingestion
hashes, validates, extracts, and copies the source in streaming passes; memory
is bounded by the longest input line plus extracted events. Query loading is
currently proportional to store size. The million-word benchmark reports
observed wall time, tracked input allocation, and process peak RSS where the
platform exposes it, without a pass/fail performance threshold.
