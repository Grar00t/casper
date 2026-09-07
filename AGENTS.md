# Casper Repository Rules

This file describes the repository as it exists on the current working branch. Do not infer features from old documentation or deleted scripts. On `integrate-teacher-training` the teacher JSONL generator and Python SFT trainer are present; they are not on `main` at `195aca04`.

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
.\scripts\niyah.ps1 train C:\path\to\teacher.jsonl -Device cuda
.\scripts\niyah.ps1 train-c Data_Training\sovereign_knowledge.txt
```

C build/run and `train-c` require `bash` plus GCC or Clang. Full-model `train` runs natively through Python/PyTorch and does not require Bash.

## Implemented Components

| Path | Responsibility |
|---|---|
| `Core_CPP/niyah_core.c` | neural allocation, forward path, sampling, persistence, legacy output-head adaptation |
| `Core_CPP/niyah_train_full.c` | deterministic initialization and full-parameter detached-KV/truncated-BPTT C training |
| `Core_CPP/niyah_train.c` | native C training executable |
| `tools/train_casper.py` | full-sequence PyTorch/CUDA SFT, checkpoint/resume, native `.bin` export |
| `tools/generate_teacher_dataset.py` | local OpenAI-compatible teacher JSONL generator; intended supervision is final `response` only |
| `Core_CPP/hybrid_reasoner.c` | symbolic terms, unification, clause solving |
| `Core_CPP/constraint_solver.c` | rational constraint operations and propagation |
| `Core_CPP/rule_parser.c` | `.nrule` parser and verification |
| `Core_CPP/proof_generator.c` | SHA-256 and proof file generation/verification |
| `Core_CPP/khz_q_svd.c` | numerical output gate |
| `Core_CPP/casper_rag.c` | search transport, result parsing/ranking, trace/context hashing |
| `Core_CPP/casper_cli.c` | Casper query/proof CLI |
| `Core_CPP/niyah_hybrid_main.c` | hybrid command-line entry point |
| `tokenizer.c` | deterministic UTF-8 tokenizer with byte fallback |

## Runtime and Training Dependencies

The non-RAG C runtime links against the C runtime and `libm`.

RAG transport is platform-specific:

- Windows: WinHTTP.
- POSIX: the `curl` executable is invoked by `casper_rag.c`.

PyTorch is training-only for `tools/train_casper.py`; exported `.bin` inference does not depend on PyTorch. The Node and WPF directories are optional runtimes with separate dependencies.

## Verification Rules

1. Read the current source before modifying it.
2. Do not reference deleted scripts or binaries outside `build/`.
3. C changes must compile with the warning gate in `scripts/build.sh`.
4. Run `bash scripts/build.sh --arch generic --smoke` after C changes.
5. Use the debug sanitizer build for memory-sensitive C changes.
6. Do not claim a fixed smoke-test count. Treat process exit status/current output as evidence.
7. Do not claim network availability from configuration alone.
8. Do not claim a feature is implemented because it appears in documentation.
9. Keep proof-format, model-format, tokenizer-contract, and public ABI changes explicit.
10. Do not commit credentials, deployment addresses, private infrastructure details, or machine-specific paths.
11. Do not describe the C trainer as exact full-sequence BPTT: its KV gradient boundary is intentionally truncated.
12. Do not describe the Python training path as verified on CUDA unless it has actually run with CUDA on the target environment.

## Tokenizer Contract

Historical ids `0..1499` are preserved. Vocabulary v2 appends byte fallback ids `1500..1755` for raw UTF-8 bytes. Old checkpoints were not trained on these appended ids and should not be assumed to use them meaningfully.

## Arithmetic and Determinism

- Constraint rationals use integer numerator/denominator representation.
- Cross-multiplication uses `__int128` where provided; the portability fallback is not exact for every int64 value.
- Determinism claims apply only to paths excluding network responses, wall-clock data, unfixed random seeds, nondeterministic GPU kernels, and other external state.

## CI

`.github/workflows/ci.yml` checks:

- C core with GCC and Clang using generic architecture, release smoke, and debug sanitizer smoke.
- Python training-tool syntax (`tools/train_casper.py` and `tools/generate_teacher_dataset.py`).
- Node source syntax after `npm ci`.
- Windows WPF build with .NET 9.

A change is not verified until the relevant current workflow and local/reproducible checks pass.
