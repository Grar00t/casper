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

- SUPPORTED: one confirmed debt is selected, confirmed payments in its
  exact currency equal the debt, and no related payment ambiguity remains.
- PARTIAL: confirmed eligible payments exceed zero but remain below the
  selected debt; pending or unrelated evidence cannot be silently counted.
- CONFLICT: opposite-polarity records describe the same claim at the same
  time, rational aggregation overflows, or confirmed eligible payments
  exceed the selected debt.
- UNKNOWN: zero or multiple matching confirmed debts, missing or unconfirmed
  intermediary/receipt links, no eligible payment, or unresolved ambiguity
  when the summed known payments already equal the debt.

PAID and FRAUD are not output states. A transfer counts only when CONFIRMED.
A payment to a third party counts only when an explicit positive, CONFIRMED,
and uncontradicted INTERMEDIARY_FOR event links that exact party to the lender.
Neither ASSERTED nor PENDING amounts settle a debt, even if their polarity is
POSITIVE. Payments in currencies other than the selected debt currency do not
reduce the displayed settled amount. Exact borrower/lender matches that select
multiple confirmed debts are UNKNOWN, never the first encountered debt.

Repeated payments with the same payer, receiver, predicate, amount, currency,
time expression, polarity and confirmation status have no unique transaction
identifier. Chronicle counts at most one and marks the remainder unresolved
instead of silently double-counting. Separate times are distinct records.
A fully covered sum accompanied by unresolved duplicate/payment claims is
UNKNOWN instead of asserting full settlement.

`CONFIRMED` is only a status *supplied by the input document*, not a
separate check that a payment occurred in the world.

Every used reasoning step is returned with its event ID and exact evidence span.

## Integrity receipt

CASPER-CHRONICLE-INTEGRITY-RECEIPT-V1 binds the store bytes, document hash,
question bytes for queries, and canonical result bytes. verify reopens the
store, validates document and span hashes, reruns the deterministic query, and
compares the result hash.

On loading the binary store, Chronicle checks source hashes, exact event
span offsets and line numbers, re-parses each event from its original UTF-8
source line, and rejects mismatching cached event fields, IDs, and spans.
Incomplete or malformed lines starting with a recognized `@chronicle`
directive fail ingestion rather than disappearing as ordinary prose.

The receipt is an integrity checksum, not a signature, provenance guarantee,
truth certificate, legal conclusion, or authenticity proof. A malicious author
can intentionally provide a false but internally consistent CONFIRMED claim;
integrity and factual truth are distinct.

## Resource limits

Fields are limited to 127 bytes and event count to 100,000. Initial ingestion
hashes, validates, extracts, and copies the source in streaming passes; memory
is bounded by the longest input line plus extracted events. Query loading is
currently proportional to store size. The million-word benchmark reports
observed wall time, tracked input allocation, and process peak RSS where the
platform exposes it, without a pass/fail performance threshold.
