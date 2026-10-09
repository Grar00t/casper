# Bounded Arabic prose extraction

Chronicle's version 2 parser recognizes the complete physical-line forms below.
They are ordinary Arabic sentences without `@chronicle` directives. This is a
small explicit grammar, not general Arabic comprehension. Unsupported prose
remains in the original document for `find`; it contributes no financial event.
Missing evidence produces `UNKNOWN` or `PARTIAL`, never an invented payment.

```text
أقر أحمد بأنه اقترض من خالد مبلغ 100 ريال سعودي في اليوم الأول.
اقترض أحمد من خالد مبلغ 100 ريال سعودي في اليوم الأول.
أكد خالد أن سعود وسيط عنه في اليوم الثاني.
نفى خالد أن سعود وسيط عنه في اليوم الثاني.
أكد سعود استلام مبلغ 40 ريال سعودي من أحمد في اليوم الثاني.
نفى سعود استلام مبلغ 40 ريال سعودي من أحمد في اليوم الثاني.
حوّل أحمد إلى سعود مبلغ 60 ريال سعودي في اليوم الثالث ولم يثبت وصول الحوالة.
```

The first two forms create `BORROWED_FROM` document assertions. The intermediary
forms create positive or negative `INTERMEDIARY_FOR`. The receipt forms create positive or negative
`PAID_TO` assertions. The transfer form creates an `UNCERTAIN`, `PENDING`
`TRANSFER_TO`; it does not establish receipt. `CONFIRMED` on the other forms is
the narrow reasoner's input classification for an explicit document assertion,
not independent confirmation that the reported event happened. The negative
receipt contradicts a positive receipt with the same parties, amount, currency,
and day; both original spans belong in the resulting `CONFLICT` evidence.
An intermediary denial likewise prevents counting a payment through a
contradicted link and retains the conflicting source spans.

Named parties can contain multiple words, such as `أحمد بن سالم`. Names are exact
UTF-8 bytes; similar names, diacritics, and spelling variants are not merged.
Only letters, Arabic combining marks, and single internal ASCII spaces are
accepted. Grammar and negation words cannot be names. The field ceiling is 127
UTF-8 bytes; crossing it rejects ingestion instead of truncating a name.

Amounts are nonnegative ASCII or Arabic-Indic (`٠١٢٣٤٥٦٧٨٩`) integers or fractions
such as `١/٣`. No binary floating-point is used. A zero denominator, malformed
digit, or signed 64-bit overflow rejects a recognized monetary sentence.
Decimals, signs, thousands separators, and written-out amounts are unsupported.
Currency is explicit: `ريال سعودي` or `SAR` maps to `SAR`; `دولار أمريكي` or `USD`
maps to `USD`. Bare `ريال` and `دولار` are unsupported. Currencies are never added
together or converted.

The time field is required and limited to `اليوم الأول` through `اليوم العاشر`.
This retains a day label for duplicate-payment checks; it does not resolve a
calendar date. An exact repeated receipt is not counted twice. Separate debts
between the same parties require disambiguation and currently yield `UNKNOWN`.

Every recognized sentence occupies one physical line and ends in one ASCII
period. Prefixes such as `لم`, `ربما`, `إذا`, `قال شاهد:`, and quotation marks,
and suffix clauses outside the exact forms above are unsupported. The parser
does not infer pronoun antecedents, merge multiline sentences, interpret reported
speech, or establish that a general intermediary has legal authority. It uses
the existing narrow intermediary relation solely under the Chronicle contract.

The parser changes no original bytes. Evidence includes the whole recognized
line, including a negative or uncertain qualifier, and its byte offsets, line
number, document SHA-256, and exact-span SHA-256. CRLF line endings stay in the
stored document. Store versioning and old-store verification are defined in
`CHRONICLE_CONTRACT.md`; changing extraction must not silently reinterpret V1.

`tests/fixtures/chronicle_ahmed_prose_ar.txt` is a hand-authored regression story,
not a held-out language benchmark. Its relevant sentences are separated by
unstructured narrative: debt 100 SAR, confirmed receipt 40 SAR through Saud, and
unconfirmed transfer 60 SAR. The expected assessment is `PARTIAL`, with four
original evidence spans. `tests/test_chronicle_arabic.py` checks independent
rational arithmetic and exact source bytes and challenges negation, uncertainty,
currency, names, duplicate records, missing links, limits, and distant evidence.
These fixtures establish this grammar's behavior, not arbitrary-story accuracy.
