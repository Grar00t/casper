# Chronicle validation — 2026-10-09

These are local measurements for the V2 continuation of the Chronicle MVP.
They are not a claim about arbitrary Arabic comprehension or a neural model.
The checked source-file hashes and gate-log digests are recorded in
[`validation/chronicle-20261009.json`](validation/chronicle-20261009.json).
GitHub CI is a separate check of the PR's exact head; local success alone does
not establish remote CI success.

## Reproduce

```sh
python3 scripts/test_chronicle.py --compiler gcc
python3 scripts/test_chronicle.py --compiler clang
bash scripts/build.sh --arch generic --smoke
bash scripts/build.sh --debug --arch generic --smoke
```

On native Windows with MinGW Clang/GCC and native Python:

```powershell
python scripts/test_chronicle.py --compiler clang --native-windows
```

The runner records every command's exit status and log SHA-256 in its output
directory. Sanitizer output is treated as failure even if the process returns
zero. Python child-process output is explicitly UTF-8 on Windows.

## Measured gates

| Environment | Gates | Result |
| --- | --- | --- |
| Ubuntu WSL, GCC 15.2.0 | O2 and O0 ASan/UBSan; 52 commands | PASS |
| Ubuntu WSL, Clang 21.1.8 | O2 and O0 ASan/UBSan; 52 commands | PASS |
| Native Windows, LLVM-MinGW Clang 22.1.8 | O2 and O0; 46 commands | PASS |
| GCC and Clang full repository builds | release/debug smoke and Casper self-check | exit 0 in all four runs |
| Valgrind 3.26.0, GCC API and pool executables | 95 API checks; 396 allocator checks | zero errors; zero live bytes at exit |
| Existing Node runtime | five tests | all passed |
| Existing WPF UI | Release build | zero warnings, zero errors |

The acceptance suites measured 102 integrity checks, 27 debt cases, 25 literal
cases, 32 preservation checks, 114 resource cases, 38 Arabic cases, 17 historical
compatibility cases, 44 query-shape cases, and 15 real native-bridge cases.
Windows additionally exercised four Unicode path cases. These are observed
counts for this revision, not a permanent advertised smoke-test count.

The reasoner, literal, Arabic and query suites ran three times per build mode.
Their result hashes agreed across all three compilers and both optimization
levels. The question hash was
`8855617191dfb8efe7c145e642559030bac4167d7ae56e8ca1f28aa09b6906b2`;
the Arabic hash was
`92914a9c043b134cf9fc9e1c6927cb2bc8ff1924b2a5eb5874168d1c21464d00`.

Regression oracles first reproduced failures: longer/negated/unrelated questions
failed 21 of 44 cases before complete-question selection; the initial native
Windows path implementation failed three of four Unicode path cases. The
64 KiB read-size oracle rejected the prior single large read, then passed both
ingestion and verification after chunking. A negative intermediary sentence
previously allowed settlement; its regression now requires `CONFLICT`.

## Synthetic million-word measurement

The corpus contains exactly 1,000,000 whitespace-delimited words, 5,000,096
bytes, and four explicit typed records separated by filler. Its SHA-256 is
`2e3eb278eb4dbaecd57451a9691840315fad057434fcfb05b3e27bccbd2cd663`.
All three builds returned `PARTIAL`, counted 40 of 100 SAR, and verified the
result receipt. Each row is one observed run, not a median or service guarantee.

| O2 build | Ingest seconds | Query seconds | Verify seconds | Process peak RSS KiB |
| --- | ---: | ---: | ---: | ---: |
| GCC 15.2.0 | 0.137951135 | 0.097553124 | 0.072685118 | 6824 |
| Clang 21.1.8 | 0.103266316 | 0.056692119 | 0.059084224 | 6952 |
| Windows Clang 22.1.8 | 0.117572400 | 0.065103400 | 0.079261000 | not measured |

Measured arena high-water usage was 5,015,664 bytes in every run, out of a
16,777,216-byte arena; all arena allocations were released. RSS includes the
whole process, while arena usage measures only Chronicle's own allocator.
Input is read in at most 64 KiB chunks but retained in memory. This benchmark
does not measure general extraction from a million words of Arabic prose.

## Local interface checks

The actual local app was exercised in the browser: keyboard submission returned
`PARTIAL` with 100 SAR debt and 40 SAR counted; independent receipt verification
returned `VALID`; a negative intermediary produced `CONFLICT`; an unknown longer
name produced `UNKNOWN` without debt evidence; empty input produced an explicit
error and cleared the old evidence. Technical JSON is collapsed initially.

At a 320 px viewport, client and scroll widths both measured 305 px; the status
control's full text fitted its 79 px client height. Primary and radio targets
measured at least 44 px high. The primary text/background colors measured white
on `rgb(29,78,216)`, contrast 6.70:1. Temporary CSS zoom of 2 also showed no
horizontal overflow; this was a CSS reflow check, not a claim about every
browser's zoom implementation. Normal zoom and viewport were restored.
Tab order reached the document, question, operation selectors, submit,
technical-details toggle, receipt download and verification button.

## Acceptance and limits

The ordinary Arabic fixture returns `PARTIAL`: the 100 SAR debt and intermediary
link are explicit; a 40 SAR receipt counts; an unconfirmed 60 SAR transfer does
not. Evidence retains all four original source sentences and their independent
hash/coordinate checks. Missing links, contradictions, similar full names,
changed bytes, omitted events, and receipt tampering have separate regressions.

This MVP provides CLI ingestion, source retrieval, bounded literal/Arabic
extraction, narrow debt settlement, and replayable integrity receipts. Its local
UI executes that native path and reports actual failures. It does not establish
real-world truth, infer arbitrary pronouns or implied events, answer unrestricted
questions, perform chronology/causal reasoning, or certify payment or fraud.
No learned extractor or model-quality benchmark was run.

Source size is capped at 8 MiB; the event ceiling is 4,096 and fields are 127
UTF-8 bytes. The ceilings share one arena and are not simultaneously guaranteed.
Calls are serial. ASan does not instrument internal arena allocation boundaries;
allocator and heap-wrap tests address additional failure cases. The baseline
uses no network or model download. Historical V1 query receipts explicitly
return `UNSUPPORTED`, never a false `VALID`, as detailed in the compatibility
contract.
