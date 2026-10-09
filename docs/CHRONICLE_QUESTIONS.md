# Exact debt-settlement query forms

`casper-chronicle query STORE "QUESTION"` selects a borrower and lender only
when the entire query matches one of the forms below. The selected debt is
then evaluated against its evidence. A supported form does not guarantee a
settlement: missing links, uncertain payments, competing debts, and conflicts
still affect the result.

| Form | Example |
| --- | --- |
| `BORROWER LENDER` | `أحمد خالد` |
| `BORROWER LENDER?` or `BORROWER LENDER؟` | `أحمد خالد؟` |
| `،BORROWER LENDER؟` | `،أحمد خالد؟` |
| `Did BORROWER repay LENDER?` | `Did Alice repay Bob?` |
| `هل سدد BORROWER دين LENDER؟` | `هل سدد أحمد دين خالد؟` |
| `ما حالة سداد دين BORROWER إلى LENDER؟` | `ما حالة سداد دين أحمد إلى خالد؟` |

The two Arabic sentence forms also accept a final ASCII `?`. Matching is
case-sensitive and byte-exact. Spaces and names are not normalized; diacritics
are not removed. Multiword names are supported: `هل سدد أحمد بن سالم دين خالد
بن عمر؟` selects those complete names. It does not select `أحمد` or `خالد`.
Borrower and lender roles cannot be reversed by rearranging the query.

Extra prefixes, suffixes, multiple questions, unsupported wording, and negated
questions produce `UNKNOWN` without selecting debt evidence. For example,
`هل سدد أحمد سالم دين خالد؟` does not match a borrower named only `أحمد`.
`Are Alice and Bob astronauts?` does not ask the supported settlement question.

## Exact selector for names containing syntax

Names in the ordinary forms must not contain the query's reserved grammar or
negation words, syntax punctuation, control bytes, leading/trailing spaces, or
doubled spaces. These names remain usable through an explicit selector:

```text
@settlement<TAB>BORROWER<TAB>LENDER
```

`<TAB>` means an actual tab character. This is a selector for two identities,
not natural-language question parsing. It preserves the exact identity bytes,
including multiword names such as `أحمد دين سالم` that contain a grammar word.
It rejects missing or extra fields and trailing question text. In PowerShell:

```powershell
$selector = "@settlement`tأحمد دين سالم`tخالد"
.\build\casper-chronicle.exe query .\story.txt.chronicle $selector
```

If a bare pair could match two distinct stored borrower/lender pairs, the
existing competing-debt rule returns `UNKNOWN`. Use the tab-separated selector
to choose the exact role boundaries. Ordinary forms are deliberately bounded;
they do not provide general question answering, implication, or coreference.

## Acceptance oracle

```sh
python tests/test_chronicle_questions.py build/casper-chronicle
```

The test checks exact full names, unknown and known longer names, reversed
roles, language forms, grammar words inside names, unrelated and negated
questions, prefix/suffix rejection, field limits, preserved question bytes,
selected evidence counts, and receipt replay. It prints a deterministic hash
of query JSON bytes in addition to the failure count.
