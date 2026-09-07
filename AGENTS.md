# Casper Repository Rules

This file describes the repository as it exists on `main`. Do not infer features from old documentation or deleted scripts.

## Build

Linux/macOS or a Windows shell with `bash` and GCC/Clang:

```bash
bash scripts/build.sh --arch generic
bash scripts/build.sh --arch generic --smoke
bash scripts/build.sh --debug --arch generic --smoke
bash scripts/build.sh --arch generic --bench
```

Build artifacts are written under `build/`.

PowerShell wrapper:

```powershell
.\scripts\niyah.ps1 build
.\scripts\niyah.ps1 smoke
.\scripts\niyah.ps1 bench
.\scripts\niyah.ps1 train
```

The PowerShell wrapper calls the same `scripts/build.sh` entry point and therefore requires `bash` plus GCC or Clang.

## Implemented C Components

| Path | Responsibility |
|---|---|
| `Core_CPP/niyah_core.c` | neural model allocation, forward path, sampling, optimizer, model save/load |
| `Core_CPP/hybrid_reasoner.c` | symbolic terms, unification, clause solving |
| `Core_CPP/constraint_solver.c` | rational constraint operations and propagation |
| `Core_CPP/rule_parser.c` | `.nrule` parser and verification |
| `Core_CPP/proof_generator.c` | SHA-256 and proof file generation/verification |
| `Core_CPP/khz_q_svd.c` | numerical output gate |
| `Core_CPP/casper_rag.c` | search transport, result parsing/ranking, trace/context hashing |
| `Core_CPP/casper_cli.c` | Casper query/proof CLI |
| `Core_CPP/niyah_hybrid_main.c` | hybrid command-line entry point |
| `tokenizer.c` | tokenizer implementation |

## Runtime Dependencies

The non-RAG C core links against the C runtime and `libm`.

RAG transport is platform-specific:

- Windows: WinHTTP.
- POSIX: the `curl` executable is invoked by `casper_rag.c`; RAG is therefore not dependency-free on Linux/macOS.

The Node and WPF directories are separate optional runtimes and have their own package/runtime dependencies.

## Verification Rules

1. Read the current source before modifying it.
2. Do not reference deleted scripts or binaries outside `build/`.
3. C changes must compile with the warning gate in `scripts/build.sh`.
4. Run `bash scripts/build.sh --arch generic --smoke` after C changes.
5. Use the debug sanitizer build for memory-sensitive C changes.
6. Do not claim a fixed smoke-test count. Treat the process exit status and current output as the evidence.
7. Do not claim network availability from configuration alone.
8. Do not claim a feature is implemented because it appears in documentation.
9. Keep proof-format, model-format, and public ABI changes explicit and versioned.
10. Do not commit credentials, deployment addresses, private infrastructure details, or machine-specific paths.

## Arithmetic and Determinism

- Constraint rationals use integer numerator/denominator representation.
- Cross-multiplication uses `__int128` where the compiler provides it; the current portability fallback in `constraint_solver.c` is not exact for every int64 value. Do not describe that fallback as exact.
- Determinism claims apply only to code paths that exclude network responses, wall-clock data, random seeds not fixed by the caller, and other external state.

## CI

`.github/workflows/ci.yml` checks:

- C core with GCC and Clang using generic architecture plus smoke execution.
- Node source syntax after `npm ci`.
- Windows WPF build with .NET 9.

A change is not verified until the relevant current workflow and local/reproducible checks pass.
