# Casper / NIYAH

Casper is a C11 codebase containing a small neural runtime/trainer, symbolic reasoning, rational constraints, `.nrule` verification, SHA-256 integrity receipts, and a web-retrieval CLI. The repository also contains optional Node.js and Windows WPF interfaces.

## Build the C Runtime

Requirements:

- GCC or Clang
- `bash`
- `libm`
- `curl` only when using RAG on Linux/macOS

```bash
git clone https://github.com/Grar00t/casper.git
cd casper
bash scripts/build.sh --arch generic
```

Build and run the current C smoke checks:

```bash
bash scripts/build.sh --arch generic --smoke
```

Debug build with AddressSanitizer/UBSan when supported:

```bash
bash scripts/build.sh --debug --arch generic --smoke
```

Artifacts are written to `build/`:

```text
build/niyah
build/trainer
build/niyah_hybrid
build/casper
build/bench_niyah
build/tokenizer_test
```

The build uses C11 plus warnings-as-errors. `--arch generic` avoids host-specific `-march=native` output.

## PowerShell Wrapper

`scripts/niyah.ps1` calls the same Bash build path, so it requires a Windows environment with `bash` and GCC/Clang available.

```powershell
.\scripts\niyah.ps1 build
.\scripts\niyah.ps1 smoke
.\scripts\niyah.ps1 bench
.\scripts\niyah.ps1 train
```

## Components

| Path | Implemented role |
|---|---|
| `Core_CPP/niyah_core.c` | model allocation, inference, sampling, persistence, legacy output-head adaptation |
| `Core_CPP/niyah_train_full.c` | deterministic initialization and full-parameter truncated-BPTT training |
| `Core_CPP/niyah_train.c` | training executable |
| `Core_CPP/hybrid_reasoner.c` | terms, Robinson unification, occurs-check, backward chaining |
| `Core_CPP/constraint_solver.c` | rational constraints and propagation |
| `Core_CPP/rule_parser.c` | deterministic `.nrule` text matching and replacement/rejection |
| `Core_CPP/proof_generator.c` | SHA-256 integrity receipt generation/verification |
| `Core_CPP/khz_q_svd.c` | numerical text-shape/coherence heuristic |
| `Core_CPP/casper_rag.c` | HTTP search transport, parsing, ranking, trace/context hashing |
| `Core_CPP/casper_cli.c` | query/integrity-receipt CLI |
| `Core_CPP/niyah_hybrid_main.c` | hybrid CLI and C11 audit bridge |
| `tokenizer.c` | tokenizer |
| `niyah_engine_local/` | optional Node.js runtime |
| `UI_CSharp/` | optional Windows WPF UI |

## Security and Semantic Boundaries

The repository uses some historical names such as `KHZ_Q`, `penalty_nasl`, and "coherence". These identifiers must not be interpreted as evidence of factual truth, ethics, policy compliance, or formal correctness.

- Casper/NIYAH is a native user-space program. It is not a Ring-0 kernel component and is not bare-metal software.
- `NiyahModel` uses a contiguous pool for neural weights, KV cache, layer metadata, and scratch space. Other subsystems still use ordinary heap allocation where required. Single-pool model allocation is not a memory-safety proof.
- `khz_q_svd.c` is a numerical heuristic over byte-frequency buckets and retained spectral energy. It does not understand meaning or prove that output is true or ethical.
- `.nrule` is a deterministic text-rule filter (`CONTAINS`, equality, replacement, rejection). It is separate from the Prolog-like symbolic reasoner.
- `hybrid_reasoner.c` and `constraint_solver.c` are implemented subsystems, but the current neural generation path does not invoke them as final authorities. Wiring them into generation requires an explicit semantic contract rather than an implicit claim.
- "Sovereign" is a deployment/design objective (local execution, minimized dependencies), not a formally verified security property.
- SIMD paths can change floating-point reduction order. Identical seeds do not by themselves establish bit-identical logits across compilers or architectures.

## Casper Query CLI

The standalone Casper CLI accepts a query as its first argument:

```bash
./build/casper "example query"
```

Select the retrieval backend with `CASPER_BACKEND`:

```bash
CASPER_BACKEND=ddg ./build/casper "example query"
CASPER_BACKEND=bing ./build/casper "example query"
SEARXNG_HOST=search.example.test CASPER_BACKEND=searxng ./build/casper "example query"
```

`SEARXNG_HOST` is the host value consumed by the current transport. The current C transport constructs HTTPS requests; a plain-HTTP SearXNG instance is not supported by this interface as currently implemented.

On Windows the C RAG path uses WinHTTP. On POSIX it invokes the `curl` executable. Network-backed results are not deterministic because remote content and availability can change.

## Hybrid CLI and Audit Bridge

Current supported entry points include:

```bash
./build/niyah_hybrid --smoke
./build/niyah_hybrid --rag
./build/niyah_hybrid --model model.bin
./build/niyah_hybrid --interactive
```

The Node runtime uses a bounded stdin JSON audit contract:

```bash
printf '%s' '{"prompt":"hello","text":"candidate answer","rules":"Data_Training/safety.nrule"}' \
  | ./build/niyah_hybrid --audit-stdin
```

The audit response reports the KHZ_Q numerical score, `.nrule` result, and a V2 integrity hash. `verified=true` means those local checks passed; the response explicitly reports `factual_truth_verified=false` because this path does not establish factual correctness.

`--rag` currently uses the backend wired by `niyah_hybrid_main.c`; do not assume the standalone Casper CLI backend selection syntax applies to this command.

## Training

```bash
./build/trainer Data_Training/sovereign_knowledge.txt 3 0.001 0.0001
```

The trainer derives `vocab_size` from the live tokenizer, applies deterministic non-zero initialization, and updates token embeddings, all attention and FFN projections, RMSNorm scales, and the LM head. A successful run writes `niyah_trained.bin`.

The training algorithm uses a deliberate detached-KV boundary: each position backpropagates through its current Q/K/V path, but future losses do not propagate into earlier cached K/V states. This is full-parameter truncated backpropagation, not exact full-sequence BPTT. The C self-check includes an overfit regression that requires loss reduction and a change in an attention backbone matrix.

An optional fifth argument supplies the deterministic initialization seed:

```bash
./build/trainer Data_Training/sovereign_knowledge.txt 3 0.001 0.0001 0x434153504552
```

## Integrity Receipt Verification

New receipts use `NIYAH-PROOF-V2`. The receipt binds:

- SHA-256 of the exact prompt bytes,
- SHA-256 of the exact output bytes,
- SHA-256 of the exact applied rule-file bytes, or SHA-256 of an empty rule set.

Prompt and output payloads are stored as hex in the receipt so multiline text is unambiguous.

For a receipt with no rules:

```bash
./build/casper --verify response.proof
```

For a rules-bound receipt, supply the rule file whose current bytes must match the bound hash:

```bash
./build/casper --verify response.proof Data_Training/safety.nrule
```

The CLI separates `receipt_valid`, `rules_bound`, `rules_verified`, and final `valid`. A receipt is an integrity checksum/receipt. It is not a digital signature, authenticity proof, factual-truth proof, external-source attestation, model-quality certificate, or legal/compliance determination.

## Constraint Arithmetic

Constraint values use integer numerator/denominator representation. Where available, comparisons use `__int128` cross-multiplication. The portability fallback on targets without `__int128` uses floating-point comparison and therefore must not be described as exact for every int64 input.

## CI

GitHub Actions builds and smokes the C runtime with GCC and Clang, exercises the `--audit-stdin` safe/reject contract, checks Node.js source syntax, and builds the WPF UI on Windows. The C smoke path runs the trainer overfit/backbone-update regression and proof receipt smoke checks. CI is the repository-level evidence for buildability; documentation claims are not treated as implementation evidence.
