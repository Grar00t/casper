# Chronicle boundary hardening — 2026-10-09

This audit starts from `9706ec58dcbecf89e53b5692fa15a0ed4c71a3d5`.
PR #15 and continuation PR #17 were already merged when this work began.
The continuation branch is retained for a separate review. The changes preserve
the C API, event encoding, V1 compatibility policy, and bounded V2 grammar.
Historical validation documents and local evidence remain unchanged.

## Reproduced defects

| Boundary | Before repair | After repair |
| --- | --- | --- |
| Length-prefixed event text | All eight fields accepted one or two appended NUL bytes; recomputed outer receipt hashes concealed the noncanonical serialization. 32 failures in 474 checks. | Validate the declared bytes as UTF-8 without NUL before terminating the C string. 474 checks, zero failures. |
| Path encoding | POSIX accepted malformed UTF-8 paths at five API/replay entry points. | Reject malformed UTF-8 before file access. Exact valid Unicode identities remain unchanged. |
| Serialized path capacity | An overlong input path created a store that could not be represented in a receipt. | Reserve the store suffix and reject the path before creating output. Exact 2047-byte store paths and their longer receipt filenames remain supported where the filesystem permits. |
| Failed reads | Source and receipt short-read paths attempted another read after failure. Two injected cases failed. | Close the stream without the extra read; three I/O cases, zero failures, zero arena bytes retained. |

The path/scalar/punctuation suite reports 86 cases with six failures on the
baseline, then zero failures on both Linux compiler configurations. Windows
runs 71 portable cases; the POSIX malformed-byte and long-path cases are skipped.
The tests require normal rejection exit codes and reject sanitizer diagnostics;
a crash cannot satisfy a rejection assertion.

## Executed gates

The adjacent [machine-readable receipt](validation/chronicle-hardening-20261009.json)
contains commands, exits, source SHA-256 values, log SHA-256 values, and actual
before/after output. `<WORKTREE>`, `<SCRATCH>`, and `<PYTHON>` denote the checkout,
a fresh temporary working directory, and the native Python interpreter.

```sh
python3 scripts/test_chronicle.py --compiler gcc --output build/verify-gcc
python3 scripts/test_chronicle.py --compiler clang --output build/verify-clang
python scripts/test_chronicle.py --compiler clang --native-windows --output build/verify-windows

bash scripts/build.sh --compiler gcc --arch generic --smoke
bash scripts/build.sh --compiler gcc --debug --arch generic --smoke
bash scripts/build.sh --compiler clang --arch generic --smoke
bash scripts/build.sh --compiler clang --debug --arch generic --smoke
# After each build:
./build/casper --self-check
```

| Local gate | Result |
| --- | --- |
| GCC 15.2, O2 and O0 ASan/UBSan | 64 commands, all exit 0 |
| Clang 21.1, O2 and O0 ASan/UBSan | 64 commands, all exit 0 |
| Native Windows Clang 22.1, O2/O0 | 50 commands, all exit 0 |
| Full C engine, both compilers, release/debug | Four smoke builds and four Casper self-checks, all exit 0 |

Full-engine builds ran in fresh source snapshots whose file hashes were checked
against this checkout, so earlier build artifacts were not overwritten.
The GCC full release build emits its existing serial-LTO scheduling warning;
the strict Chronicle compilations emit no compiler warnings.

The allocation fault oracle executes nine operations, including debt and literal
queries and receipt replay. Each Linux build reports
`allocation_fault_checks=238 injections=58 failures=0 arena_live_bytes=0`.
Every injected allocation failure rejects the operation, transfers no output,
and recovers for the subsequent successful invocation.

The existing reasoner, literal, Arabic and question results have identical hashes
across three repetitions, both optimization modes, and the three platforms/compiler
configurations. The new Unicode result hash also agrees across those configurations.
Import timestamps remain metadata; this is not a claim that newly created store
files at different times have identical whole-file hashes.

Each runner also executes the existing million-word synthetic benchmark:
1,000,000 words, 5,000,096 source bytes, four events, `PARTIAL`, settled `40/100`,
verified receipt, and 5,015,664 arena high-water bytes. This measures a sparse
synthetic fixture, not unrestricted prose comprehension or dense-event throughput.

The four new regression suites are invoked by the existing CI workflow through
`scripts/test_chronicle.py`. Remote CI must be checked against the final PR head;
local results and historical PR #17 CI are not evidence of that remote result.

To reproduce the negative oracle, compile the Chronicle sources from the initial
commit in a separate directory, then run the current
`tests/test_chronicle_store_hardening.py` and
`tests/test_chronicle_utf8_hardening.py` against that executable. Each must exit 1.
The I/O regression requires the wrapper linker flags recorded in the receipt.

## Static findings and untested behavior

```sh
clang --analyze -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
  -ICore_CPP -Xanalyzer -analyzer-output=text \
  Core_CPP/casper_chronicle.c Core_CPP/chronicle_pool.c Core_CPP/proof_generator.c
```

This command exited 0 and produced no diagnostics in Chronicle or its allocator.
Six existing `unix.Stream` diagnostics remain in unchanged `proof_generator.c`
at its hash loop and older proof readers (lines 72, 150, 178). They concern
continued reads after possible stream errors/EOF; they are static findings,
not demonstrated runtime failures or a clean static-analysis claim.

Native Windows sanitizers and linker-wrapper fault injection were not executed;
Linux covers those checks. ARM and macOS execution, concurrent modification
between separate hash/load opens, crash durability, and arbitrary Arabic
understanding are not verified. Stores remain capped at 8 MiB and 4096 events,
with one serial 16 MiB arena. ASan does not instrument internal arena boundaries.
No model weights, training artifacts, or historical receipts were changed.
